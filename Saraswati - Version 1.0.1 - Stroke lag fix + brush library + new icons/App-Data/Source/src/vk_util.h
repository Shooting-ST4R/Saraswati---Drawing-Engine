// Small Vulkan helpers: error checks, memory types, images, buffers, barriers.
#pragma once
#include <volk.h>
#include <cstdint>
#include <stdexcept>
#include <string>

struct VkFatal : std::runtime_error {
  VkResult result;
  VkFatal(const std::string& what, VkResult r) : std::runtime_error(what), result(r) {}
};

const char* vkResultName(VkResult r);
void vkCheck(VkResult r, const char* what);
#define VK_CHECK(x) vkCheck((x), #x)

uint32_t findMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t typeBits,
                        VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  VkDeviceSize bytes = 0;
};

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
};

// Device-local 2D image with its own dedicated allocation. Returns the failing
// VkResult (e.g. VK_ERROR_OUT_OF_DEVICE_MEMORY) instead of throwing, leaving `out` empty.
VkResult createImage(VkDevice dev, const VkPhysicalDeviceMemoryProperties& mem, uint32_t w, uint32_t h,
                     VkFormat fmt, VkImageUsageFlags usage, GpuImage& out);
void destroyImage(VkDevice dev, GpuImage& img);

VkResult createBuffer(VkDevice dev, const VkPhysicalDeviceMemoryProperties& mem, VkDeviceSize size,
                      VkBufferUsageFlags usage, VkMemoryPropertyFlags props, GpuBuffer& out, bool map);
void destroyBuffer(VkDevice dev, GpuBuffer& buf);

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dst, VkAccessFlags dstAccess);
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                  VkAccessFlags dstAccess);
VkShaderModule createShader(VkDevice dev, const uint32_t* code, size_t bytes);
