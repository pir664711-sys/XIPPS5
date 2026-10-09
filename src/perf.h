#pragma once
#include <cstdint>
#include <string>

struct PerfStats {
  double fps = 0;     // guest frames submitted per second
  double cpu = 0;     // emulator process CPU, % of total machine CPU
  double gpu = -1;    // emulator process GPU engine utilisation %, -1 = unavailable
};

namespace Perf {
  void Sample(uint64_t flipsNow);       // call ~2x per second from one thread
  PerfStats Get();                      // thread-safe snapshot
  void SetGpuDesc(const std::string&);  // e.g. "Vulkan 1.3: NVIDIA GeForce ..."
  std::string GpuDesc();
}
