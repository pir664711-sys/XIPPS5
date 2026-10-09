// emu.cpp - PS5 guest = x86-64, host = x86-64 => run guest code NATIVELY (no CPU translation).
// Work = loader + OS/lib HLE ("API") + GPU (stub: software framebuffer window).
// Guest uses SysV ABI, host (Windows) uses MS ABI => runtime-generated thunks both ways.
//
// 0.018: unresolved-import caller diagnostics to identify initialization loops and missing HLE APIs.
// 0.017: mutex attributes, virtual-query HLE, memory-map argument guards, clearer API diagnostics.
// 0.016: sceKernelReserveVirtualRange, threads (pthread_create/join), condvars, semaphores, event flags.
// 0.015: NID import resolution, Orbis entry ABI, TLS (fs: -> per-thread TCB), fault capture on the guest thread,
//        first batch of libc / libkernel HLE, import table dump (ps5emu.imports.txt).
#include "emu.h"
#include "nid.h"
#include "overlay.h"
#include "perf.h"
#include "vk_backend.h"
#include <windows.h>
#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN64

#define FB_W 1280
#define FB_H 720

namespace {

struct Ehdr { uint8_t id[16]; uint16_t type, machine; uint32_t ver; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct Dyn  { int64_t tag; uint64_t val; };
struct Sym  { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct Rela { uint64_t offset, info; int64_t addend; };

// table handed to guest entry point (rdi) for the legacy test guest. Layout must match tools/guest.c
struct GuestHle { uint32_t* fb; int32_t w, h; void* flip; void* log; };

// an import that no HLE function covers: the guest calls a stub that logs + returns 0
struct UnresInfo { std::string raw, display; std::atomic<uint64_t> calls{0}; std::atomic<uintptr_t> firstCaller{0}; };

struct TlsInfo { bool present = false; uint64_t vaddr = 0, filesz = 0, memsz = 0, align = 0; };

std::atomic<int>  g_state{Emu::Stopped};
std::atomic<bool> g_quit{false};
HWND     g_wnd   = nullptr;
HANDLE   g_guest = nullptr;
uint8_t* g_img   = nullptr;
size_t   g_imgSize = 0;
uint8_t* g_bias  = nullptr;
int      g_nRes = 0, g_nUnres = 0;
uint8_t* g_pool  = nullptr;
size_t   g_poolUsed = 0;
const size_t POOL_SIZE = 1 << 20;
uint8_t* g_tramp = nullptr;              // TLS patch trampolines (must be within +-2GB of the guest image)
size_t   g_trampUsed = 0;
const size_t TRAMP_SIZE = 1 << 18;
std::map<std::string, UnresInfo> g_unres;
std::map<std::string, void*>     g_resolved;      // raw import name -> thunk / data address
struct ImportRec { std::string raw, nid, status, name; };
std::vector<ImportRec> g_imports;
std::atomic<UnresInfo*> g_lastUnres{nullptr};
uint32_t g_fb[FB_W * FB_H];
uint32_t g_pres[FB_W * FB_H];            // software path: fb + overlay copy
const uint32_t* g_show = g_fb;           // what WM_PAINT draws in software mode
int  g_gpuMode = 1;                      // 1 = Vulkan, 0 = software
bool g_vsync = true, g_overlay = true;
VkPresenter g_vk;
std::atomic<bool> g_vkOk{false};
std::wstring g_title;

bool     g_orbis = false;                // SCE ELF types (0xFE00 / 0xFE10 / 0xFE18) -> real console entry ABI
TlsInfo  g_tls;
DWORD    g_tlsIdx = TLS_OUT_OF_INDEXES;  // host TLS slot that holds the guest thread pointer (read through gs:[0x1480+8*idx])
uint64_t g_stackGuard = 0x5A17C0DE9E3779B9ull;
thread_local bool t_isGuest = false;
std::atomic<bool> g_faulted{false};
LARGE_INTEGER g_qpcFreq, g_qpc0;

std::wstring ExeDirW() {
  wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p; return s.substr(0, s.find_last_of(L'\\') + 1);
}
uint64_t AlignUp(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

// on-screen "FPS 60.0 CPU 12% GPU 8% VK"
void OverlayPost(uint32_t* px, int w, int h) {
  PerfStats s = Perf::Get();
  char gpu[16]; if (s.gpu < 0) snprintf(gpu, sizeof gpu, "--"); else snprintf(gpu, sizeof gpu, "%.0f%%", s.gpu);
  char t[96]; snprintf(t, sizeof t, "FPS %.1f CPU %.0f%% GPU %s %s", s.fps, s.cpu, gpu, g_vkOk ? "VK" : "SW");
  OverlayDrawText(px, w, h, 12, 12, 3, t, 0xFF40FF60u, 0xFF101010u);
}
GuestHle g_hle;

// ---------------------------------------------------------------- thunks
void* PoolPut(const uint8_t* code, size_t n) {
  static std::mutex mx; std::lock_guard<std::mutex> lk(mx);   // guest threads create thunks too
  if (!g_pool) g_pool = (uint8_t*)VirtualAlloc(nullptr, POOL_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!g_pool || g_poolUsed + n > POOL_SIZE) return nullptr;
  uint8_t* p = g_pool + g_poolUsed;
  memcpy(p, code, n);
  g_poolUsed = (g_poolUsed + n + 15) & ~(size_t)15;
  return p;
}

// guest(SysV, <=6 int args, float args in xmm0..) -> host fn (MS ABI). 5th/6th args go to the MS stack slots.
void* ThunkToHost(void* fn) {
  uint8_t c[] = {
    0x48,0x83,0xEC,0x38,        // sub rsp,38h
    0x4C,0x89,0x44,0x24,0x20,   // mov [rsp+20h],r8   (arg5)
    0x4C,0x89,0x4C,0x24,0x28,   // mov [rsp+28h],r9   (arg6)
    0x49,0x89,0xC9,             // mov r9,rcx
    0x49,0x89,0xD0,             // mov r8,rdx
    0x48,0x89,0xF2,             // mov rdx,rsi
    0x48,0x89,0xF9,             // mov rcx,rdi
    0x48,0xB8, 0,0,0,0,0,0,0,0, // mov rax,fn
    0xFF,0xD0,                  // call rax
    0x48,0x83,0xC4,0x38,        // add rsp,38h
    0xC3 };
  memcpy(c + 28, &fn, 8);
  return PoolPut(c, sizeof c);
}

// guest(SysV) -> host fn(void* arg) : for unresolved imports (arg = UnresInfo*)
void* ThunkUnresolved(void* fn, void* arg) {
  // Preserve the guest call-site return address as arg2 for diagnostics. At thunk
  // entry [rsp] is the guest return address; after sub rsp,28h it is [rsp+28h].
  uint8_t c[] = {
    0x48,0x83,0xEC,0x28,        // sub rsp,28h (Windows shadow space + alignment)
    0x48,0x8B,0x54,0x24,0x28,  // mov rdx,[rsp+28h] (guest caller return address)
    0x48,0xB9, 0,0,0,0,0,0,0,0, // mov rcx,arg (UnresInfo*)
    0x48,0xB8, 0,0,0,0,0,0,0,0, // mov rax,fn
    0xFF,0xD0,                  // call rax
    0x48,0x83,0xC4,0x28,
    0xC3 };
  memcpy(c + 11, &arg, 8);
  memcpy(c + 21, &fn, 8);
  return PoolPut(c, sizeof c);
}

// host(MS) -> guest entry(SysV): (rcx,rdx) -> (rdi,rsi), preserves rdi/rsi
void* ThunkToGuest(void* fn) {
  uint8_t c[] = {
    0x57, 0x56,                 // push rdi; push rsi
    0x48,0x83,0xEC,0x28,        // sub rsp,28h
    0x48,0x89,0xCF,             // mov rdi,rcx
    0x48,0x89,0xD6,             // mov rsi,rdx
    0x48,0xB8, 0,0,0,0,0,0,0,0, // mov rax,fn
    0xFF,0xD0,                  // call rax
    0x48,0x83,0xC4,0x28,
    0x5E, 0x5F, 0xC3 };
  memcpy(c + 14, &fn, 8);
  return PoolPut(c, sizeof c);
}

// allocate executable memory within +-2GB of [lo, hi) so rel32 jumps reach it
uint8_t* AllocNear(const uint8_t* lo, const uint8_t* hi, size_t size) {
  const int64_t kSpan = 0x70000000, kStep = 0x10000;
  for (int64_t d = 0x100000; d < kSpan; d += kStep) {
    uintptr_t hiA = ((uintptr_t)hi + d) & ~(uintptr_t)0xFFFF;
    void* p = VirtualAlloc((void*)hiA, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (p) return (uint8_t*)p;
    if ((uintptr_t)lo > (uintptr_t)d + size + 0x10000) {
      uintptr_t loA = ((uintptr_t)lo - d - size) & ~(uintptr_t)0xFFFF;
      p = VirtualAlloc((void*)loA, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (p) return (uint8_t*)p;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- TLS
// Windows has no usable fs: base in user mode, but the guest reads its thread pointer with
//     64 48 8B /r 25 disp32      mov r64, fs:[disp32]
// We rewrite every such 9-byte load into "jmp trampoline; nop x4". The trampoline does
//     mov r, gs:[TEB.TlsSlots + 8*idx]   ; per-thread guest TCB pointer (TlsSetValue)
//     mov r, [r + disp32]
//     jmp back
// so fs:[0] returns the TCB self pointer, fs:[0x28] the stack-guard value, etc.
bool PatchFsLoads(const Phdr* ph, int phnum, const std::vector<char>& skip, int& patched, int& failed) {
  patched = failed = 0;
  if (g_tlsIdx == TLS_OUT_OF_INDEXES || g_tlsIdx >= 64) return false;
  const uint32_t slotOff = 0x1480 + 8 * g_tlsIdx;
  for (int i = 0; i < phnum; i++) {
    if (ph[i].type != 1 || !(ph[i].flags & 1) || skip[i]) continue;
    uint8_t* d = g_bias + ph[i].vaddr;
    for (uint64_t k = 0; k + 9 <= ph[i].filesz; k++) {
      uint8_t* p = d + k;
      if (!(p[0] == 0x64 && (p[1] & 0xFB) == 0x48 && p[2] == 0x8B && (p[3] & 0xC7) == 0x04 && p[4] == 0x25)) continue;
      if (!g_tramp) {
        g_tramp = AllocNear(g_img, g_img + g_imgSize, TRAMP_SIZE); g_trampUsed = 0;
        if (!g_tramp) { Log("TLS patch: no executable memory within +-2GB of the guest image"); return false; }
      }
      uint8_t reg = ((p[3] >> 3) & 7) | ((p[1] & 4) ? 8 : 0);
      int32_t disp; memcpy(&disp, p + 5, 4);
      uint8_t t[32]; size_t n = 0;
      t[n++] = 0x65; t[n++] = 0x48 | ((reg >> 3) << 2); t[n++] = 0x8B; t[n++] = ((reg & 7) << 3) | 0x04; t[n++] = 0x25;
      memcpy(t + n, &slotOff, 4); n += 4;                                  // mov reg, gs:[slotOff]
      uint8_t r3 = reg & 7, rx = reg >> 3;
      t[n++] = 0x48 | (rx << 2) | rx; t[n++] = 0x8B; t[n++] = 0x80 | (r3 << 3) | r3;
      if (r3 == 4) t[n++] = 0x24;
      memcpy(t + n, &disp, 4); n += 4;                                     // mov reg, [reg+disp]
      t[n++] = 0xE9;
      uint8_t* tr = g_tramp + g_trampUsed;
      int64_t back = (int64_t)(p + 9) - (int64_t)(tr + n + 4);
      int64_t fwd  = (int64_t)tr - (int64_t)(p + 5);
      if (g_trampUsed + n + 4 > TRAMP_SIZE || back != (int32_t)back || fwd != (int32_t)fwd) { failed++; continue; }
      int32_t b32 = (int32_t)back, f32 = (int32_t)fwd;
      memcpy(t + n, &b32, 4); n += 4;
      memcpy(tr, t, n); g_trampUsed = (g_trampUsed + n + 15) & ~(size_t)15;
      p[0] = 0xE9; memcpy(p + 1, &f32, 4); p[5] = p[6] = p[7] = p[8] = 0x90;
      patched++; k += 8;
    }
  }
  return patched > 0;
}

// per guest thread: TCB (self pointer at +0, guard at +0x28) with the PT_TLS image just below it
uint8_t* SetupGuestTcb() {
  if (g_tlsIdx == TLS_OUT_OF_INDEXES) return nullptr;
  const size_t TOTAL = 2u << 20, TPOFF = 1u << 20;
  uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, TOTAL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!base) return nullptr;
  uint8_t* tp = base + TPOFF;
  if (g_tls.present) {
    size_t sz = (size_t)AlignUp(g_tls.memsz, g_tls.align ? g_tls.align : 16);
    if (sz < TPOFF && g_tls.filesz <= sz) memcpy(tp - sz, g_bias + g_tls.vaddr, (size_t)g_tls.filesz);
  }
  ((uint64_t*)tp)[0] = (uint64_t)tp;
  ((uint64_t*)tp)[5] = g_stackGuard;
  TlsSetValue(g_tlsIdx, tp);
  return tp;
}

// ---------------------------------------------------------------- HLE "API" (MS ABI host side)
std::atomic<uint64_t> g_flips{0}; int g_hb = 0;
uint64_t H_Flip() {
  g_flips++;
  if (g_vkOk) {   // Vulkan: upload + scaled blit + present (paced by vsync when enabled)
    if (!g_vk.Present(g_fb, FB_W, FB_H, g_overlay ? OverlayPost : nullptr)) {
      Log("Vulkan present failed -> switching to software rendering");
      g_vkOk = false; Perf::SetGpuDesc("Software (Vulkan failed)");
    }
  }
  if (!g_vkOk) {  // software fallback: GDI paint on the window thread
    const uint32_t* src = g_fb;
    if (g_overlay) { memcpy(g_pres, g_fb, sizeof g_pres); OverlayPost(g_pres, FB_W, FB_H); src = g_pres; }
    g_show = src;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
    Sleep(16);
  }
  return g_quit ? 1 : 0;
}
uint64_t H_Log(const char* s){ Log("[guest] %s", s); return 0; }
uint64_t H_Puts(const char* s){ Log("%s", s ? s : "(null)"); return 1; }
uint64_t H_Exit(int code)    { Log("guest exit(%d)", code); g_quit = true; if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0); ExitThread(0); return 0; }
uint64_t H_Abort()           { Log("guest abort()"); g_quit = true; if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0); ExitThread(1); return 0; }
uint64_t H_StackChkFail()    { Log("guest __stack_chk_fail: stack smashing detected (or the guard value is wrong)"); return H_Abort(); }
uint64_t H_PureVirtual()     { Log("guest __cxa_pure_virtual: call through a null vtable slot"); return H_Abort(); }
uint64_t H_Nop()             { return 0; }
uint64_t H_Usleep(unsigned us){ Sleep(us / 1000 ? us / 1000 : 1); return 0; }
// ---- guest printf family -------------------------------------------------------------
// SysV x86-64 va_list as the guest builds it. vsnprintf() gets a pointer to one of these. For the
// non-v variants we synthesise one from the integer argument registers the host thunk forwards
// (rdi,rsi,rdx,rcx,r8,r9). Limitation: the thunk does not forward xmm registers or stack args, so
// snprintf("%f", x) / more than 6 total args read as 0 / 0.0; vsnprintf is complete.
struct SysVVaList { uint32_t gp_offset, fp_offset; uint8_t* overflow; uint8_t* reg_save; };
static uint64_t VaInt(SysVVaList* v) {
  uint64_t r = 0;
  if (v->gp_offset < 48) { memcpy(&r, v->reg_save + v->gp_offset, 8); v->gp_offset += 8; }
  else if (v->overflow) { memcpy(&r, v->overflow, 8); v->overflow += 8; }
  return r;
}
static double VaDouble(SysVVaList* v) {
  double r = 0;
  if (v->fp_offset < 176) { memcpy(&r, v->reg_save + v->fp_offset, 8); v->fp_offset += 16; }
  else if (v->overflow) { memcpy(&r, v->overflow, 8); v->overflow += 8; }
  return r;
}
template <class T> static void FmtOne(std::string& out, const std::string& spec, T val) {
  int n = snprintf(nullptr, 0, spec.c_str(), val);
  if (n <= 0) return;
  size_t at = out.size(); out.resize(at + (size_t)n + 1);
  snprintf(&out[at], (size_t)n + 1, spec.c_str(), val);
  out.resize(at + (size_t)n);
}
static std::string GuestFormat(const char* f, SysVVaList* va) {
  std::string out; if (!f) return out;
  while (*f) {
    if (*f != '%') { out += *f++; continue; }
    const char* start = f++;
    if (*f == '%') { out += '%'; f++; continue; }
    std::string spec = "%";
    while (*f && strchr("-+ #0", *f)) spec += *f++;
    if (*f == '*') { spec += std::to_string((int)VaInt(va)); f++; } else while (*f >= '0' && *f <= '9') spec += *f++;
    if (*f == '.') {
      spec += *f++;
      if (*f == '*') { spec += std::to_string((int)VaInt(va)); f++; } else while (*f >= '0' && *f <= '9') spec += *f++;
    }
    bool is64 = false;
    while (*f && strchr("hlqjztL", *f)) { if (*f != 'h' && *f != 'L') is64 = true; f++; }   // guest long == 64-bit
    char c = *f; if (!c) { out.append(start); break; }
    f++;
    switch (c) {
      case 'd': case 'i': { uint64_t v = VaInt(va); if (is64) FmtOne(out, spec + "lld", (long long)v); else FmtOne(out, spec + "d", (int)v); break; }
      case 'u': case 'x': case 'X': case 'o': {
        uint64_t v = VaInt(va); std::string cv(1, c);
        if (is64) FmtOne(out, spec + "ll" + cv, (unsigned long long)v); else FmtOne(out, spec + cv, (unsigned)v); break; }
      case 'c': FmtOne(out, spec + "c", (int)VaInt(va)); break;
      case 's': { const char* sp = (const char*)VaInt(va); FmtOne(out, spec + "s", sp ? sp : "(null)"); break; }
      case 'p': { uint64_t v = VaInt(va); FmtOne(out, std::string("0x%llx"), (unsigned long long)v); break; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': FmtOne(out, spec + c, VaDouble(va)); break;
      case 'n': (void)VaInt(va); break;
      default: out.append(start, (size_t)(f - start)); break;
    }
  }
  return out;
}
static int GuestSnprintf(char* b, size_t n, const char* fmt, SysVVaList* va) {
  std::string s = GuestFormat(fmt, va);
  if (b && n) { size_t c = s.size() < n - 1 ? s.size() : n - 1; memcpy(b, s.data(), c); b[c] = 0; }
  return (int)s.size();
}
// register-passed variadics: `fixed` = number of named integer args already consumed
struct RegVa { uint8_t save[176]; SysVVaList va; RegVa(int fixed, const uint64_t* a, int n) { memset(save, 0, sizeof save);
  for (int i = 0; i < n && fixed + i < 6; i++) memcpy(save + 8 * (fixed + i), &a[i], 8); va = { (uint32_t)(8 * fixed), 176, nullptr, save }; } };
int      H_Vsnprintf(char* b, size_t n, const char* fmt, SysVVaList* va) { return va ? GuestSnprintf(b, n, fmt, va) : 0; }
int      H_Snprintf(char* b, size_t n, const char* fmt, uint64_t a4, uint64_t a5, uint64_t a6) { uint64_t a[] = {a4, a5, a6}; RegVa r(3, a, 3); return GuestSnprintf(b, n, fmt, &r.va); }
int      H_Sprintf(char* b, const char* fmt, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) { uint64_t a[] = {a3, a4, a5, a6}; RegVa r(2, a, 4); return GuestSnprintf(b, (size_t)-1 >> 1, fmt, &r.va); }
uint64_t H_Printf(const char* fmt, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
  uint64_t a[] = {a2, a3, a4, a5, a6}; RegVa r(1, a, 5);
  std::string s = GuestFormat(fmt, &r.va);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  Log("[guest printf] %s", s.c_str());
  return s.size();
}

// libc memory: one aligned heap so free()/realloc() work for memalign() blocks too
void*    H_Malloc(size_t n)  { return _aligned_malloc(n ? n : 1, 16); }
void     H_Free(void* p)     { _aligned_free(p); }
void*    H_Calloc(size_t a, size_t b) { size_t n = a * b; void* p = _aligned_malloc(n ? n : 1, 16); if (p) memset(p, 0, n); return p; }
void*    H_Realloc(void* p, size_t n) { return _aligned_realloc(p, n ? n : 1, 16); }
void*    H_Memalign(size_t al, size_t n) { if (al < 16) al = 16; return _aligned_malloc(n ? n : 1, al); }
void*    H_Memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
void*    H_Memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
void*    H_Memset(void* d, int c, size_t n)         { return memset(d, c, n); }
int      H_Memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
void*    H_Memchr(const void* a, int c, size_t n) { return (void*)memchr(a, c, n); }
size_t   H_Strlen(const char* s) { return strlen(s); }
char*    H_Strcpy(char* d, const char* s) { return strcpy(d, s); }
char*    H_Strncpy(char* d, const char* s, size_t n) { return strncpy(d, s, n); }
char*    H_Strcat(char* d, const char* s) { return strcat(d, s); }
char*    H_Strncat(char* d, const char* s, size_t n) { return strncat(d, s, n); }
int      H_Strcmp(const char* a, const char* b) { return strcmp(a, b); }
int      H_Strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
char*    H_Strchr(const char* s, int c) { return (char*)strchr(s, c); }
char*    H_Strrchr(const char* s, int c) { return (char*)strrchr(s, c); }
char*    H_Strstr(const char* a, const char* b) { return (char*)strstr(a, b); }
// C++ runtime
int      H_AtExit(void*)               { return 0; }          // handlers are never run (guest never exits cleanly yet)
int      H_CxaAtExit(void*, void*, void*) { return 0; }
int      H_GuardAcquire(uint8_t* g)    { return *g ? 0 : 1; }
void     H_GuardRelease(uint8_t* g)    { *g = 1; }
// math (floats / doubles are passed in xmm0/xmm1 in both ABIs)
float    H_Sinf(float x) { return sinf(x); }   float H_Cosf(float x) { return cosf(x); }   float H_Tanf(float x) { return tanf(x); }
float    H_Powf(float a, float b) { return powf(a, b); }   float H_Atan2f(float a, float b) { return atan2f(a, b); }
float    H_Fmodf(float a, float b) { return fmodf(a, b); } float H_Expf(float x) { return expf(x); }   float H_Logf(float x) { return logf(x); }
double   H_Sin(double x) { return sin(x); }    double H_Cos(double x) { return cos(x); }    double H_Tan(double x) { return tan(x); }
double   H_Pow(double a, double b) { return pow(a, b); }   double H_Fmod(double a, double b) { return fmod(a, b); }
double   H_Exp(double x) { return exp(x); }    double H_LogD(double x) { return log(x); }

// time
uint64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
uint64_t H_ProcTime()        { return (QpcNow() - (uint64_t)g_qpc0.QuadPart) * 1000000ull / (uint64_t)g_qpcFreq.QuadPart; }
uint64_t H_ProcTimeCounter() { return QpcNow(); }
uint64_t H_ProcTimeFreq()    { return (uint64_t)g_qpcFreq.QuadPart; }
void UnixNow(int64_t& sec, int64_t& nsec) {
  FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
  sec = (int64_t)(t / 10000000ull); nsec = (int64_t)(t % 10000000ull) * 100;
}
int H_ClockGettime(int clk, int64_t* ts) {
  if (!ts) return 22;
  if (clk == 0) { UnixNow(ts[0], ts[1]); return 0; }
  uint64_t q = QpcNow(), f = (uint64_t)g_qpcFreq.QuadPart;
  ts[0] = (int64_t)(q / f); ts[1] = (int64_t)((q % f) * 1000000000ull / f); return 0;
}
int H_Gettimeofday(int64_t* tv, void*) { if (!tv) return 0; int64_t s, n; UnixNow(s, n); tv[0] = s; tv[1] = n / 1000; return 0; }
int64_t H_Time(int64_t* t) { int64_t s, n; UnixNow(s, n); if (t) *t = s; return s; }

// kernel memory. "Direct memory" is faked: allocate hands out offsets, map gives fresh zeroed host memory.
uint64_t g_dmemNext = 0;
int H_AllocMainDirectMem(size_t len, size_t al, int, int64_t* out) {
  if (!al) al = 0x10000;
  g_dmemNext = AlignUp(g_dmemNext, al); if (out) *out = (int64_t)g_dmemNext; g_dmemNext += len; return 0;
}
int H_AllocDirectMem(int64_t, int64_t, size_t len, size_t al, int type, int64_t* out) { return H_AllocMainDirectMem(len, al, type, out); }
// reserve address space aligned to `align` (Windows only guarantees 64 KB)
void* ReserveAligned(size_t len, size_t align) {
  if (align <= 0x10000) return VirtualAlloc(nullptr, len, MEM_RESERVE, PAGE_NOACCESS);
  for (int i = 0; i < 16; i++) {
    void* b = VirtualAlloc(nullptr, len + align, MEM_RESERVE, PAGE_NOACCESS);
    if (!b) return nullptr;
    uintptr_t a = (uintptr_t)AlignUp((uint64_t)(uintptr_t)b, align);
    VirtualFree(b, 0, MEM_RELEASE);
    void* p = VirtualAlloc((void*)a, len, MEM_RESERVE, PAGE_NOACCESS);
    if (p) return p;
  }
  return nullptr;
}
// sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align): address space only, no access until mapped
int H_ReserveVirtualRange(void** addr, size_t len, int flags, size_t align) {
  if (!addr || !len) return 0x80020016;
  uintptr_t want = (uintptr_t)*addr;
  void* p = nullptr;
  if (want) {
    uintptr_t b = want & ~(uintptr_t)0xFFFF;
    if (VirtualAlloc((void*)b, len + (want - b), MEM_RESERVE, PAGE_NOACCESS)) p = (void*)want;
  }
  if (!p && !(want && (flags & 0x10))) p = ReserveAligned(len, align);
  if (!p) { Log("guest sceKernelReserveVirtualRange(%p, 0x%llx) failed (err %lu)", (void*)want, (unsigned long long)len, GetLastError()); return 0x8002000C; }
  *addr = p; return 0;
}
// sceKernelMapDirectMemory(void** addr, len, prot, flags, phys, align): commits inside a reserved range, else allocates fresh
int H_MapDirectMem(void** addr, size_t len, int, int flags, int64_t, size_t align) {
  // A malformed import thunk or unsupported ABI can otherwise turn a signed value into
  // a multi-exabyte allocation request. Reject implausible sizes before calling VirtualAlloc.
  if (!addr || !len || len > 0x80000000ull) {
    Log("guest sceKernelMapDirectMemory: invalid arguments (addr=%p, len=0x%llx)",
        (void*)addr, (unsigned long long)len);
    return 0x80020016;   // SCE_KERNEL_ERROR_EINVAL
  }
  uintptr_t want = (uintptr_t)*addr;
  void* p = nullptr;
  if (want) {
    p = VirtualAlloc((void*)want, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE);              // inside a range we reserved
    if (!p) { uintptr_t b = want & ~(uintptr_t)0xFFFF;
              if (VirtualAlloc((void*)b, len + (want - b), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) p = (void*)want; }
  }
  if (!p && !(want && (flags & 0x10))) {
    void* r = ReserveAligned(len, align);
    if (r) p = VirtualAlloc(r, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  }
  if (!p) { Log("guest sceKernelMapDirectMemory(%p, 0x%llx) failed (err %lu)", (void*)want, (unsigned long long)len, GetLastError()); return 0x8002000C; }
  *addr = p; return 0;
}

// sceKernelVirtualQuery(address, flags, info, infoSize).
// This compatibility structure follows the common Orbis/Kyty layout (72 bytes). The
// values describe the host-backed region used by this emulator, not physical PS5 memory.
struct GuestVirtualQueryInfo {
  uint64_t start;
  uint64_t end;
  uint64_t offset;
  int32_t protection;
  int32_t memory_type;
  uint32_t flags;
  char name[32];
  uint8_t gpu_mask_id;
  uint8_t reserved2;
};
static_assert(sizeof(GuestVirtualQueryInfo) == 72, "virtual query layout must stay 72 bytes");
int H_VirtualQuery(const void* address, int, GuestVirtualQueryInfo* out, size_t outSize) {
  if (!address || !out || outSize < sizeof(GuestVirtualQueryInfo)) return 0x80020016;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(address, &mbi, sizeof(mbi))) return 0x8002000B; // address not mapped
  memset(out, 0, sizeof(*out));
  out->start = (uint64_t)(uintptr_t)mbi.BaseAddress;
  out->end = out->start + (uint64_t)mbi.RegionSize;
  out->offset = 0;
  out->memory_type = (int32_t)mbi.Type;
  out->flags = (mbi.State == MEM_COMMIT) ? (1u << 4) : 0u; // is_committed bit
  if (mbi.State == MEM_COMMIT) {
    switch (mbi.Protect & 0xFF) {
      case PAGE_READONLY: out->protection = 1; break;
      case PAGE_READWRITE: case PAGE_WRITECOPY: out->protection = 3; break;
      case PAGE_EXECUTE: out->protection = 4; break;
      case PAGE_EXECUTE_READ: out->protection = 5; break;
      case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: out->protection = 7; break;
      default: out->protection = 0; break;
    }
  }
  return 0;
}
int      H_ReleaseDirectMem(int64_t, size_t) { return 0; }
uint64_t H_GetDirectMemSize() { return 0x180000000ull; }

// pthread-ish (sce + posix names share these). A mutex slot is a pointer to a lazily created CRITICAL_SECTION.
CRITICAL_SECTION* GetMutex(void** m) {
  CRITICAL_SECTION* cs = (CRITICAL_SECTION*)*(void* volatile*)m;
  if (!cs) {
    CRITICAL_SECTION* n = new CRITICAL_SECTION; InitializeCriticalSection(n);
    void* old = InterlockedCompareExchangePointer((PVOID volatile*)m, n, nullptr);
    if (old) { DeleteCriticalSection(n); delete n; cs = (CRITICAL_SECTION*)old; } else cs = n;
  }
  return cs;
}
// PS4/PS5 pthread mutex attributes are caller-owned opaque storage. Store the type
// in the first word; CRITICAL_SECTION is recursive, matching the recursive type.
int H_MutexAttrInit(void* attr) { if (!attr) return 22; *(int32_t*)attr = 0; return 0; }
int H_MutexAttrSetType(void* attr, int type) { if (!attr) return 22; *(int32_t*)attr = type; return 0; }
int H_MutexAttrDestroy(void* attr) { if (!attr) return 22; *(int32_t*)attr = 0; return 0; }
int H_MutexInit(void** m, void*, const char*) { if (!m) return 22; *m = nullptr; GetMutex(m); return 0; }
int H_MutexLock(void** m)    { if (!m) return 22; EnterCriticalSection(GetMutex(m)); return 0; }
int H_MutexUnlock(void** m)  { if (!m) return 22; LeaveCriticalSection(GetMutex(m)); return 0; }
int H_MutexTrylock(void** m) { if (!m) return 22; return TryEnterCriticalSection(GetMutex(m)) ? 0 : (int)0x80020010; }
int H_MutexDestroy(void** m) { if (!m || !*m) return 0; CRITICAL_SECTION* cs = (CRITICAL_SECTION*)*m; *m = nullptr; DeleteCriticalSection(cs); delete cs; return 0; }
thread_local void* t_keys[256];
std::atomic<int> g_nKeys{0};
int   H_KeyCreate(uint32_t* key, void*) { int k = g_nKeys++; if (k >= 256) return 0x8002000C; if (key) *key = (uint32_t)k; return 0; }
void* H_GetSpecific(uint32_t k) { return k < 256 ? t_keys[k] : nullptr; }
int   H_SetSpecific(uint32_t k, void* v) { if (k < 256) t_keys[k] = v; return 0; }
uint64_t H_PthreadSelf() { return (uint64_t)GetCurrentThreadId(); }
uint64_t H_PthreadYield() { SwitchToThread(); return 0; }

// condition variables: same lazy-pointer scheme as mutexes (our mutexes are CRITICAL_SECTIONs)
CONDITION_VARIABLE* GetCond(void** c) {
  CONDITION_VARIABLE* cv = (CONDITION_VARIABLE*)*(void* volatile*)c;
  if (!cv) {
    CONDITION_VARIABLE* n = new CONDITION_VARIABLE; InitializeConditionVariable(n);
    void* old = InterlockedCompareExchangePointer((PVOID volatile*)c, n, nullptr);
    if (old) { delete n; cv = (CONDITION_VARIABLE*)old; } else cv = n;
  }
  return cv;
}
int H_CondInit(void** c, void*)       { if (!c) return 22; *c = nullptr; GetCond(c); return 0; }
int H_CondWait(void** c, void** m)    { if (!c || !m) return 22; SleepConditionVariableCS(GetCond(c), GetMutex(m), INFINITE); return 0; }
int H_CondSignal(void** c)            { if (!c) return 22; WakeConditionVariable(GetCond(c)); return 0; }
int H_CondBroadcast(void** c)         { if (!c) return 22; WakeAllConditionVariable(GetCond(c)); return 0; }

// kernel objects (semaphores, event flags) are handed to the guest as 32-bit ids
std::mutex g_kmx;
std::vector<void*> g_kobj;
int   KPut(void* o)  { std::lock_guard<std::mutex> l(g_kmx); g_kobj.push_back(o); return (int)g_kobj.size() - 1 + 0x1000; }
void* KGet(int id)   { std::lock_guard<std::mutex> l(g_kmx); int i = id - 0x1000; return (i >= 0 && i < (int)g_kobj.size()) ? g_kobj[i] : nullptr; }
void  KClear(int id) { std::lock_guard<std::mutex> l(g_kmx); int i = id - 0x1000; if (i >= 0 && i < (int)g_kobj.size()) g_kobj[i] = nullptr; }
const int SCE_ENOENT = 0x80020002;
const int SCE_EBADF = 0x80020009, SCE_EBUSY = 0x80020010, SCE_ETIMEDOUT = 0x8002003C, SCE_EINVAL = 0x80020016, SCE_ENOMEM = 0x8002000C;

int H_CreateSema(int32_t* sem, const char*, uint32_t, int init, int max, void*) {
  HANDLE h = CreateSemaphoreW(nullptr, init, max > 0 ? max : 0x7fffffff, nullptr);
  if (!h) return SCE_ENOMEM;
  if (sem) *sem = KPut(h);
  return 0;
}
int H_WaitSema(int id, int need, uint32_t* timeoutUs) {
  HANDLE h = (HANDLE)KGet(id); if (!h) return SCE_EBADF;
  DWORD ms = timeoutUs ? (*timeoutUs + 999) / 1000 : INFINITE;
  for (int i = 0; i < need; i++)
    if (WaitForSingleObject(h, ms) != WAIT_OBJECT_0) { if (i) ReleaseSemaphore(h, i, nullptr); return SCE_ETIMEDOUT; }
  return 0;
}
int H_PollSema(int id, int need) {
  HANDLE h = (HANDLE)KGet(id); if (!h) return SCE_EBADF;
  for (int i = 0; i < need; i++)
    if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) { if (i) ReleaseSemaphore(h, i, nullptr); return SCE_EBUSY; }
  return 0;
}
int H_SignalSema(int id, int count) { HANDLE h = (HANDLE)KGet(id); if (!h) return SCE_EBADF; ReleaseSemaphore(h, count, nullptr); return 0; }
int H_DeleteSema(int id)            { HANDLE h = (HANDLE)KGet(id); if (!h) return SCE_EBADF; KClear(id); CloseHandle(h); return 0; }

struct EvFlag { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; uint64_t bits; };
int H_CreateEvf(int32_t* out, const char*, uint32_t, uint64_t init, void*) {
  EvFlag* e = new EvFlag; InitializeCriticalSection(&e->cs); InitializeConditionVariable(&e->cv); e->bits = init;
  if (out) *out = KPut(e);
  return 0;
}
int H_SetEvf(int id, uint64_t bits) {
  EvFlag* e = (EvFlag*)KGet(id); if (!e) return SCE_EBADF;
  EnterCriticalSection(&e->cs); e->bits |= bits; LeaveCriticalSection(&e->cs); WakeAllConditionVariable(&e->cv); return 0;
}
// mode: 0x01 = all bits (AND), 0x02 = any bit (OR), 0x10 = clear everything on success, 0x20 = clear the waited bits
int H_WaitEvf(int id, uint64_t pat, uint32_t mode, uint64_t* res, uint32_t* timeoutUs) {
  EvFlag* e = (EvFlag*)KGet(id); if (!e) return SCE_EBADF;
  ULONGLONG deadline = timeoutUs ? GetTickCount64() + (*timeoutUs + 999) / 1000 : 0;
  int rc = 0;
  EnterCriticalSection(&e->cs);
  for (;;) {
    bool ok = (mode & 1) ? ((e->bits & pat) == pat) : ((e->bits & pat) != 0);
    if (ok) { if (res) *res = e->bits; if (mode & 0x10) e->bits = 0; else if (mode & 0x20) e->bits &= ~pat; break; }
    DWORD ms = INFINITE;
    if (timeoutUs) { ULONGLONG now = GetTickCount64(); if (now >= deadline) { if (res) *res = e->bits; rc = SCE_ETIMEDOUT; break; } ms = (DWORD)(deadline - now); }
    SleepConditionVariableCS(&e->cv, &e->cs, ms);
  }
  LeaveCriticalSection(&e->cs);
  return rc;
}
int H_DeleteEvf(int id) { EvFlag* e = (EvFlag*)KGet(id); if (!e) return SCE_EBADF; KClear(id); return 0; }   // object leaks on purpose: waiters may still hold it

// threads: a guest pthread is a host thread that runs the guest entry through a host->guest thunk
struct PthAttr { size_t stack = 0; };
int H_AttrInit(PthAttr** a)                 { if (!a) return SCE_EINVAL; *a = new PthAttr; return 0; }
int H_AttrDestroy(PthAttr** a)              { if (a && *a) { delete *a; *a = nullptr; } return 0; }
int H_AttrSetStack(PthAttr** a, size_t sz)  { if (!a || !*a) return SCE_EINVAL; (*a)->stack = sz; return 0; }
struct GThread { void* thunk; void* arg; };
std::mutex g_thrMx;
std::vector<HANDLE> g_threads;
DWORD WINAPI GuestThreadProc(void* p) {
  typedef uint64_t (*Fn)(void*, void*);
  GThread t = *(GThread*)p; delete (GThread*)p;
  t_isGuest = true;
  SetupGuestTcb();
  return (DWORD)((Fn)t.thunk)(t.arg, nullptr);
}
int H_PthreadCreate(HANDLE* out, PthAttr** attr, void* entry, void* arg) {
  if (!out || !entry) return SCE_EINVAL;
  size_t stack = (attr && *attr && (*attr)->stack) ? (*attr)->stack : 0;
  if (stack < (1u << 20)) stack = 1u << 20;
  GThread* t = new GThread{ ThunkToGuest(entry), arg };
  HANDLE h = CreateThread(nullptr, stack, GuestThreadProc, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!h) { delete t; return 0x80020023; }   // EAGAIN
  { std::lock_guard<std::mutex> l(g_thrMx); g_threads.push_back(h); }
  *out = h; return 0;
}
int H_PthreadJoin(HANDLE h, void** ret) {
  if (!h) return SCE_EINVAL;
  WaitForSingleObject(h, INFINITE);
  DWORD c = 0; GetExitCodeThread(h, &c);
  if (ret) *ret = (void*)(uintptr_t)c;
  return 0;
}
uint64_t H_PthreadExit(void* v) { ExitThread((DWORD)(uintptr_t)v); return 0; }
void StopGuestThreads() {   // after the main guest thread is gone: don't leave guest threads running in freed memory
  std::vector<HANDLE> v; { std::lock_guard<std::mutex> l(g_thrMx); v.swap(g_threads); }
  for (HANDLE h : v) { if (WaitForSingleObject(h, 300) == WAIT_TIMEOUT) { TerminateThread(h, 0); Log("guest thread force-terminated"); } CloseHandle(h); }
  std::lock_guard<std::mutex> l(g_kmx); g_kobj.clear();
}

// misc services that hand out handles / ids
int H_GetInitialUser(int32_t* id) { if (id) *id = 1; return 0; }
int H_OpenHandle() { return 1; }          // sceVideoOutOpen / scePadOpen / sceAudioOutOpen: any non-negative handle

// host directory of the booted title (the folder holding eboot.bin); /app0 maps here
std::string g_gameRoot;
static bool MapGuestPath(const char* p, std::string& host) {
  if (!p || g_gameRoot.empty()) return false;
  if (strncmp(p, "/app0", 5) != 0 || (p[5] != 0 && p[5] != '/')) return false;
  host = g_gameRoot + (p + 5);
  for (char& ch : host) if (ch == '/') ch = '\\';
  return true;
}
// Orbis `struct stat`: st_mode u16 @8, st_nlink u16 @10, st_size i64 @72, st_blocks i64 @80, st_blksize u32 @88.
// Before this, the stub returned 0 ("exists") with an all-zero buffer, i.e. a file with mode 0.
int H_KernelStat(const char* path, uint8_t* st) {
  static std::atomic<int> logged{0};
  std::string h; WIN32_FILE_ATTRIBUTE_DATA a;
  bool ok = st && MapGuestPath(path, h) && GetFileAttributesExA(h.c_str(), GetFileExInfoStandard, &a);
  if (logged++ < 40) Log("sceKernelStat(\"%s\") -> %s", path ? path : "(null)", ok ? "ok" : "ENOENT");
  if (!ok) return SCE_ENOENT;
  memset(st, 0, 120);
  bool dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  uint16_t mode = dir ? (uint16_t)(040000 | 0755) : (uint16_t)(0100000 | 0644), nlink = 1;
  uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow, blocks = (size + 511) / 512; uint32_t bs = 0x4000;
  memcpy(st + 8, &mode, 2); memcpy(st + 10, &nlink, 2); memcpy(st + 72, &size, 8); memcpy(st + 80, &blocks, 8); memcpy(st + 88, &bs, 4);
  return 0;
}
// sceUserServiceGetLoginUserIdList(int32_t ids[4]): one logged-in user (id 1), the rest -1
int H_GetLoginUserIdList(int32_t* ids) { if (!ids) return SCE_EINVAL; ids[0] = 1; ids[1] = ids[2] = ids[3] = -1; return 0; }
int H_GetUserName(int32_t userId, char* name, size_t size) {
  if (!name || size < 2) return SCE_EINVAL;
  const char* n = "User"; size_t l = strlen(n); if (l > size - 1) l = size - 1; memcpy(name, n, l); name[l] = 0; return 0;
}
// sceAppContentAppParamGetInt(int paramId, int* value). Only the "unset" value 0 is returned for every
// id for now, but the out-param is now written (before it held stack garbage). The id is logged.
int H_AppParamGetInt(int paramId, int32_t* value) {
  static std::atomic<int> logged{0}; if (logged++ < 16) Log("sceAppContentAppParamGetInt(id=%d) -> 0", paramId);
  if (!value) return SCE_EINVAL; *value = 0; return 0;
}
// scePadGetControllerInformation(handle, info*): report "not connected" with all-zero fields instead of garbage
int H_PadGetInfo(int32_t handle, void* info) { if (!info) return SCE_EINVAL; memset(info, 0, 20); return 0; }

static std::string UnresolvedCallsiteBytes(uintptr_t caller) {
  if (!g_img || caller < (uintptr_t)g_img || caller >= (uintptr_t)g_img + g_imgSize) return "outside guest image";
  const size_t off = (size_t)(caller - (uintptr_t)g_img);
  const size_t begin = off > 16 ? off - 16 : 0;
  const size_t end = (off + 12 < g_imgSize) ? off + 12 : g_imgSize;
  char out[256] = {}; size_t used = 0;
  for (size_t i = begin; i < end && used + 5 < sizeof(out); ++i) {
    int n = snprintf(out + used, sizeof(out) - used, "%02X%s", g_img[i], i + 1 < end ? " " : "");
    if (n <= 0) break; used += (size_t)n;
  }
  return std::string(out);
}

uint64_t H_Unresolved(UnresInfo* u, uintptr_t caller) {
  uint64_t n = ++u->calls;
  g_lastUnres = u;
  if (n == 1) {
    u->firstCaller.store(caller);
    Log("unresolved import first called: %s | caller: %s | bytes around return address (-16..+11): %s", u->display.c_str(), EmuDescribeAddr(caller).c_str(), UnresolvedCallsiteBytes(caller).c_str());
  } else if (n <= 3) {
    Log("unresolved import called (#%llu): %s | first caller: %s", (unsigned long long)n,
        u->display.c_str(), EmuDescribeAddr(u->firstCaller.load()).c_str());
  } else if (n == 100 || n == 1000 || n == 10000 || n == 100000) {
    Log("unresolved import %s called %llu times | first caller: %s", u->display.c_str(),
        (unsigned long long)n, EmuDescribeAddr(u->firstCaller.load()).c_str());
  }
  return 0;
}

struct HleEntry { const char* name; void* fn; };
struct HleData  { const char* name; void* addr; };
// Names are hashed to NIDs at start-up, so one table serves plain-name test guests and real NID imports.
const HleEntry kHle[] = {
  // libc
  {"puts", (void*)H_Puts}, {"exit", (void*)H_Exit}, {"_exit", (void*)H_Exit}, {"abort", (void*)H_Abort},
  {"malloc", (void*)H_Malloc}, {"free", (void*)H_Free}, {"calloc", (void*)H_Calloc}, {"realloc", (void*)H_Realloc}, {"memalign", (void*)H_Memalign},
  {"memcpy", (void*)H_Memcpy}, {"memmove", (void*)H_Memmove}, {"memset", (void*)H_Memset}, {"memcmp", (void*)H_Memcmp}, {"memchr", (void*)H_Memchr},
  {"strlen", (void*)H_Strlen}, {"strcpy", (void*)H_Strcpy}, {"strncpy", (void*)H_Strncpy}, {"strcat", (void*)H_Strcat}, {"strncat", (void*)H_Strncat},
  {"strcmp", (void*)H_Strcmp}, {"strncmp", (void*)H_Strncmp}, {"strchr", (void*)H_Strchr}, {"strrchr", (void*)H_Strrchr}, {"strstr", (void*)H_Strstr},
  {"printf", (void*)H_Printf}, {"snprintf", (void*)H_Snprintf}, {"sprintf", (void*)H_Sprintf}, {"vsnprintf", (void*)H_Vsnprintf},
  {"setenv", (void*)H_Nop},
  {"atexit", (void*)H_AtExit}, {"__cxa_atexit", (void*)H_CxaAtExit}, {"__cxa_guard_acquire", (void*)H_GuardAcquire}, {"__cxa_guard_release", (void*)H_GuardRelease},
  {"__cxa_pure_virtual", (void*)H_PureVirtual}, {"__stack_chk_fail", (void*)H_StackChkFail}, {"_init_env", (void*)H_Nop},
  {"sinf", (void*)H_Sinf}, {"cosf", (void*)H_Cosf}, {"tanf", (void*)H_Tanf}, {"powf", (void*)H_Powf}, {"atan2f", (void*)H_Atan2f},
  {"fmodf", (void*)H_Fmodf}, {"expf", (void*)H_Expf}, {"logf", (void*)H_Logf},
  {"sin", (void*)H_Sin}, {"cos", (void*)H_Cos}, {"tan", (void*)H_Tan}, {"pow", (void*)H_Pow}, {"fmod", (void*)H_Fmod}, {"exp", (void*)H_Exp}, {"log", (void*)H_LogD},
  {"time", (void*)H_Time}, {"gettimeofday", (void*)H_Gettimeofday}, {"clock_gettime", (void*)H_ClockGettime},
  // libkernel
  {"sceKernelUsleep", (void*)H_Usleep}, {"sceKernelExit", (void*)H_Exit},
  {"sceKernelGetProcessTime", (void*)H_ProcTime}, {"sceKernelGetProcessTimeCounter", (void*)H_ProcTimeCounter},
  {"sceKernelGetProcessTimeCounterFrequency", (void*)H_ProcTimeFreq}, {"sceKernelClockGettime", (void*)H_ClockGettime},
  {"sceKernelAllocateDirectMemory", (void*)H_AllocDirectMem}, {"sceKernelAllocateMainDirectMemory", (void*)H_AllocMainDirectMem},
  {"sceKernelMapDirectMemory", (void*)H_MapDirectMem}, {"sceKernelReleaseDirectMemory", (void*)H_ReleaseDirectMem},
  {"sceKernelCheckedReleaseDirectMemory", (void*)H_ReleaseDirectMem}, {"sceKernelGetDirectMemorySize", (void*)H_GetDirectMemSize},
  {"scePthreadMutexInit", (void*)H_MutexInit}, {"scePthreadMutexLock", (void*)H_MutexLock}, {"scePthreadMutexUnlock", (void*)H_MutexUnlock},
  {"scePthreadMutexTrylock", (void*)H_MutexTrylock}, {"scePthreadMutexDestroy", (void*)H_MutexDestroy},
  {"scePthreadMutexattrInit", (void*)H_MutexAttrInit}, {"scePthreadMutexattrSettype", (void*)H_MutexAttrSetType},
  {"scePthreadMutexattrDestroy", (void*)H_MutexAttrDestroy},
  {"pthread_mutexattr_init", (void*)H_MutexAttrInit}, {"pthread_mutexattr_settype", (void*)H_MutexAttrSetType},
  {"pthread_mutexattr_destroy", (void*)H_MutexAttrDestroy},
  {"sceKernelVirtualQuery", (void*)H_VirtualQuery},
  {"pthread_mutex_init", (void*)H_MutexInit}, {"pthread_mutex_lock", (void*)H_MutexLock}, {"pthread_mutex_unlock", (void*)H_MutexUnlock},
  {"pthread_mutex_destroy", (void*)H_MutexDestroy},
  {"scePthreadKeyCreate", (void*)H_KeyCreate}, {"scePthreadGetspecific", (void*)H_GetSpecific}, {"scePthreadSetspecific", (void*)H_SetSpecific},
  {"pthread_key_create", (void*)H_KeyCreate}, {"pthread_getspecific", (void*)H_GetSpecific}, {"pthread_setspecific", (void*)H_SetSpecific},
  {"scePthreadSelf", (void*)H_PthreadSelf}, {"pthread_self", (void*)H_PthreadSelf}, {"scePthreadYield", (void*)H_PthreadYield},
  {"scePthreadCreate", (void*)H_PthreadCreate}, {"pthread_create", (void*)H_PthreadCreate},
  {"scePthreadJoin", (void*)H_PthreadJoin}, {"pthread_join", (void*)H_PthreadJoin}, {"scePthreadExit", (void*)H_PthreadExit},
  {"scePthreadAttrInit", (void*)H_AttrInit}, {"scePthreadAttrDestroy", (void*)H_AttrDestroy}, {"scePthreadAttrSetstacksize", (void*)H_AttrSetStack},
  {"pthread_cond_init", (void*)H_CondInit}, {"pthread_cond_wait", (void*)H_CondWait}, {"pthread_cond_signal", (void*)H_CondSignal},
  {"pthread_cond_broadcast", (void*)H_CondBroadcast}, {"scePthreadCondInit", (void*)H_CondInit}, {"scePthreadCondWait", (void*)H_CondWait},
  {"scePthreadCondSignal", (void*)H_CondSignal}, {"scePthreadCondBroadcast", (void*)H_CondBroadcast},
  {"sceKernelCreateSema", (void*)H_CreateSema}, {"sceKernelWaitSema", (void*)H_WaitSema}, {"sceKernelSignalSema", (void*)H_SignalSema},
  {"sceKernelPollSema", (void*)H_PollSema}, {"sceKernelDeleteSema", (void*)H_DeleteSema},
  {"sceKernelCreateEventFlag", (void*)H_CreateEvf}, {"sceKernelSetEventFlag", (void*)H_SetEvf},
  {"sceKernelWaitEventFlag", (void*)H_WaitEvf}, {"sceKernelDeleteEventFlag", (void*)H_DeleteEvf},
  {"sceKernelReserveVirtualRange", (void*)H_ReserveVirtualRange}, {"sceKernelMapNamedDirectMemory", (void*)H_MapDirectMem},
  {"sceKernelStat", (void*)H_KernelStat},
  // thread tuning calls the game makes while creating its workers: accepted, no host-side effect
  {"scePthreadSetaffinity", (void*)H_Nop}, {"scePthreadSetprio", (void*)H_Nop}, {"scePthreadRename", (void*)H_Nop},
  {"scePthreadAttrSetschedpolicy", (void*)H_Nop}, {"scePthreadAttrSetinheritsched", (void*)H_Nop},
  {"scePthreadAttrSetdetachstate", (void*)H_Nop}, {"scePthreadAttrSetschedparam", (void*)H_Nop}, {"scePthreadAttrSetaffinity", (void*)H_Nop},
  // system services
  {"sceUserServiceInitialize", (void*)H_Nop}, {"sceUserServiceGetLoginUserIdList", (void*)H_GetLoginUserIdList}, {"sceUserServiceGetUserName", (void*)H_GetUserName},
  {"sceAppContentInitialize", (void*)H_Nop}, {"sceAppContentAppParamGetInt", (void*)H_AppParamGetInt},
  {"sceCommonDialogInitialize", (void*)H_Nop}, {"scePadInit", (void*)H_Nop}, {"scePadGetControllerInformation", (void*)H_PadGetInfo},
  {"sceUserServiceGetInitialUser", (void*)H_GetInitialUser},
  {"sceVideoOutOpen", (void*)H_OpenHandle}, {"scePadOpen", (void*)H_OpenHandle}, {"sceAudioOutOpen", (void*)H_OpenHandle},
  {"sceVideoOutSubmitFlip", (void*)H_Flip},
};
const HleData kHleData[] = {
  {"__stack_chk_guard", &g_stackGuard},
};

std::unordered_map<std::string, void*> g_hleFn;     // key: plain name AND NID
std::unordered_map<std::string, void*> g_hleData;
void BuildHleMaps() {
  g_hleFn.clear(); g_hleData.clear();
  for (auto& h : kHle) { void* t = ThunkToHost(h.fn); g_hleFn[h.name] = t; g_hleFn[NidFromName(h.name)] = t; }
  for (auto& d : kHleData) { g_hleData[d.name] = d.addr; g_hleData[NidFromName(d.name)] = d.addr; }
}

// "gQX+4GDQjpM#j#j" -> import key "gQX+4GDQjpM"; plain names pass through unchanged
std::string ImportKey(const char* raw) { std::string s = raw; size_t h = s.find('#'); return h == std::string::npos ? s : s.substr(0, h); }

void* Resolve(const char* raw) {
  auto cached = g_resolved.find(raw);
  if (cached != g_resolved.end()) return cached->second;
  std::string key = ImportKey(raw);
  const char* known = NidToName(key);
  ImportRec rec{raw, key, "", known ? known : ""};
  void* res = nullptr;
  auto f = g_hleFn.find(key);
  if (f != g_hleFn.end()) { res = f->second; rec.status = "HLE"; g_nRes++; }
  else {
    auto d = g_hleData.find(key);
    if (d != g_hleData.end()) { res = d->second; rec.status = "HLE-data"; g_nRes++; }
  }
  if (!res) {
    auto ins = g_unres.try_emplace(raw).first;
    UnresInfo& u = ins->second;
    u.raw = raw;
    u.display = known ? std::string(known) + " [" + key + "]" : std::string(raw);
    g_nUnres++;
    res = ThunkUnresolved((void*)H_Unresolved, &u);
    rec.status = "UNRESOLVED";
  }
  g_imports.push_back(rec);
  g_resolved[raw] = res;
  return res;
}

void DumpImports() {
  std::wstring p = ExeDirW() + L"ps5emu.imports.txt";
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) return;
  fprintf(f, "# NID\tlib\tmodule\tstatus\tname (blank = name unknown; add names to nids.txt, see tools/nid.py)\n");
  for (auto& r : g_imports) {
    std::string lib = "", mod = "";
    size_t a = r.raw.find('#');
    if (a != std::string::npos) { size_t b = r.raw.find('#', a + 1); lib = r.raw.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1); if (b != std::string::npos) mod = r.raw.substr(b + 1); }
    fprintf(f, "%s\t%s\t%s\t%s\t%s\n", r.nid.c_str(), lib.c_str(), mod.c_str(), r.status.c_str(), r.name.c_str());
  }
  fclose(f);
}

void LogUnresolvedSummary() {
  std::vector<std::pair<uint64_t, UnresInfo*>> v;
  for (auto& kv : g_unres) if (kv.second.calls) v.push_back({kv.second.calls.load(), &kv.second});
  if (v.empty()) return;
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  Log("unresolved imports that were called (%d distinct), most called first:", (int)v.size());
  for (size_t i = 0; i < v.size() && i < 25; i++) Log("  %8llu x %s", (unsigned long long)v[i].first, v[i].second->display.c_str());
}

// ---------------------------------------------------------------- fault capture
void LogFault(EXCEPTION_POINTERS* ep) {
  EXCEPTION_RECORD* r = ep->ExceptionRecord; CONTEXT* c = ep->ContextRecord;
  uintptr_t pc = (uintptr_t)c->Rip;
  Log("*** GUEST FAULT: exception 0x%08lX at %s", r->ExceptionCode, EmuDescribeAddr(pc).c_str());
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    const char* k = r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "execute";
    uintptr_t a = (uintptr_t)r->ExceptionInformation[1];
    Log("    access violation: %s of address 0x%llx (%s)", k, (unsigned long long)a, EmuDescribeAddr(a).c_str());
    if (a < 0x10000) Log("    hint: near-NULL address -> null pointer, usually the return value of an unresolved import (stubs return 0) or an unpatched fs: access");
  }
  if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) Log("    hint: illegal instruction (unsupported CPU feature, or jumped into data)");
  if (r->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION)    Log("    hint: privileged instruction (guest executed ring0/OS-specific code)");
  if (r->ExceptionCode == EXCEPTION_BREAKPOINT)          Log("    hint: INT3 - the guest hit a breakpoint/assert (often after a failed import)");
  Log("    RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
  Log("    RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
  Log("    R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx", c->R8, c->R9, c->R10, c->R11);
  Log("    R12=%016llx R13=%016llx R14=%016llx R15=%016llx", c->R12, c->R13, c->R14, c->R15);
  unsigned char b[16] = {0}; SIZE_T got = 0;
  if (ReadProcessMemory(GetCurrentProcess(), (void*)pc, b, sizeof b, &got) && got) {
    char hex[64] = ""; for (SIZE_T i = 0; i < got; i++) snprintf(hex + i * 3, 4, "%02x ", b[i]);
    Log("    code at fault: %s", hex);
    if (b[0] == 0x64 || b[1] == 0x64 || b[2] == 0x64) Log("    hint: fs: access that the TLS patcher did not rewrite (only 'mov r64, fs:[disp32]' is handled)");
    if (b[0] == 0x0F && b[1] == 0x05) Log("    hint: raw SYSCALL instruction -> guest talks to the kernel directly, needs HLE");
  }
  // execute-at-NULL: [rsp] is the return address pushed by the bad `call`. Decode that call so we know
  // WHICH pointer was null (a global slot, a vtable entry, or a register) instead of guessing.
  if (pc < 0x10000) {
    uint64_t ret = 0; uint8_t pre[8] = {0}; SIZE_T g3 = 0;
    ReadProcessMemory(GetCurrentProcess(), (void*)c->Rsp, &ret, 8, &g3);
    Log("    faulting thread id: %lu (guest main thread: %s)", GetCurrentThreadId(), g_guest && GetThreadId(g_guest) == GetCurrentThreadId() ? "yes" : "no, a worker");
    if (g3 == 8 && ReadProcessMemory(GetCurrentProcess(), (void*)(ret - 8), pre, 8, &g3)) {
      Log("    bytes around the call (-16..+11): %s", UnresolvedCallsiteBytes((uintptr_t)ret).c_str());
      const DWORD64* R = &c->Rax;     // CONTEXT order: rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15
      static const char* rn[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
      auto slotInfo = [&](uintptr_t slot, const char* how) {
        uint64_t val = 0; SIZE_T gg = 0; bool rd = ReadProcessMemory(GetCurrentProcess(), (void*)slot, &val, 8, &gg) && gg == 8;
        Log("    faulting call: %s -> slot %s  value=%s0x%llx", how, EmuDescribeAddr(slot).c_str(), rd ? "" : "(unreadable) ", (unsigned long long)val);
      };
      // pre[7] is the last byte before the return address
      if (pre[2] == 0xFF && pre[3] == 0x15) {                                  // call [rip+disp32]  (6 bytes)
        int32_t d; memcpy(&d, pre + 4, 4); slotInfo((uintptr_t)(ret + (int64_t)d), "call [rip+disp32]");
      } else if (pre[5] == 0x41 && pre[6] == 0xFF && (pre[7] & 0xF8) == 0xD0) {  // call r8..r15
        Log("    faulting call: call %s (register held 0)", rn[8 + (pre[7] & 7)]);
      } else if (pre[6] == 0xFF && (pre[7] & 0xF8) == 0xD0) {                   // call rax..rdi
        Log("    faulting call: call %s (register held 0)", rn[pre[7] & 7]);
      } else {
        // [reg+disp8] / [reg+disp32] / [reg], optionally with REX.B; SIB forms (rm==4) not decoded
        struct F { int len; int mod; } forms[] = {{3,1},{6,2},{2,0}};
        bool done = false;
        for (auto& fm : forms) {
          for (int rex = 0; rex <= 1 && !done; rex++) {
            int total = fm.len + rex; const uint8_t* q = pre + 8 - total;
            if (rex && q[0] != 0x41) continue;
            const uint8_t* op = q + rex;
            if (op[0] != 0xFF) continue;
            uint8_t m = op[1]; if ((m >> 6) != fm.mod || ((m >> 3) & 7) != 2 || (m & 7) == 4 || (fm.mod == 0 && (m & 7) == 5)) continue;
            int idx = (m & 7) + (rex ? 8 : 0); int64_t disp = 0;
            if (fm.mod == 1) disp = (int8_t)op[2]; else if (fm.mod == 2) { int32_t d; memcpy(&d, op + 2, 4); disp = d; }
            char how[64]; snprintf(how, sizeof how, "call [%s%+lld]", rn[idx], (long long)disp);
            slotInfo((uintptr_t)(R[idx] + disp), how); done = true;
          }
          if (done) break;
        }
        if (!done) Log("    faulting call: form not decoded - use the bytes above");
      }
    }
  }
  if (UnresInfo* u = g_lastUnres.load()) Log("    last unresolved import called: %s (%llu calls so far)", u->display.c_str(), (unsigned long long)u->calls.load());
  uint64_t st[96]; SIZE_T g2 = 0; int shown = 0;
  if (ReadProcessMemory(GetCurrentProcess(), (void*)c->Rsp, st, sizeof st, &g2)) {
    Log("    guest return-address candidates on stack:");
    for (SIZE_T i = 0; i < g2 / 8 && shown < 12; i++) {
      std::string d = EmuDescribeAddr((uintptr_t)st[i]);
      if (d.rfind("GUEST", 0) == 0) { Log("      [rsp+0x%02llx] %s", (unsigned long long)(i * 8), d.c_str()); shown++; }
    }
    if (!shown) Log("      (none)");
  }
  LogUnresolvedSummary();
}

[[noreturn]] void GuestFaultExit() {
  g_quit = true;
  if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  ExitThread(1);
  for (;;) {}
}

LONG CALLBACK GuestVeh(EXCEPTION_POINTERS* ep) {
  if (!t_isGuest) return EXCEPTION_CONTINUE_SEARCH;
  switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_BREAKPOINT: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      break;
    default: return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!g_faulted.exchange(true)) LogFault(ep);
  CONTEXT* c = ep->ContextRecord;
  c->Rip = (DWORD64)&GuestFaultExit;
  c->Rsp = ((c->Rsp - 0x400) & ~0xFull) - 8;     // fresh MS-ABI frame below the guest's red zone
  return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------- loader
bool LoadElf(const std::wstring& path, void** entryOut) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) { Log("cannot open file"); return false; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(n > 0 ? n : 0);
  if (n > 0) fread(buf.data(), 1, n, f);
  fclose(f);
  if (n < (long)sizeof(Ehdr)) { Log("file too small"); return false; }
  uint32_t magic; memcpy(&magic, buf.data(), 4);
  if (magic == 0x1D3D154F) { Log("SELF container (signed/encrypted). A decrypted ELF is required."); return false; }
  const Ehdr* e = (const Ehdr*)buf.data();
  if (memcmp(e->id, "\x7f" "ELF", 4) || e->id[4] != 2 || e->machine != 62) { Log("not an ELF64 x86-64 file"); return false; }
  if (e->phoff + (uint64_t)e->phnum * sizeof(Phdr) > (uint64_t)n) { Log("bad program headers"); return false; }
  const Phdr* ph = (const Phdr*)(buf.data() + e->phoff);
  g_orbis = (e->type == 0xFE00 || e->type == 0xFE10 || e->type == 0xFE18);
  g_tls = TlsInfo();
  Log("ELF: type=0x%x (%s) machine=%u entry=0x%llx phnum=%u size=%ld bytes", e->type,
      e->type == 2 ? "EXEC, fixed address" : e->type == 3 ? "DYN/PIE" : e->type == 0xFE10 ? "SCE_DYNEXEC (console executable)" :
      e->type == 0xFE00 ? "SCE_EXEC" : e->type == 0xFE18 ? "SCE_DYNAMIC (library)" : "other", e->machine,
      (unsigned long long)e->entry, e->phnum, n);
  for (int i = 0; i < e->phnum; i++) {
    Log("  PH[%d] type=0x%x flags=%u vaddr=0x%llx filesz=0x%llx memsz=0x%llx", i, ph[i].type, ph[i].flags,
        (unsigned long long)ph[i].vaddr, (unsigned long long)ph[i].filesz, (unsigned long long)ph[i].memsz);
    if (ph[i].type == 3) Log("  WARNING: PT_INTERP present - guest expects a dynamic loader/libc. Only the HLE imports are available; build with -nostdlib or use HLE names.");
    if (ph[i].type == 7) { g_tls = { true, ph[i].vaddr, ph[i].filesz, ph[i].memsz, ph[i].align };
      Log("  note: PT_TLS present (init 0x%llx bytes, total 0x%llx, align %llu) - per-thread TCB + fs: patching is on",
          (unsigned long long)ph[i].filesz, (unsigned long long)ph[i].memsz, (unsigned long long)ph[i].align); }
    if (ph[i].type == 0x61000001) Log("  note: PT_SCE_PROCPARAM (process parameters, read by libkernel through sceKernelGetProcParam)");
  }

  std::vector<char> skip(e->phnum, 0);
  uint64_t lo = 0, hi = 0;
  // skipHdr: ignore a read-only "ELF header only" segment (some linkers put it 1 page below .text)
  auto bounds = [&](bool skipHdr) {
    lo = ~0ull; hi = 0;
    for (int i = 0; i < e->phnum; i++) if (ph[i].type == 1) {
      skip[i] = skipHdr && ph[i].off == 0 && !(ph[i].flags & 3) && ph[i].memsz <= 0x1000 && e->phnum > 1;
      if (skip[i]) continue;
      if (ph[i].vaddr < lo) lo = ph[i].vaddr;
      if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
  };
  for (int i = 0; i < e->phnum; i++) if (ph[i].type == 1 && ph[i].off + ph[i].filesz > (uint64_t)n) { Log("segment outside file"); return false; }
  bounds(false);
  if (lo == ~0ull) { Log("no loadable segments"); return false; }
  uint64_t base = lo & ~0xFFFFull, size = (hi - base + 0xFFF) & ~0xFFFull;
  uint8_t* mem = nullptr;
  if (e->type == 2) {  // ET_EXEC: wants a fixed address
    mem = (uint8_t*)VirtualAlloc((void*)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) {
      DWORD err = GetLastError();
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((void*)base, &mbi, sizeof mbi))
        Log("fixed base %p unavailable (err %lu): host region state=0x%lx type=0x%lx allocbase=%p size=0x%llx",
            (void*)base, err, mbi.State, mbi.Type, mbi.AllocationBase, (unsigned long long)mbi.RegionSize);
      bounds(true);   // retry without the header-only page
      if (lo != ~0ull && (lo & ~0xFFFFull) != base) {
        uint64_t b2 = lo & ~0xFFFFull, s2 = (hi - b2 + 0xFFF) & ~0xFFFull;
        mem = (uint8_t*)VirtualAlloc((void*)b2, s2, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (mem) { base = b2; size = s2; Log("mapped at fixed base %p without the ELF-header page (it overlapped host memory)", (void*)b2); }
      }
      if (!mem) {
        bounds(false);
        Log("falling back to a free address; non-PIE code with absolute addresses will break. Link the guest as PIE or at a high base (e.g. -Wl,--image-base=0x10000000).");
      }
    }
  }
  if (!mem) mem = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!mem) { Log("VirtualAlloc failed (err %lu)", GetLastError()); return false; }
  g_img = mem; g_imgSize = size;
  uint8_t* bias = mem - base;
  g_bias = bias; g_nRes = g_nUnres = 0;
  for (int i = 0; i < e->phnum; i++) if (ph[i].type == 1 && !skip[i])
    memcpy(bias + ph[i].vaddr, buf.data() + ph[i].off, ph[i].filesz);
  Log("loaded %s ELF, %llu KB at %p, entry +0x%llx", e->type == 2 ? "EXEC" : "DYN",
      (unsigned long long)(size >> 10), (void*)mem, (unsigned long long)e->entry);

  if (e->entry < base || e->entry >= base + size) { Log("entry point 0x%llx outside loaded image", (unsigned long long)e->entry); return false; }
  { char hex[100] = ""; const uint8_t* q = bias + e->entry;
    for (int k = 0; k < 24 && e->entry + k < base + size; k++) snprintf(hex + k * 3, 4, "%02x ", q[k]);
    Log("entry bytes: %s", hex); }
  { int nsys = 0, nint80 = 0, nfs = 0; uint64_t firstSys = 0;
    for (int i = 0; i < e->phnum; i++) if (ph[i].type == 1 && (ph[i].flags & 1) && !skip[i]) {
      const uint8_t* d = buf.data() + ph[i].off;
      for (uint64_t k = 0; k + 8 < ph[i].filesz; k++) {
        if (d[k] == 0x0F && d[k + 1] == 0x05) { if (!nsys) firstSys = ph[i].vaddr + k; nsys++; }
        else if (d[k] == 0xCD && d[k + 1] == 0x80) nint80++;
        else if (d[k] == 0x64 && (d[k + 1] & 0xFB) == 0x48 && d[k + 2] == 0x8B && (d[k + 3] & 0xC7) == 0x04 && d[k + 4] == 0x25) nfs++;
      }
    }
    if (nsys) Log("code scan: %d x SYSCALL (0F 05), first at guest vaddr 0x%llx (byte pattern only: may include false positives) -> raw kernel calls need HLE", nsys, (unsigned long long)firstSys);
    if (nint80) Log("code scan: %d x INT 0x80 (byte pattern only)", nint80);
    if (nfs) {
      if (g_tlsIdx == TLS_OUT_OF_INDEXES) g_tlsIdx = TlsAlloc();
      int patched = 0, failed = 0;
      bool ok = PatchFsLoads(ph, e->phnum, skip, patched, failed);
      Log("code scan: %d x 'mov r64, fs:[disp32]' -> TLS patch %s: %d rewritten, %d failed", nfs, ok ? "ok" : "NOT applied", patched, failed);
    }
    if (!nsys && !nint80 && !nfs) Log("code scan: no syscalls / TLS access found"); }

  // dynamic section: relocations + imports
  std::map<uint32_t, int> relTypes;
  for (int i = 0; i < e->phnum; i++) if (ph[i].type == 2) {
    const Dyn* d = (const Dyn*)(bias + ph[i].vaddr);
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, symtab = 0, strtab = 0;
    for (; d->tag; d++) switch (d->tag) {
      case 7: rela = d->val; break;   case 8: relasz = d->val; break;
      case 23: jmprel = d->val; break; case 2: pltsz = d->val; break;
      case 6: symtab = d->val; break; case 5: strtab = d->val; break;
    }
    const Sym* syms = symtab ? (const Sym*)(bias + symtab) : nullptr;
    const char* strs = strtab ? (const char*)(bias + strtab) : nullptr;
    auto apply = [&](uint64_t off, uint64_t sz) {
      if (!off) return;
      const Rela* r = (const Rela*)(bias + off);
      for (uint64_t k = 0; k < sz / sizeof(Rela); k++) {
        uint32_t type = (uint32_t)r[k].info, si = (uint32_t)(r[k].info >> 32);
        uint64_t* where = (uint64_t*)(bias + r[k].offset);
        uint64_t S = 0;
        if (si && syms) {
          const Sym& s = syms[si];
          S = s.shndx ? (uint64_t)(bias + s.value) : (uint64_t)Resolve(strs + s.name);
        }
        switch (type) {
          case 8: *where = (uint64_t)bias + r[k].addend; break;  // RELATIVE
          case 1: *where = S + r[k].addend; break;               // 64
          case 6: case 7: *where = S; break;                     // GLOB_DAT / JUMP_SLOT
          default: relTypes[type]++; break;
        }
      }
    };
    apply(rela, relasz);
    apply(jmprel, pltsz);
  }
  for (auto& kv : relTypes) Log("WARNING: %d relocation(s) of type %u not handled (e.g. 16/17/18 = TLS DTPMOD/DTPOFF/TPOFF) - those slots stay unrelocated", kv.second, kv.first);
  int known = 0; for (auto& r : g_imports) if (r.status == "UNRESOLVED" && !r.name.empty()) known++;
  Log("imports: %d resolved to HLE, %d unresolved (stubbed; %d of those have a known name). Full table: ps5emu.imports.txt", g_nRes, g_nUnres, known);
  DumpImports();
  *entryOut = bias + e->entry;
  Log("entry point at %p (guest vaddr 0x%llx)", (void*)(bias + e->entry), (unsigned long long)e->entry);
  return true;
}

// ---------------------------------------------------------------- guest + window
uint64_t g_orbisArgs[8];
char     g_argv0[] = "eboot.bin";

DWORD WINAPI GuestMain(void* entryThunk) {
  typedef uint64_t (*Fn)(void*, void*);
  t_isGuest = true;
  Log("guest thread started");
  uint8_t* tp = SetupGuestTcb();
  if (tp) Log("guest thread pointer (TCB) at %p", (void*)tp);
  uint64_t r;
  if (g_orbis) {
    // console process start: _start(void* args, void (*exit_fn)()), args = { u64 argc; char* argv[argc]; NULL; envp...; NULL }
    memset(g_orbisArgs, 0, sizeof g_orbisArgs);
    g_orbisArgs[0] = 1; g_orbisArgs[1] = (uint64_t)g_argv0;
    r = ((Fn)entryThunk)(g_orbisArgs, ThunkToHost((void*)H_Nop));
  } else {
    r = ((Fn)entryThunk)(&g_hle, nullptr);      // legacy test guest: rdi = HLE table
  }
  Log("guest returned %llu", (unsigned long long)r);
  LogUnresolvedSummary();
  if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  return 0;
}

LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_PAINT: {
      if (g_vkOk) { ValidateRect(h, nullptr); return 0; }
      PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
      BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = FB_W; bi.bmiHeader.biHeight = -FB_H;
      bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
      RECT rc; GetClientRect(h, &rc);
      SetStretchBltMode(dc, COLORONCOLOR);
      StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, FB_W, FB_H, g_show, &bi, DIB_RGB_COLORS, SRCCOPY);
      EndPaint(h, &ps); return 0;
    }
    case WM_TIMER:
      if (w == 2) {   // window title: FPS / CPU / GPU
        PerfStats s = Perf::Get();
        wchar_t gpu[16]; if (s.gpu < 0) wcscpy(gpu, L"n/a"); else swprintf(gpu, 16, L"%.0f%%", s.gpu);
        wchar_t t[400];
        swprintf(t, 400, L"%ls | FPS %.1f | CPU %.0f%% | GPU %ls | %ls", g_title.c_str(), s.fps, s.cpu, gpu, g_vkOk ? L"Vulkan" : L"Software");
        SetWindowTextW(h, t);
        return 0;
      }
      if (g_guest && g_state == Emu::Running && !g_quit) {
        CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool ok = false;
        if (SuspendThread(g_guest) != (DWORD)-1) { ok = GetThreadContext(g_guest, &c) != 0; ResumeThread(g_guest); }
        g_hb++;
        if (ok && (g_hb <= 5 || g_hb % 5 == 0))   // log AFTER resume: guest may hold the log lock
          Log("guest alive: RIP=%s RSP=%p flips=%llu%s", EmuDescribeAddr((uintptr_t)c.Rip).c_str(), (void*)c.Rsp,
              (unsigned long long)g_flips.load(), g_flips ? "" : " (never submitted a frame; guest has not reached the HLE flip path)");
      }
      return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: g_quit = true; PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

struct BootArgs { std::wstring title; void* entryThunk; };

DWORD WINAPI WinThread(void* p) {
  BootArgs* a = (BootArgs*)p;
  static bool reg = false;
  if (!reg) {
    WNDCLASSW wc = {}; wc.lpfnWndProc = GameProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ps5emu_game"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); reg = true;
  }
  RECT r = {0, 0, 960, 540};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_quit = false;
  g_wnd = CreateWindowW(L"ps5emu_game", a->title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  g_hb = 0; g_flips = 0; g_title = a->title; g_show = g_fb;
  g_vkOk = false;
  if (g_gpuMode == 1) {
    if (g_vk.Init(g_wnd, g_vsync, FB_W, FB_H)) { g_vkOk = true; Perf::SetGpuDesc(g_vk.Desc()); }
    else { Log("Vulkan unavailable -> software rendering"); g_vk.Shutdown(); Perf::SetGpuDesc("Software (Vulkan unavailable)"); }
  } else {
    Log("GPU backend: software (selected)"); Perf::SetGpuDesc("Software");
  }
  SetTimer(g_wnd, 1, 3000, nullptr);
  SetTimer(g_wnd, 2, 500, nullptr);
  UiOnEmuState();
  g_guest = CreateThread(nullptr, 0, GuestMain, a->entryThunk, 0, nullptr);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  g_quit = true;
  if (g_guest) {
    bool killed = false;
    if (WaitForSingleObject(g_guest, 1000) == WAIT_TIMEOUT) { TerminateThread(g_guest, 0); killed = true; Log("guest force-terminated"); }
    CloseHandle(g_guest); g_guest = nullptr;
    if (killed && g_vkOk) { g_vkOk = false; Log("Vulkan objects leaked (guest killed mid-present); skipping teardown"); }
  }
  StopGuestThreads();
  if (g_vkOk) { g_vkOk = false; g_vk.Shutdown(); }
  Perf::SetGpuDesc("");
  g_wnd = nullptr;
  if (g_img) { VirtualFree(g_img, 0, MEM_RELEASE); g_img = nullptr; g_imgSize = 0; }
  if (g_tramp) { VirtualFree(g_tramp, 0, MEM_RELEASE); g_tramp = nullptr; g_trampUsed = 0; }
  g_state = Emu::Stopped;
  Log("emulation stopped");
  UiOnEmuState();
  delete a;
  return 0;
}

} // namespace

bool Emu::Boot(const std::wstring& path, const std::wstring& title) {
  if (g_state != Stopped) { Log("already running"); return false; }
  static bool vehInstalled = false;
  if (!vehInstalled) { AddVectoredExceptionHandler(1, GuestVeh); vehInstalled = true; }
  QueryPerformanceFrequency(&g_qpcFreq); QueryPerformanceCounter(&g_qpc0);
  g_poolUsed = 0; g_unres.clear(); g_resolved.clear(); g_imports.clear();
  g_lastUnres = nullptr; g_faulted = false; g_dmemNext = 0; g_nKeys = 0;
  { std::lock_guard<std::mutex> l(g_kmx); g_kobj.clear(); }
  if (g_tramp) { VirtualFree(g_tramp, 0, MEM_RELEASE); g_tramp = nullptr; g_trampUsed = 0; }
  NidLoadExtra(ExeDirW() + L"nids.txt");
  { size_t sl = path.find_last_of(L"\\/"); std::wstring d = sl == std::wstring::npos ? L"." : path.substr(0, sl);
    int n = WideCharToMultiByte(CP_ACP, 0, d.c_str(), -1, nullptr, 0, nullptr, nullptr);
    g_gameRoot.assign(n > 0 ? (size_t)n - 1 : 0, '\0'); if (n > 1) WideCharToMultiByte(CP_ACP, 0, d.c_str(), -1, &g_gameRoot[0], n, nullptr, nullptr); }
  BuildHleMaps();
  Log("NID name table: %llu known names", (unsigned long long)NidKnownCount());
  void* entry = nullptr;
  if (!LoadElf(path, &entry)) {
    if (g_img) { VirtualFree(g_img, 0, MEM_RELEASE); g_img = nullptr; g_imgSize = 0; }
    return false;
  }
  g_hle = { g_fb, FB_W, FB_H, ThunkToHost((void*)H_Flip), ThunkToHost((void*)H_Log) };
  BootArgs* a = new BootArgs{ title, ThunkToGuest(entry) };
  g_state = Running;
  HANDLE t = CreateThread(nullptr, 0, WinThread, a, 0, nullptr);
  if (!t) { g_state = Stopped; delete a; Log("CreateThread failed"); return false; }
  CloseHandle(t);
  return true;
}

void Emu::Pause(bool p) {
  if (!g_guest) return;
  if (p && g_state == Running)       { SuspendThread(g_guest); g_state = Paused; }
  else if (!p && g_state == Paused)  { ResumeThread(g_guest);  g_state = Running; }
  UiOnEmuState();
}

void Emu::Stop() { if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0); }
Emu::State Emu::GetState() { return (State)g_state.load(); }
void Emu::SetGpuMode(int m) { g_gpuMode = m ? 1 : 0; }
int  Emu::GetGpuMode() { return g_gpuMode; }
void Emu::SetVsync(bool on) { g_vsync = on; if (g_vkOk) g_vk.SetVsync(on); }
void Emu::SetOverlay(bool on) { g_overlay = on; }
uint64_t Emu::Flips() { return g_flips.load(); }

std::string EmuDescribeAddr(uintptr_t a) {
  char b[400];
  if (g_img && a >= (uintptr_t)g_img && a < (uintptr_t)g_img + g_imgSize) {
    snprintf(b, sizeof b, "GUEST image: vaddr 0x%llx (image base %p, file-relative +0x%llx)",
             (unsigned long long)(a - (uintptr_t)g_bias), (void*)g_img, (unsigned long long)(a - (uintptr_t)g_img));
    return b;
  }
  if (g_pool && a >= (uintptr_t)g_pool && a < (uintptr_t)g_pool + POOL_SIZE) {
    snprintf(b, sizeof b, "HLE thunk pool +0x%llx", (unsigned long long)(a - (uintptr_t)g_pool)); return b;
  }
  if (g_tramp && a >= (uintptr_t)g_tramp && a < (uintptr_t)g_tramp + TRAMP_SIZE) {
    snprintf(b, sizeof b, "TLS patch trampoline +0x%llx", (unsigned long long)(a - (uintptr_t)g_tramp)); return b;
  }
  HMODULE m = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
    char path[MAX_PATH] = "?"; GetModuleFileNameA(m, path, MAX_PATH);
    const char* nm = strrchr(path, '\\'); nm = nm ? nm + 1 : path;
    snprintf(b, sizeof b, "%s+0x%llx", nm, (unsigned long long)(a - (uintptr_t)m)); return b;
  }
  snprintf(b, sizeof b, "0x%llx", (unsigned long long)a);
  return b;
}

#else  // ---------------------------------------------------------------- 32-bit host

bool Emu::Boot(const std::wstring&, const std::wstring&) {
  Log("x86 (32-bit) host cannot natively execute x86-64 guest code and there is no CPU translation layer. Use the x64 build.");
  return false;
}
void Emu::Pause(bool) {}
void Emu::Stop() {}
Emu::State Emu::GetState() { return Stopped; }
void Emu::SetGpuMode(int) {}
int  Emu::GetGpuMode() { return 0; }
void Emu::SetVsync(bool) {}
void Emu::SetOverlay(bool) {}
uint64_t Emu::Flips() { return 0; }
std::string EmuDescribeAddr(uintptr_t a) { char b[32]; snprintf(b, sizeof b, "0x%llx", (unsigned long long)a); return b; }

#endif
