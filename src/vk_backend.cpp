#include "vk_backend.h"
#include "vk_core.h"
#include <algorithm>

using namespace vkc;

struct VkPresenter::Impl {
  HMODULE lib = nullptr;
  HWND hwnd = nullptr;
  Ctx c;
  VkSurfaceKHR surf = VK_NULL_HANDLE;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  std::vector<VkImage> imgs;
  std::vector<VkSemaphore> renderDone;
  VkExtent2D ext{};
  VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkCommandBuffer cmd = nullptr;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore acq = VK_NULL_HANDLE;
  Source src;
  bool vsync = true, dirty = false;
  std::string desc;
};

static void DestroySwapchainObjects(VkPresenter::Impl& I) {
  for (auto s : I.renderDone) vkDestroySemaphore(I.c.dev, s, nullptr);
  I.renderDone.clear(); I.imgs.clear();
}

static bool MakeSwapchain(VkPresenter::Impl& I) {
  Ctx& c = I.c;
  VK(vkDeviceWaitIdle(c.dev));
  VkSurfaceCapabilitiesKHR caps;
  VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, I.surf, &caps));
  VkExtent2D e = caps.currentExtent;
  if (e.width == 0xFFFFFFFFu) {
    RECT rc; GetClientRect(I.hwnd, &rc);
    e.width = std::clamp<uint32_t>((uint32_t)rc.right, caps.minImageExtent.width, caps.maxImageExtent.width);
    e.height = std::clamp<uint32_t>((uint32_t)rc.bottom, caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  if (e.width == 0 || e.height == 0) return true;   // minimised: keep old state, retry next frame
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { Log("Vulkan: surface cannot be a transfer target"); return false; }

  uint32_t nf = 0; VK(vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, I.surf, &nf, nullptr));
  std::vector<VkSurfaceFormatKHR> fmts(nf); VK(vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, I.surf, &nf, fmts.data()));
  bool found = false;
  for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { I.colorSpace = f.colorSpace; found = true; break; }
  if (!found) { Log("Vulkan: surface has no B8G8R8A8_UNORM format"); return false; }

  uint32_t nm = 0; VK(vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, I.surf, &nm, nullptr));
  std::vector<VkPresentModeKHR> modes(nm); VK(vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, I.surf, &nm, modes.data()));
  VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;   // FIFO = vsync, always available
  if (!I.vsync) {
    for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { pm = m; break; }
    if (pm == VK_PRESENT_MODE_FIFO_KHR) for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }
  }
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = I.surf; sci.minImageCount = count; sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; sci.imageColorSpace = I.colorSpace;
  sci.imageExtent = e; sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                        : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  sci.presentMode = pm; sci.clipped = VK_TRUE; sci.oldSwapchain = I.sc;
  VkSwapchainKHR ns = VK_NULL_HANDLE;
  VK(vkCreateSwapchainKHR(c.dev, &sci, nullptr, &ns));
  DestroySwapchainObjects(I);
  if (I.sc) vkDestroySwapchainKHR(c.dev, I.sc, nullptr);
  I.sc = ns; I.ext = e;

  uint32_t ni = 0; VK(vkGetSwapchainImagesKHR(c.dev, I.sc, &ni, nullptr));
  I.imgs.resize(ni); VK(vkGetSwapchainImagesKHR(c.dev, I.sc, &ni, I.imgs.data()));
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < ni; i++) { VkSemaphore s; VK(vkCreateSemaphore(c.dev, &semi, nullptr, &s)); I.renderDone.push_back(s); }
  I.dirty = false;
  return true;
}

bool VkPresenter::Init(HWND hwnd, bool vsync, int fbW, int fbH) {
  p = new Impl; Impl& I = *p; I.hwnd = hwnd; I.vsync = vsync;
  I.lib = LoadLibraryA("vulkan-1.dll");
  if (!I.lib) { Log("Vulkan: vulkan-1.dll not found (install or update your GPU driver)"); return false; }
  auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(I.lib, "vkGetInstanceProcAddr");
  if (!gipa || !LoadGlobal(gipa)) { Log("Vulkan: loader entry points missing"); return false; }

  Ctx& c = I.c;
  // This release intentionally requires Vulkan 1.1+. The loader and selected physical
  // device are checked separately because a 1.1 loader can expose a 1.0-only GPU.
  auto enumInstanceVersion = (PFN_vkEnumerateInstanceVersion)gipa(nullptr, "vkEnumerateInstanceVersion");
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (!enumInstanceVersion || enumInstanceVersion(&loaderVersion) != VK_SUCCESS) {
    Log("Vulkan: cannot verify loader version; Vulkan 1.1 is required"); return false;
  }
  if (loaderVersion < VK_API_VERSION_1_1) {
    Log("Vulkan: loader supports only %u.%u; ps5emu 0.017 requires Vulkan 1.1+",
        VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion)); return false;
  }
  const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName = "ps5emu"; ai.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
  VK(vkCreateInstance(&ici, nullptr, &c.inst));
  LoadInstance(c.inst);

  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = GetModuleHandleW(nullptr); sci.hwnd = hwnd;
  VK(vkCreateWin32SurfaceKHR(c.inst, &sci, nullptr, &I.surf));

  uint32_t nd = 0; VK(vkEnumeratePhysicalDevices(c.inst, &nd, nullptr));
  if (!nd) { Log("Vulkan: no Vulkan-capable GPU found"); return false; }
  std::vector<VkPhysicalDevice> pds(nd); VK(vkEnumeratePhysicalDevices(c.inst, &nd, pds.data()));
  int bestScore = -1; uint32_t bestQ = 0; VkPhysicalDevice best = nullptr;
  for (auto d : pds) {
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, ex.data());
    bool sw = false; for (auto& x : ex) if (!strcmp(x.extensionName, "VK_KHR_swapchain")) sw = true;
    if (!sw) continue;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qp(nq); vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qp.data());
    int q = -1;
    for (uint32_t i = 0; i < nq; i++) {
      VkBool32 sup = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(d, i, I.surf, &sup);
      if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { q = (int)i; break; }
    }
    if (q < 0) continue;
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(d, &pr);
    if (pr.apiVersion < VK_API_VERSION_1_1) {
      Log("Vulkan: skipping %s (supports %u.%u; need 1.1+)", pr.deviceName,
          VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion));
      continue;
    }
    int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
              : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : 1;
    if (score > bestScore) { bestScore = score; best = d; bestQ = (uint32_t)q; }
  }
  if (!best) { Log("Vulkan: no Vulkan 1.1+ GPU with graphics+present+swapchain support"); return false; }
  c.phys = best; c.qfam = bestQ;
  VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(c.phys, &pr);
  snprintf(c.name, sizeof c.name, "%s", pr.deviceName); c.apiVer = pr.apiVersion; c.vendor = pr.vendorID; c.devType = pr.deviceType;
  vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);
  VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(c.phys, VK_FORMAT_B8G8R8A8_UNORM, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((fp.optimalTilingFeatures & need) != need) { Log("Vulkan: GPU lacks blit support for B8G8R8A8"); return false; }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = c.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  const char* dext[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
  VK(vkCreateDevice(c.phys, &dci, nullptr, &c.dev));
  LoadDevice(c.dev);
  vkGetDeviceQueue(c.dev, c.qfam, 0, &c.queue);

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = c.qfam;
  VK(vkCreateCommandPool(c.dev, &cpi, nullptr, &c.pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = c.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VK(vkAllocateCommandBuffers(c.dev, &cai, &I.cmd));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK(vkCreateFence(c.dev, &fi, nullptr, &I.fence));
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VK(vkCreateSemaphore(c.dev, &semi, nullptr, &I.acq));

  if (!CreateSource(c, I.src, (uint32_t)fbW, (uint32_t)fbH)) return false;
  if (!MakeSwapchain(I)) return false;

  char d[400];
  snprintf(d, sizeof d, "Vulkan %u.%u: %s", pr.apiVersion >> 22, (pr.apiVersion >> 12) & 0x3FF, c.name);
  I.desc = d;
  Log("Vulkan ready: %s (vendor 0x%04x, %s, %s)", c.name, c.vendor,
      c.devType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : c.devType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : "other",
      I.vsync ? "vsync on" : "vsync off");
  return true;
}

bool VkPresenter::Present(const uint32_t* fb, int w, int h, void (*post)(uint32_t*, int, int)) {
  if (!p) return false;
  Impl& I = *p; Ctx& c = I.c;
  if ((uint32_t)w != I.src.w || (uint32_t)h != I.src.h) return false;
  RECT rc; GetClientRect(I.hwnd, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) { Sleep(16); return true; }   // minimised
  if (!I.sc || I.dirty || (uint32_t)rc.right != I.ext.width || (uint32_t)rc.bottom != I.ext.height)
    if (!MakeSwapchain(I)) return false;
  if (!I.sc) { Sleep(16); return true; }

  VK(vkWaitForFences(c.dev, 1, &I.fence, VK_TRUE, UINT64_MAX));
  uint32_t idx = 0;
  VkResult r = vkAcquireNextImageKHR(c.dev, I.sc, UINT64_MAX, I.acq, VK_NULL_HANDLE, &idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) { I.dirty = true; return true; }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { Log("Vulkan: acquire failed (%d)", (int)r); return false; }
  VK(vkResetFences(c.dev, 1, &I.fence));

  memcpy(I.src.map, fb, (size_t)w * h * 4);
  if (post) post((uint32_t*)I.src.map, w, h);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK(vkResetCommandBuffer(I.cmd, 0));
  VK(vkBeginCommandBuffer(I.cmd, &bi));
  RecordUploadAndBlit(I.cmd, I.src, I.imgs[idx], I.ext.width, I.ext.height,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  VK(vkEndCommandBuffer(I.cmd));

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1; si.pWaitSemaphores = &I.acq; si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1; si.pCommandBuffers = &I.cmd;
  si.signalSemaphoreCount = 1; si.pSignalSemaphores = &I.renderDone[idx];
  VK(vkQueueSubmit(c.queue, 1, &si, I.fence));

  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &I.renderDone[idx];
  pi.swapchainCount = 1; pi.pSwapchains = &I.sc; pi.pImageIndices = &idx;
  r = vkQueuePresentKHR(c.queue, &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) I.dirty = true;
  else if (r != VK_SUCCESS) { Log("Vulkan: present failed (%d)", (int)r); return false; }
  return true;
}

void VkPresenter::SetVsync(bool v) { if (p && p->vsync != v) { p->vsync = v; p->dirty = true; } }

std::string VkPresenter::Desc() const { return p ? p->desc : std::string(); }

void VkPresenter::Shutdown() {
  if (!p) return;
  Impl& I = *p; Ctx& c = I.c;
  if (c.dev) {
    vkDeviceWaitIdle(c.dev);
    DestroySwapchainObjects(I);
    if (I.sc) vkDestroySwapchainKHR(c.dev, I.sc, nullptr);
    if (I.acq) vkDestroySemaphore(c.dev, I.acq, nullptr);
    if (I.fence) vkDestroyFence(c.dev, I.fence, nullptr);
    DestroySource(c, I.src);
    if (c.pool) vkDestroyCommandPool(c.dev, c.pool, nullptr);
    vkDestroyDevice(c.dev, nullptr);
  }
  if (c.inst) {
    if (I.surf) vkDestroySurfaceKHR(c.inst, I.surf, nullptr);
    vkDestroyInstance(c.inst, nullptr);
  }
  // vulkan-1.dll intentionally not unloaded (some drivers crash on unload)
  delete p; p = nullptr;
}
