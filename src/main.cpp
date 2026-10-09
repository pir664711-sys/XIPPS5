// main.cpp - RPCS3-style main window: menu bar, toolbar (+search), game list, log panel, status bar.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "emu.h"
#include "perf.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#ifdef _WIN64
#define ARCH L"x64"
#else
#define ARCH L"x86"
#endif

enum {
  ID_BOOT = 100, ID_ADDDIR, ID_EXIT, ID_START, ID_PAUSE, ID_STOP, ID_REFRESH, ID_SHOWLOG, ID_ABOUT,
  ID_CFG_CPU, ID_GPU_SW, ID_GPU_VK, ID_VSYNC, ID_OVERLAY, ID_CFG_AUDIO, ID_CFG_IO, ID_CTX_BOOT, ID_CTX_OPEN, ID_OPENLOG,
  ID_SEARCH = 200, ID_LIST, ID_LOG, ID_TOOL, ID_STATUS
};
#define WM_LOG      (WM_APP + 1)
#define WM_EMUSTATE (WM_APP + 2)

static HWND g_main, g_tool, g_list, g_log, g_status, g_search;
static HFONT g_font;
static bool g_showLog = true;
static std::wstring g_curTitle;
static bool g_vsyncOpt = true, g_overlayOpt = true;

// ------------------------------------------------------------------ logging
static std::wstring ExeDir();
static CRITICAL_SECTION g_fileCs;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static std::mutex g_logMx;
static std::vector<std::string> g_logQ;

// log file next to exe: written synchronously (survives crashes), previous run kept as ps5emu.prev.log
static void OpenLogFile() {
  InitializeCriticalSection(&g_fileCs);
  std::wstring p = ExeDir() + L"ps5emu.log", prev = ExeDir() + L"ps5emu.prev.log";
  MoveFileExW(p.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
  g_logFile = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}
static void FileWrite(const char* s) {
  if (g_logFile == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t; GetLocalTime(&t);
  char head[48]; int hn = snprintf(head, sizeof head, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  EnterCriticalSection(&g_fileCs);
  DWORD w; WriteFile(g_logFile, head, hn, &w, nullptr);
  WriteFile(g_logFile, s, (DWORD)strlen(s), &w, nullptr);
  WriteFile(g_logFile, "\r\n", 2, &w, nullptr);
  LeaveCriticalSection(&g_fileCs);
}
static void LogRaw(const char* fmt, ...) {   // file only, no UI queue (safe inside crash handler)
  char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); FileWrite(buf);
}

void Log(const char* fmt, ...) {
  char buf[2048];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  FileWrite(buf);
  { std::lock_guard<std::mutex> l(g_logMx); g_logQ.push_back(buf); }
  if (g_main) PostMessageW(g_main, WM_LOG, 0, 0);
}
void UiOnEmuState() { if (g_main) PostMessageW(g_main, WM_EMUSTATE, 0, 0); }

static std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static std::string N(const std::wstring& s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string r(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n, nullptr, nullptr); return r;
}

static void FlushLog() {
  std::vector<std::string> q;
  { std::lock_guard<std::mutex> l(g_logMx); q.swap(g_logQ); }
  if (q.empty()) return;
  std::wstring out;
  for (auto& s : q) out += W(s) + L"\r\n";
  int len = GetWindowTextLengthW(g_log);
  if (len > 400000) { SetWindowTextW(g_log, L""); len = 0; }
  SendMessageW(g_log, EM_SETSEL, len, len);
  SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)out.c_str());
  SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

// ------------------------------------------------------------------ config / library
struct Game { std::wstring name, serial, version, category, path, dir; };
static std::vector<Game> g_games;
static std::vector<int> g_view;
static std::vector<std::wstring> g_dirs;

static std::wstring ExeDir() {
  wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p; return s.substr(0, s.find_last_of(L'\\') + 1);
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
  EXCEPTION_RECORD* r = ep->ExceptionRecord; CONTEXT* c = ep->ContextRecord;
#ifdef _WIN64
  uintptr_t pc = (uintptr_t)c->Rip;
#else
  uintptr_t pc = (uintptr_t)c->Eip;
#endif
  LogRaw("*** CRASH: exception 0x%08lX at %s (thread %lu)", r->ExceptionCode, EmuDescribeAddr(pc).c_str(), GetCurrentThreadId());
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    const char* k = r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "execute";
    uintptr_t a = (uintptr_t)r->ExceptionInformation[1];
    LogRaw("    access violation: %s of address 0x%llx (%s)", k, (unsigned long long)a, EmuDescribeAddr(a).c_str());
    if (a < 0x10000) LogRaw("    hint: near-NULL address -> null pointer / uninitialised TLS or unresolved import data");
  }
  if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) LogRaw("    hint: illegal instruction (unsupported CPU feature or jumped into data)");
  if (r->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) LogRaw("    hint: privileged instruction (guest executed ring0/OS-specific code)");
  if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) LogRaw("    hint: stack overflow");
#ifdef _WIN64
  LogRaw("    RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
  LogRaw("    RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
  LogRaw("    R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx", c->R8, c->R9, c->R10, c->R11);
  LogRaw("    R12=%016llx R13=%016llx R14=%016llx R15=%016llx", c->R12, c->R13, c->R14, c->R15);
#endif
  unsigned char b[16] = {0}; SIZE_T got = 0;
  if (ReadProcessMemory(GetCurrentProcess(), (void*)pc, b, sizeof b, &got) && got) {
    char hex[64] = ""; for (SIZE_T i = 0; i < got; i++) snprintf(hex + i * 3, 4, "%02x ", b[i]);
    LogRaw("    code at fault: %s", hex);
    if (b[0] == 0x64 || b[1] == 0x64 || b[2] == 0x64 || b[0] == 0x65 || b[1] == 0x65 || b[2] == 0x65)
      LogRaw("    hint: fs:/gs: segment access -> guest uses thread-local storage (TLS not implemented yet)");
    if (b[0] == 0x0F && b[1] == 0x05) LogRaw("    hint: raw SYSCALL instruction -> guest talks to a kernel directly (Linux/BSD syscall), needs HLE");
  }
#ifdef _WIN64
  { // scan stack for return addresses into guest code
    uint64_t st[96]; SIZE_T g2 = 0; int shown = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (void*)c->Rsp, st, sizeof st, &g2)) {
      LogRaw("    guest return-address candidates on stack:");
      for (SIZE_T i = 0; i < g2 / 8 && shown < 12; i++) {
        std::string d = EmuDescribeAddr((uintptr_t)st[i]);
        if (d.rfind("GUEST", 0) == 0) { LogRaw("      [rsp+0x%02llx] %s", (unsigned long long)(i * 8), d.c_str()); shown++; }
      }
      if (!shown) LogRaw("      (none)");
    }
  }
#endif
  LogRaw("*** process terminating. Full log: ps5emu.log next to the exe");
  return EXCEPTION_EXECUTE_HANDLER;
}

static std::wstring IniPath() { return ExeDir() + L"ps5emu.ini"; }

static void LoadConfig() {
  wchar_t buf[8192];
  GetPrivateProfileStringW(L"Library", L"Dirs", L"", buf, 8192, IniPath().c_str());
  std::wstringstream ss(buf); std::wstring t;
  while (std::getline(ss, t, L'|')) if (!t.empty()) g_dirs.push_back(t);
  if (g_dirs.empty()) g_dirs.push_back(ExeDir() + L"games");
  std::wstring ini = IniPath();
  Emu::SetGpuMode(GetPrivateProfileIntW(L"GPU", L"Backend", 1, ini.c_str()));   // default: Vulkan
  g_vsyncOpt = GetPrivateProfileIntW(L"GPU", L"VSync", 1, ini.c_str()) != 0;
  g_overlayOpt = GetPrivateProfileIntW(L"GPU", L"Overlay", 1, ini.c_str()) != 0;
  Emu::SetVsync(g_vsyncOpt); Emu::SetOverlay(g_overlayOpt);
}
static void SaveGpuConfig() {
  std::wstring ini = IniPath();
  WritePrivateProfileStringW(L"GPU", L"Backend", Emu::GetGpuMode() ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"GPU", L"VSync", g_vsyncOpt ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"GPU", L"Overlay", g_overlayOpt ? L"1" : L"0", ini.c_str());
}
static void SaveConfig() {
  std::wstring j;
  for (auto& d : g_dirs) { if (!j.empty()) j += L'|'; j += d; }
  WritePrivateProfileStringW(L"Library", L"Dirs", j.c_str(), IniPath().c_str());
}

static std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string JsonStr(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k); if (p == std::string::npos) return "";
  p = s.find(':', p + k.size()); if (p == std::string::npos) return "";
  p = s.find('"', p); if (p == std::string::npos) return "";
  size_t e = s.find('"', p + 1); if (e == std::string::npos) return "";
  return s.substr(p + 1, e - p - 1);
}

static void TryGame(const fs::path& d) {
  std::error_code ec;
  fs::path e = d / L"eboot.bin";
  if (!fs::exists(e, ec)) { e = d / L"eboot.elf"; if (!fs::exists(e, ec)) return; }
  for (auto& g : g_games) if (g.path == e.wstring()) return;
  Game g; g.path = e.wstring(); g.dir = d.wstring();
  std::string j = ReadAll(d / L"sce_sys" / L"param.json");
  g.serial = W(JsonStr(j, "titleId"));
  g.name = W(JsonStr(j, "titleName"));
  g.version = W(JsonStr(j, "contentVersion"));
  if (g.name.empty()) g.name = d.filename().wstring();
  if (g.serial.empty()) g.serial = L"-";
  if (g.version.empty()) g.version = L"-";
  g.category = L"PS5 Game";
  g_games.push_back(g);
}

static std::wstring Lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; }

static void ApplyFilter() {
  wchar_t q[256] = L""; GetWindowTextW(g_search, q, 256);
  std::wstring ql = Lower(q);
  g_view.clear();
  for (int i = 0; i < (int)g_games.size(); i++)
    if (ql.empty() || Lower(g_games[i].name).find(ql) != std::wstring::npos ||
        Lower(g_games[i].serial).find(ql) != std::wstring::npos) g_view.push_back(i);
  ListView_SetItemCountEx(g_list, (int)g_view.size(), 0);
  InvalidateRect(g_list, nullptr, TRUE);
}

static void RefreshGames() {
  g_games.clear();
  std::error_code ec;
  for (auto& dir : g_dirs) {
    TryGame(dir);
    for (auto& it : fs::directory_iterator(dir, ec)) if (it.is_directory(ec)) TryGame(it.path());
  }
  ApplyFilter();
  Log("game list: %d title(s) in %d folder(s)", (int)g_games.size(), (int)g_dirs.size());
}

// ------------------------------------------------------------------ actions
static void BootPath(const std::wstring& path, const std::wstring& title) {
  Log("booting: %s", N(title).c_str());
  if (Emu::Boot(path, title)) g_curTitle = title;
}
static int SelectedGame() {
  int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
  return (i >= 0 && i < (int)g_view.size()) ? g_view[i] : -1;
}
static void BootSelected() {
  int g = SelectedGame();
  if (g < 0) { Log("select a game first (or File > Boot ELF)"); return; }
  BootPath(g_games[g].path, g_games[g].name);
}
static void BootFile() {
  wchar_t f[MAX_PATH] = L"";
  OPENFILENAMEW o = {}; o.lStructSize = sizeof o; o.hwndOwner = g_main;
  o.lpstrFilter = L"PS5 executable (eboot.bin, *.elf)\0eboot.bin;*.elf;*.bin\0All files\0*.*\0";
  o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&o)) BootPath(f, fs::path(f).filename().wstring());
}
static void AddDir() {
  BROWSEINFOW bi = {}; bi.hwndOwner = g_main; bi.lpszTitle = L"Select games folder";
  bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl) return;
  wchar_t p[MAX_PATH]; BOOL ok = SHGetPathFromIDListW(pidl, p); CoTaskMemFree(pidl);
  if (!ok) return;
  g_dirs.push_back(p); SaveConfig(); RefreshGames();
}
static void ConfigBox(const wchar_t* t, const wchar_t* body) { MessageBoxW(g_main, body, t, MB_OK | MB_ICONINFORMATION); }

static void UpdatePerfStatus() {
  PerfStats s = Perf::Get();
  bool run = Emu::GetState() != Emu::Stopped;
  wchar_t gpu[16]; if (s.gpu < 0) wcscpy(gpu, L"n/a"); else swprintf(gpu, 16, L"%.0f%%", s.gpu);
  PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
  wchar_t ram[64] = L"RAM n/a";
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
    // Working set is physical RAM currently resident; private usage is committed memory unique to this process.
    swprintf(ram, 64, L"RAM WS %.0f MB | Private %.0f MB",
      (double)pmc.WorkingSetSize / (1024.0 * 1024.0),
      (double)pmc.PrivateUsage / (1024.0 * 1024.0));
  }
  wchar_t a[192];
  if (run) swprintf(a, 192, L"FPS %.1f | CPU %.1f%% | GPU %ls | %ls", s.fps, s.cpu, gpu, ram);
  else swprintf(a, 192, L"FPS - | CPU %.1f%% | GPU %ls | %ls", s.cpu, gpu, ram);
  SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)a);
  std::wstring b = L"GPU: ";
  if (run && !Perf::GpuDesc().empty()) b += W(Perf::GpuDesc());
  else b += Emu::GetGpuMode() ? L"Vulkan (idle)" : L"Software (idle)";
  SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)b.c_str());
}

static void UpdateGpuMenu() {
  HMENU m = GetMenu(g_main); if (!m) return;
  CheckMenuRadioItem(m, ID_GPU_SW, ID_GPU_VK, Emu::GetGpuMode() ? ID_GPU_VK : ID_GPU_SW, MF_BYCOMMAND);
  CheckMenuItem(m, ID_VSYNC, g_vsyncOpt ? MF_CHECKED : MF_UNCHECKED);
  CheckMenuItem(m, ID_OVERLAY, g_overlayOpt ? MF_CHECKED : MF_UNCHECKED);
}

static void UpdateUi() {
  Emu::State st = Emu::GetState();
  bool run = st != Emu::Stopped;
  SendMessageW(g_tool, TB_ENABLEBUTTON, ID_START, MAKELONG(!run, 0));
  SendMessageW(g_tool, TB_ENABLEBUTTON, ID_PAUSE, MAKELONG(run, 0));
  SendMessageW(g_tool, TB_ENABLEBUTTON, ID_STOP, MAKELONG(run, 0));
  TBBUTTONINFOW bi = {}; bi.cbSize = sizeof bi; bi.dwMask = TBIF_TEXT;
  bi.pszText = (LPWSTR)(st == Emu::Paused ? L"Resume" : L"Pause");
  SendMessageW(g_tool, TB_SETBUTTONINFOW, ID_PAUSE, (LPARAM)&bi);
  std::wstring s = st == Emu::Stopped ? L"Ready" : (st == Emu::Paused ? L"Paused: " : L"Running: ") + g_curTitle;
  SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)s.c_str());
  UpdatePerfStatus();
}

// ------------------------------------------------------------------ layout / creation
static void Layout() {
  if (!g_list) return;
  RECT rc; GetClientRect(g_main, &rc);
  SendMessageW(g_status, WM_SIZE, 0, 0);
  SendMessageW(g_tool, TB_AUTOSIZE, 0, 0);
  RECT tr, sr; GetWindowRect(g_tool, &tr); GetWindowRect(g_status, &sr);
  int th = tr.bottom - tr.top, sh = sr.bottom - sr.top;
  int avail = rc.bottom - th - sh; if (avail < 0) avail = 0;
  int logH = g_showLog ? avail * 30 / 100 : 0;
  MoveWindow(g_list, 0, th, rc.right, avail - logH, TRUE);
  MoveWindow(g_log, 0, th + avail - logH, rc.right, logH, TRUE);
  ShowWindow(g_log, g_showLog ? SW_SHOW : SW_HIDE);
  MoveWindow(g_search, rc.right - 230, 2, 220, th - 4, TRUE);
  int p2 = rc.right - 420, p1 = rc.right - 740;
  if (p1 < 100) p1 = 100;
  if (p2 < p1 + 50) p2 = p1 + 50;
  int parts[3] = { p1, p2, -1 };
  SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
}

static HMENU BuildMenu() {
  HMENU bar = CreateMenu();
  auto add = [&](const wchar_t* title, HMENU sub) { AppendMenuW(bar, MF_POPUP, (UINT_PTR)sub, title); };
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_BOOT, L"Boot ELF / Game...");
  AppendMenuW(m, MF_STRING, ID_ADDDIR, L"Add Games Folder...");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
  add(L"&File", m);
  m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_START, L"Boot Selected");
  AppendMenuW(m, MF_STRING, ID_PAUSE, L"Pause / Resume");
  AppendMenuW(m, MF_STRING, ID_STOP, L"Stop");
  add(L"&Emulation", m);
  m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_CFG_CPU, L"CPU");
  AppendMenuW(m, MF_STRING, ID_GPU_VK, L"GPU: Vulkan");
  AppendMenuW(m, MF_STRING, ID_GPU_SW, L"GPU: Software (fallback)");
  AppendMenuW(m, MF_STRING, ID_VSYNC, L"VSync");
  AppendMenuW(m, MF_STRING, ID_OVERLAY, L"Performance Overlay (FPS / CPU / GPU)");
  AppendMenuW(m, MF_STRING, ID_CFG_AUDIO, L"Audio");
  AppendMenuW(m, MF_STRING, ID_CFG_IO, L"Input / Output");
  add(L"&Configuration", m);
  m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh Game List");
  AppendMenuW(m, MF_STRING, ID_ADDDIR, L"Game Folders...");
  add(L"&Manage", m);
  m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | MF_CHECKED, ID_SHOWLOG, L"Show Log");
  add(L"&View", m);
  m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_OPENLOG, L"Open Log File");
  AppendMenuW(m, MF_STRING, ID_ABOUT, L"About ps5emu...");
  add(L"&Help", m);
  return bar;
}

static void AddCol(int i, const wchar_t* t, int w) {
  LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  c.pszText = (LPWSTR)t; c.cx = w; c.iSubItem = i;
  SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
}

static void CreateChildren(HWND h) {
  HINSTANCE hi = GetModuleHandleW(nullptr);
  SetMenu(h, BuildMenu());

  g_tool = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
      WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | CCS_TOP,
      0, 0, 0, 0, h, (HMENU)ID_TOOL, hi, nullptr);
  SendMessageW(g_tool, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(g_tool, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
  TBBUTTON b[] = {
    { I_IMAGENONE, ID_START,  TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Start" },
    { I_IMAGENONE, ID_PAUSE,  0,               BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Pause" },
    { I_IMAGENONE, ID_STOP,   0,               BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Stop" },
    { 0, 0, TBSTATE_ENABLED, BTNS_SEP, {0}, 0, 0 },
    { I_IMAGENONE, ID_CFG_CPU, TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Config" },
    { I_IMAGENONE, ID_REFRESH, TBSTATE_ENABLED, BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"Refresh" },
  };
  SendMessageW(g_tool, TB_ADDBUTTONSW, (WPARAM)(sizeof b / sizeof b[0]), (LPARAM)b);

  g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
      0, 0, 0, 0, h, (HMENU)ID_SEARCH, hi, nullptr);
  SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search games...");
  SendMessageW(g_search, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

  g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
      0, 0, 0, 0, h, (HMENU)ID_LIST, hi, nullptr);
  ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
  AddCol(0, L"Name", 300); AddCol(1, L"Serial", 110); AddCol(2, L"Version", 70);
  AddCol(3, L"Category", 90); AddCol(4, L"Path", 420);

  g_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Consolas");
  g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
      0, 0, 0, 0, h, (HMENU)ID_LOG, hi, nullptr);
  SendMessageW(g_log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
  SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);

  g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
      0, 0, 0, 0, h, (HMENU)ID_STATUS, hi, nullptr);
  SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)L"FPS - | CPU - | GPU -");
  SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)L"GPU: Vulkan (idle)");
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE:
      g_main = h; CreateChildren(h); UpdateUi();
      Log("ps5emu %ls [%ls] - native x86-64 execution, no CPU translation layer", WIDEN(PS5EMU_VERSION), ARCH);
      LoadConfig(); RefreshGames(); UpdateGpuMenu(); UpdatePerfStatus();
      Perf::Sample(0); SetTimer(h, 1, 500, nullptr);
      return 0;
    case WM_TIMER:
      if (w == 1) { Perf::Sample(Emu::Flips()); UpdatePerfStatus(); }
      return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_LOG: FlushLog(); return 0;
    case WM_EMUSTATE: UpdateUi(); return 0;
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case ID_SEARCH: if (HIWORD(w) == EN_CHANGE) ApplyFilter(); break;
        case ID_BOOT: BootFile(); break;
        case ID_ADDDIR: AddDir(); break;
        case ID_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); break;
        case ID_START: case ID_CTX_BOOT: BootSelected(); break;
        case ID_PAUSE: Emu::Pause(Emu::GetState() == Emu::Running); break;
        case ID_STOP: Emu::Stop(); break;
        case ID_REFRESH: RefreshGames(); break;
        case ID_SHOWLOG:
          g_showLog = !g_showLog;
          CheckMenuItem(GetMenu(h), ID_SHOWLOG, g_showLog ? MF_CHECKED : MF_UNCHECKED);
          Layout(); break;
        case ID_CTX_OPEN: { int g = SelectedGame(); if (g >= 0) ShellExecuteW(h, L"explore", g_games[g].dir.c_str(), 0, 0, SW_SHOW); break; }
        case ID_CFG_CPU: ConfigBox(L"CPU", L"Native x86-64 execution (guest code runs directly on the host CPU).\nNo CPU translation layer.\nx86 (32-bit) builds cannot run guests."); break;
        case ID_GPU_VK: case ID_GPU_SW:
          Emu::SetGpuMode(LOWORD(w) == ID_GPU_VK ? 1 : 0); SaveGpuConfig(); UpdateGpuMenu(); UpdatePerfStatus();
          if (Emu::GetState() != Emu::Stopped) Log("GPU backend change applies on next boot");
          break;
        case ID_VSYNC: g_vsyncOpt = !g_vsyncOpt; Emu::SetVsync(g_vsyncOpt); SaveGpuConfig(); UpdateGpuMenu(); break;
        case ID_OVERLAY: g_overlayOpt = !g_overlayOpt; Emu::SetOverlay(g_overlayOpt); SaveGpuConfig(); UpdateGpuMenu(); break;
        case ID_CFG_AUDIO: ConfigBox(L"Audio", L"Not implemented yet."); break;
        case ID_CFG_IO: ConfigBox(L"Input / Output", L"Not implemented yet."); break;
        case ID_OPENLOG: ShellExecuteW(h, L"open", (ExeDir() + L"ps5emu.log").c_str(), 0, 0, SW_SHOW); break;
        case ID_ABOUT: ConfigBox(L"About", L"ps5emu 0.020\nExperimental PS5 emulator: native loader + HLE.\nGPL-friendly, research / education."); break;
      }
      return 0;
    case WM_NOTIFY: {
      NMHDR* nh = (NMHDR*)l;
      if (nh->hwndFrom == g_list) {
        if (nh->code == LVN_GETDISPINFOW) {
          NMLVDISPINFOW* di = (NMLVDISPINFOW*)l;
          if ((di->item.mask & LVIF_TEXT) && di->item.iItem < (int)g_view.size()) {
            const Game& g = g_games[g_view[di->item.iItem]];
            const std::wstring* s = &g.name;
            switch (di->item.iSubItem) {
              case 1: s = &g.serial; break; case 2: s = &g.version; break;
              case 3: s = &g.category; break; case 4: s = &g.path; break;
            }
            di->item.pszText = (LPWSTR)s->c_str();
          }
        } else if (nh->code == NM_DBLCLK) {
          NMITEMACTIVATE* ia = (NMITEMACTIVATE*)l;
          if (ia->iItem >= 0 && ia->iItem < (int)g_view.size()) { SelectedGame(); BootSelected(); }
        } else if (nh->code == NM_RCLICK) {
          NMITEMACTIVATE* ia = (NMITEMACTIVATE*)l;
          if (ia->iItem >= 0) {
            ListView_SetItemState(g_list, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            POINT pt; GetCursorPos(&pt);
            HMENU pm = CreatePopupMenu();
            AppendMenuW(pm, MF_STRING, ID_CTX_BOOT, L"Boot");
            AppendMenuW(pm, MF_STRING, ID_CTX_OPEN, L"Open Game Folder");
            TrackPopupMenu(pm, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(pm);
          }
        }
      }
      return 0;
    }
    case WM_CLOSE:
      KillTimer(h, 1);
      Emu::Stop();
      for (int i = 0; i < 300 && Emu::GetState() != Emu::Stopped; i++) Sleep(10);
      DestroyWindow(h);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
  OpenLogFile();
  SetUnhandledExceptionFilter(CrashFilter);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
  InitCommonControlsEx(&ic);
  WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"ps5emu_main";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  RegisterClassW(&wc);
  std::wstring title = std::wstring(L"ps5emu ") + WIDEN(PS5EMU_VERSION) + L" [" + ARCH + L"]";
  HWND h = CreateWindowExW(0, L"ps5emu_main", title.c_str(), WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700, nullptr, nullptr, hi, nullptr);
  ShowWindow(h, show);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  Log("ps5emu exiting normally");
  return 0;
}
