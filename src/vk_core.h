#pragma once
// vk_core.h - minimal Vulkan plumbing. Functions are loaded dynamically (vulkan-1.dll), so the build needs
// only the vendored headers in third_party/vulkan, not the Vulkan SDK.
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "emu.h"   // Log()

#define VKC_GLOBAL(X) X(vkCreateInstance)

#define VKC_INSTANCE(X) \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
  X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VKC_DEVICE(X) \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
  X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
  X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) \
  X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
  X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
  X(vkBindImageMemory) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
  X(vkCmdBlitImage) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { Log("Vulkan: %s failed (VkResult %d)", #x, (int)r_); return false; } } while (0)

namespace vkc {

#define X(n) inline PFN_##n n = nullptr;
VKC_GLOBAL(X) VKC_INSTANCE(X) VKC_DEVICE(X)
#undef X
#ifdef _WIN32
inline PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
#endif
inline PFN_vkGetInstanceProcAddr g_gipa = nullptr;

inline bool LoadGlobal(PFN_vkGetInstanceProcAddr gipa) {
  g_gipa = gipa;
#define X(n) n = (PFN_##n)gipa(nullptr, #n);
  VKC_GLOBAL(X)
#undef X
  return vkCreateInstance != nullptr;
}
inline void LoadInstance(VkInstance i) {
#define X(n) n = (PFN_##n)g_gipa(i, #n);
  VKC_INSTANCE(X)
#undef X
#ifdef _WIN32
  vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)g_gipa(i, "vkCreateWin32SurfaceKHR");
#endif
}
inline void LoadDevice(VkDevice d) {
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(d, #n);
  VKC_DEVICE(X)
#undef X
}

struct Ctx {
  VkInstance inst = nullptr;
  VkPhysicalDevice phys = nullptr;
  VkDevice dev = nullptr;
  VkQueue queue = nullptr;
  uint32_t qfam = 0;
  VkPhysicalDeviceMemoryProperties mem{};
  char name[256] = "";
  uint32_t apiVer = 0, vendor = 0, devType = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
};

inline int FindMem(const Ctx& c, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < c.mem.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (c.mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

// CPU framebuffer -> staging buffer -> device-local image (the "GPU front buffer")
struct Source {
  VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory bufMem = VK_NULL_HANDLE; uint8_t* map = nullptr;
  VkImage img = VK_NULL_HANDLE; VkDeviceMemory imgMem = VK_NULL_HANDLE;
  uint32_t w = 0, h = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

inline bool CreateSource(Ctx& c, Source& s, uint32_t w, uint32_t h) {
  s.w = w; s.h = h; s.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = (VkDeviceSize)w * h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK(vkCreateBuffer(c.dev, &bi, nullptr, &s.buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c.dev, s.buf, &mr);
  int mt = FindMem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mt < 0) { Log("Vulkan: no host-visible coherent memory type"); return false; }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)mt;
  VK(vkAllocateMemory(c.dev, &ai, nullptr, &s.bufMem));
  VK(vkBindBufferMemory(c.dev, s.buf, s.bufMem, 0));
  void* p = nullptr; VK(vkMapMemory(c.dev, s.bufMem, 0, VK_WHOLE_SIZE, 0, &p)); s.map = (uint8_t*)p;

  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM; ii.extent = {w, h, 1};
  ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK(vkCreateImage(c.dev, &ii, nullptr, &s.img));
  vkGetImageMemoryRequirements(c.dev, s.img, &mr);
  mt = FindMem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mt < 0) mt = FindMem(c, mr.memoryTypeBits, 0);
  if (mt < 0) { Log("Vulkan: no memory type for image"); return false; }
  ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)mt;
  VK(vkAllocateMemory(c.dev, &ai, nullptr, &s.imgMem));
  VK(vkBindImageMemory(c.dev, s.img, s.imgMem, 0));
  return true;
}

inline void DestroySource(Ctx& c, Source& s) {
  if (!c.dev) return;
  if (s.map) { vkUnmapMemory(c.dev, s.bufMem); s.map = nullptr; }
  if (s.buf) vkDestroyBuffer(c.dev, s.buf, nullptr);
  if (s.bufMem) vkFreeMemory(c.dev, s.bufMem, nullptr);
  if (s.img) vkDestroyImage(c.dev, s.img, nullptr);
  if (s.imgMem) vkFreeMemory(c.dev, s.imgMem, nullptr);
  s = Source();
}

inline void Barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// staging buffer -> source image -> (scaled blit) -> dst image, leaving dst in dstFinal layout
inline void RecordUploadAndBlit(VkCommandBuffer cmd, Source& s, VkImage dst, uint32_t dw, uint32_t dh,
                                VkImageLayout dstFinal, VkAccessFlags dstFinalAccess, VkPipelineStageFlags dstFinalStage) {
  const VkPipelineStageFlags T = VK_PIPELINE_STAGE_TRANSFER_BIT;
  Barrier(cmd, s.img, s.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          s.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, T, T);
  VkBufferImageCopy bc{};
  bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bc.imageExtent = {s.w, s.h, 1};
  vkCmdCopyBufferToImage(cmd, s.buf, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  Barrier(cmd, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, T, T);
  s.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, T);
  VkImageBlit bl{};
  bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bl.srcOffsets[1] = {(int32_t)s.w, (int32_t)s.h, 1};
  bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  bl.dstOffsets[1] = {(int32_t)dw, (int32_t)dh, 1};
  vkCmdBlitImage(cmd, s.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &bl, VK_FILTER_LINEAR);
  Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstFinal,
          VK_ACCESS_TRANSFER_WRITE_BIT, dstFinalAccess, T, dstFinalStage);
}

} // namespace vkc
