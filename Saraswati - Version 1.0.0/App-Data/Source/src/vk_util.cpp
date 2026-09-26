#include "vk_util.h"

const char* vkResultName(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult(other)";
  }
}

void vkCheck(VkResult r, const char* what) {
  if (r < 0) throw VkFatal(std::string(what) + " failed: " + vkResultName(r), r);
}

uint32_t findMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t typeBits,
                        VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
  for (int pass = 0; pass < 2; ++pass)
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
      if (!(typeBits & (1u << i)) || (f & want) != want) continue;
      if (pass == 0 && (f & avoid)) continue;
      return i;
    }
  return UINT32_MAX;
}

VkResult createImage(VkDevice dev, const VkPhysicalDeviceMemoryProperties& mem, uint32_t w, uint32_t h,
                     VkFormat fmt, VkImageUsageFlags usage, GpuImage& out) {
  out = {};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = fmt;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult r = vkCreateImage(dev, &ci, nullptr, &out.image);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(dev, out.image, &req);
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = out.image;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = findMemoryType(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = findMemoryType(mem, req.memoryTypeBits, 0);
  r = vkAllocateMemory(dev, &ai, nullptr, &out.memory);
  if (r != VK_SUCCESS) { destroyImage(dev, out); return r; }
  r = vkBindImageMemory(dev, out.image, out.memory, 0);
  if (r != VK_SUCCESS) { destroyImage(dev, out); return r; }
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = out.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  r = vkCreateImageView(dev, &vi, nullptr, &out.view);
  if (r != VK_SUCCESS) { destroyImage(dev, out); return r; }
  out.format = fmt;
  out.width = w;
  out.height = h;
  out.bytes = req.size;
  return VK_SUCCESS;
}

void destroyImage(VkDevice dev, GpuImage& img) {
  if (img.view) vkDestroyImageView(dev, img.view, nullptr);
  if (img.image) vkDestroyImage(dev, img.image, nullptr);
  if (img.memory) vkFreeMemory(dev, img.memory, nullptr);
  img = {};
}

VkResult createBuffer(VkDevice dev, const VkPhysicalDeviceMemoryProperties& mem, VkDeviceSize size,
                      VkBufferUsageFlags usage, VkMemoryPropertyFlags props, GpuBuffer& out, bool map) {
  out = {};
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  VkResult r = vkCreateBuffer(dev, &ci, nullptr, &out.buffer);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, out.buffer, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  // Host-visible buffers should live in system RAM, not in (scarce) device-local memory.
  ai.memoryTypeIndex = findMemoryType(mem, req.memoryTypeBits, props,
                                      (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
  if (ai.memoryTypeIndex == UINT32_MAX) { destroyBuffer(dev, out); return VK_ERROR_FEATURE_NOT_PRESENT; }
  r = vkAllocateMemory(dev, &ai, nullptr, &out.memory);
  if (r != VK_SUCCESS) { destroyBuffer(dev, out); return r; }
  r = vkBindBufferMemory(dev, out.buffer, out.memory, 0);
  if (r == VK_SUCCESS && map) r = vkMapMemory(dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
  if (r != VK_SUCCESS) { destroyBuffer(dev, out); return r; }
  out.size = size;
  return VK_SUCCESS;
}

void destroyBuffer(VkDevice dev, GpuBuffer& buf) {
  if (buf.buffer) vkDestroyBuffer(dev, buf.buffer, nullptr);
  if (buf.memory) vkFreeMemory(dev, buf.memory, nullptr);
  buf = {};
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                  VkAccessFlags dstAccess) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkShaderModule createShader(VkDevice dev, const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m;
  VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
  return m;
}
