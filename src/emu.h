#pragma once
#include <string>
#include <cstdint>

void Log(const char* fmt, ...);   // thread-safe, goes to UI log panel (defined in main.cpp)
void UiOnEmuState();              // thread-safe, tells UI emu state changed (defined in main.cpp)

struct Emu {
  enum State { Stopped = 0, Running, Paused };
  static bool  Boot(const std::wstring& path, const std::wstring& title);
  static void  Pause(bool p);
  static void  Stop();
  static State GetState();
  // GPU / perf settings (0.014)
  static void     SetGpuMode(int mode);   // 1 = Vulkan (default), 0 = software fallback; applies on next boot
  static int      GetGpuMode();
  static void     SetVsync(bool on);      // live
  static void     SetOverlay(bool on);    // live
  static uint64_t Flips();                // total guest frames submitted since boot
};

std::string EmuDescribeAddr(uintptr_t addr);   // "guest vaddr 0x..", "module+0x.." for crash logs
