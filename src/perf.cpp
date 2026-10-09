// perf.cpp - FPS (guest flips/s), process CPU % (GetProcessTimes), process GPU % (PDH "GPU Engine" counters).
#include "perf.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <mutex>
#include <set>
#include <vector>
#include <cstdio>
#include <cwchar>

namespace {
std::mutex g_mx;
PerfStats g_stats;
std::string g_gpuDesc;

// CPU / FPS state
bool g_init = false;
LARGE_INTEGER g_freq, g_lastQpc;
uint64_t g_lastProc = 0, g_lastFlips = 0;
DWORD g_ncpu = 1;

// GPU (PDH) state
PDH_HQUERY g_q = nullptr;
std::vector<PDH_HCOUNTER> g_ctrs;
std::set<std::wstring> g_paths;
ULONGLONG g_lastExpand = 0;

uint64_t ProcTime100ns() {
  FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

void ExpandGpuCounters() {
  if (!g_q && PdhOpenQueryW(nullptr, 0, &g_q) != ERROR_SUCCESS) { g_q = nullptr; return; }
  wchar_t pat[160];
  swprintf(pat, 160, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
  DWORD len = 0;
  PDH_STATUS st = PdhExpandWildCardPathW(nullptr, pat, nullptr, &len, 0);
  if ((DWORD)st != (DWORD)PDH_MORE_DATA || !len) return;   // no GPU counters yet (or localized Windows) -> GPU % stays n/a
  std::vector<wchar_t> buf(len);
  if (PdhExpandWildCardPathW(nullptr, pat, buf.data(), &len, 0) != ERROR_SUCCESS) return;
  bool added = false;
  for (wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
    if (g_paths.count(p)) continue;
    PDH_HCOUNTER h;
    if (PdhAddCounterW(g_q, p, 0, &h) == ERROR_SUCCESS) { g_ctrs.push_back(h); g_paths.insert(p); added = true; }
  }
  if (added) PdhCollectQueryData(g_q);   // baseline for rate counters
}

double SampleGpu() {
  ULONGLONG now = GetTickCount64();
  if (now - g_lastExpand > 3000) { g_lastExpand = now; ExpandGpuCounters(); }   // instances appear once the process uses the GPU
  if (!g_q || g_ctrs.empty()) return -1;
  if (PdhCollectQueryData(g_q) != ERROR_SUCCESS) return -1;
  double mx = 0; bool any = false;
  for (auto h : g_ctrs) {
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(h, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS && v.CStatus == PDH_CSTATUS_VALID_DATA) {
      any = true; if (v.doubleValue > mx) mx = v.doubleValue;   // busiest engine, like Task Manager
    }
  }
  return any ? (mx > 100 ? 100 : mx) : -1;
}
}

namespace Perf {

void Sample(uint64_t flipsNow) {
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  uint64_t proc = ProcTime100ns();
  if (!g_init) {
    QueryPerformanceFrequency(&g_freq);
    SYSTEM_INFO si; GetSystemInfo(&si); g_ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    g_lastQpc = now; g_lastProc = proc; g_lastFlips = flipsNow; g_init = true;
    return;
  }
  double dt = (double)(now.QuadPart - g_lastQpc.QuadPart) / (double)g_freq.QuadPart;
  if (dt < 0.05) return;
  PerfStats s;
  s.fps = flipsNow >= g_lastFlips ? (double)(flipsNow - g_lastFlips) / dt : 0;
  s.cpu = (double)(proc - g_lastProc) / (dt * 1e7 * g_ncpu) * 100.0;
  if (s.cpu > 100) s.cpu = 100;
  if (s.cpu < 0) s.cpu = 0;
  s.gpu = SampleGpu();
  g_lastQpc = now; g_lastProc = proc; g_lastFlips = flipsNow;
  std::lock_guard<std::mutex> l(g_mx); g_stats = s;
}

PerfStats Get() { std::lock_guard<std::mutex> l(g_mx); return g_stats; }
void SetGpuDesc(const std::string& d) { std::lock_guard<std::mutex> l(g_mx); g_gpuDesc = d; }
std::string GpuDesc() { std::lock_guard<std::mutex> l(g_mx); return g_gpuDesc; }

}
