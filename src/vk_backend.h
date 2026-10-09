#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Vulkan GPU backend (0.014): presents the guest framebuffer through a Vulkan swapchain.
// Loads vulkan-1.dll at runtime; Init() fails cleanly (caller falls back to software) if unavailable.
struct VkPresenter {
  bool Init(HWND hwnd, bool vsync, int fbW, int fbH);
  // post: optional hook run on the mapped staging pixels (used for the performance overlay)
  bool Present(const uint32_t* fb, int w, int h, void (*post)(uint32_t*, int, int));
  void Shutdown();
  void SetVsync(bool v);
  std::string Desc() const;
  struct Impl;
  Impl* p = nullptr;
};
