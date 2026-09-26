// Renderer support for the non-brush tools: coverage painting (fill, shapes, delete),
// gradients, selection upload, region readback, floating images and the transform stamp.
#include "renderer.h"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>

static constexpr VkPipelineStageFlags kCS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
static constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
static constexpr VkShaderStageFlags kPcStages =
    VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

std::vector<VkRect2D> Renderer::tilesForRect(int x0, int y0, int x1, int y1) const {
  std::vector<VkRect2D> out;
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, int(docW)); y1 = std::min(y1, int(docH));
  if (x0 >= x1 || y0 >= y1) return out;
  for (uint32_t ty = uint32_t(y0) / kTile; ty <= uint32_t(y1 - 1) / kTile; ++ty)
    for (uint32_t tx = uint32_t(x0) / kTile; tx <= uint32_t(x1 - 1) / kTile; ++tx) {
      VkRect2D r;
      r.offset = {int32_t(tx * kTile), int32_t(ty * kTile)};
      r.extent = {std::min(kTile, docW - tx * kTile), std::min(kTile, docH - ty * kTile)};
      out.push_back(r);
    }
  return out;
}

void Renderer::markDirtyRect(int x0, int y0, int x1, int y1) {
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, int(docW)); y1 = std::min(y1, int(docH));
  if (x0 >= x1 || y0 >= y1) return;
  sx0 = std::min(sx0, x0); sy0 = std::min(sy0, y0);
  sx1 = std::max(sx1, x1); sy1 = std::max(sy1, y1);
  for (uint32_t ty = uint32_t(y0) / kTile; ty <= uint32_t(y1 - 1) / kTile; ++ty)
    for (uint32_t tx = uint32_t(x0) / kTile; tx <= uint32_t(x1 - 1) / kTile; ++tx) {
      uint32_t t = ty * tilesX + tx;
      if (!tileDirty[t]) { tileDirty[t] = 1; dirtyTiles.push_back(t); }
    }
}

bool Renderer::uploadRegion(VkImage img, int x, int y, uint32_t w, uint32_t h, uint32_t bpp,
                            const std::function<void(uint32_t, uint8_t*)>& fillRow, std::string& err) {
  if (!w || !h) return true;
  const VkDeviceSize maxStage = 64ull << 20;
  uint32_t rows = uint32_t(std::clamp<VkDeviceSize>(maxStage / (VkDeviceSize(w) * bpp), 1, h));
  GpuBuffer stage;
  VkResult r = createBuffer(device, memProps, VkDeviceSize(w) * bpp * rows, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage, true);
  if (r != VK_SUCCESS) { err = std::string("Staging buffer: ") + vkResultName(r); return false; }
  for (uint32_t y0 = 0; y0 < h; y0 += rows) {
    uint32_t n = std::min(rows, h - y0);
    for (uint32_t k = 0; k < n; ++k) fillRow(y0 + k, (uint8_t*)stage.mapped + size_t(k) * w * bpp);
    VkCommandBuffer cmd = beginOneShot();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y + int32_t(y0), 0};
    c.imageExtent = {w, n, 1};
    vkCmdCopyBufferToImage(cmd, stage.buffer, img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    endOneShot(cmd);
  }
  destroyBuffer(device, stage);
  return true;
}

bool Renderer::paintCoverage(int layer, int x, int y, uint32_t w, uint32_t h, const uint8_t* cov,
                             const StrokeStyle& st, std::string& err) {
  if (strokeActive || layer < 0 || layer >= int(layers.size())) return false;
  // clip to the document
  int x0 = std::max(x, 0), y0 = std::max(y, 0);
  int x1 = std::min<int64_t>(int64_t(x) + w, docW), y1 = std::min<int64_t>(int64_t(y) + h, docH);
  if (x0 >= x1 || y0 >= y1) return true;
  uint32_t cw = uint32_t(x1 - x0), ch = uint32_t(y1 - y0);
  bool ok = uploadRegion(mask.image, x0, y0, cw, ch, maskR16 ? 2 : 4, [&](uint32_t row, uint8_t* dst) {
    const uint8_t* src = cov + (size_t(y0 - y + int(row)) * w + size_t(x0 - x));
    if (maskR16) {
      uint16_t* d = reinterpret_cast<uint16_t*>(dst);
      for (uint32_t i = 0; i < cw; ++i) d[i] = uint16_t(src[i] * 257);
    } else {
      float* d = reinterpret_cast<float*>(dst);
      for (uint32_t i = 0; i < cw; ++i) d[i] = src[i] / 255.0f;
    }
  }, err);
  if (!ok) return false;
  beginStroke(layer, st);
  markDirtyRect(x0, y0, x1, y1);
  strokeEnding = true;  // committed (with undo) by the next frame
  return true;
}

bool Renderer::paintGradient(int layer, float gx0, float gy0, float gx1, float gy1, int rx0, int ry0, int rx1, int ry1,
                             const StrokeStyle& st) {
  if (strokeActive || layer < 0 || layer >= int(layers.size())) return false;
  rx0 = std::max(rx0, 0); ry0 = std::max(ry0, 0);
  rx1 = std::min(rx1, int(docW)); ry1 = std::min(ry1, int(docH));
  if (rx0 >= rx1 || ry0 >= ry1) return false;
  struct { int32_t origin[2], size[2]; float p0[2], p1[2]; } pc{{rx0, ry0}, {rx1 - rx0, ry1 - ry0}, {gx0, gy0}, {gx1, gy1}};
  VkCommandBuffer cmd = beginOneShot();
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradPipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &slots[0].set0, 0, nullptr);
  vkCmdPushConstants(cmd, pipeLayout, kPcStages, 0, sizeof pc, &pc);
  vkCmdDispatch(cmd, uint32_t(pc.size[0] + 15) / 16, uint32_t(pc.size[1] + 15) / 16, 1);
  endOneShot(cmd);
  beginStroke(layer, st);
  markDirtyRect(rx0, ry0, rx1, ry1);
  strokeEnding = true;
  return true;
}

bool Renderer::uploadSelection(const uint8_t* s, int x, int y, uint32_t w, uint32_t h, std::string& err) {
  x = std::max(x, 0); y = std::max(y, 0);
  w = std::min<uint32_t>(w, docW - uint32_t(x));
  h = std::min<uint32_t>(h, docH - uint32_t(y));
  return uploadRegion(sel.image, x, y, w, h, maskR16 ? 1 : 4, [&](uint32_t row, uint8_t* dst) {
    const uint8_t* src = s + size_t(y + int(row)) * docW + size_t(x);
    if (maskR16) memcpy(dst, src, w);
    else {
      float* d = reinterpret_cast<float*>(dst);
      for (uint32_t i = 0; i < w; ++i) d[i] = src[i] / 255.0f;
    }
  }, err);
}

bool Renderer::readLayerRegion(int index, int x, int y, uint32_t w, uint32_t h, std::vector<uint8_t>& out, std::string& err) {
  if (index < 0 || index >= int(layers.size()) || !w || !h) return false;
  out.resize(size_t(w) * h * 4);
  const VkDeviceSize maxStage = 64ull << 20;
  uint32_t rows = uint32_t(std::clamp<VkDeviceSize>(maxStage / (VkDeviceSize(w) * 4), 1, h));
  GpuBuffer stage;
  VkResult r = createBuffer(device, memProps, VkDeviceSize(w) * 4 * rows, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stage, true);
  if (r != VK_SUCCESS) { err = std::string("Staging buffer: ") + vkResultName(r); return false; }
  for (uint32_t y0 = 0; y0 < h; y0 += rows) {
    uint32_t n = std::min(rows, h - y0);
    VkCommandBuffer cmd = beginOneShot();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y + int32_t(y0), 0};
    c.imageExtent = {w, n, 1};
    vkCmdCopyImageToBuffer(cmd, layers[index].image.image, VK_IMAGE_LAYOUT_GENERAL, stage.buffer, 1, &c);
    endOneShot(cmd);
    memcpy(out.data() + size_t(y0) * w * 4, stage.mapped, size_t(n) * w * 4);
  }
  destroyBuffer(device, stage);
  return true;
}

bool Renderer::createFloating(uint32_t w, uint32_t h, const uint8_t* rgba, Floating& f, std::string& err) {
  f = {};
  VkResult r = createImage(device, memProps, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, f.image);
  if (r != VK_SUCCESS) { err = std::string("Floating image: ") + vkResultName(r); return false; }
  VkCommandBuffer cmd = beginOneShot();
  imageBarrier(cmd, f.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  endOneShot(cmd);
  if (!uploadRegion(f.image.image, 0, 0, w, h, 4, [&](uint32_t row, uint8_t* dst) {
        memcpy(dst, rgba + size_t(row) * w * 4, size_t(w) * 4);
      }, err)) {
    destroyImage(device, f.image);
    return false;
  }
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set1Layout;
  if (vkAllocateDescriptorSets(device, &dai, &f.set) != VK_SUCCESS) { destroyImage(device, f.image); err = "descriptor"; return false; }
  VkDescriptorImageInfo ii{VK_NULL_HANDLE, f.image.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wr.dstSet = f.set;
  wr.descriptorCount = 1;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  wr.pImageInfo = &ii;
  vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
  f.imguiTex = ImGui_ImplVulkan_AddTexture(f.image.view, VK_IMAGE_LAYOUT_GENERAL);
  f.w = w;
  f.h = h;
  return true;
}

void Renderer::destroyFloating(Floating& f) {
  if (!f.image.image) return;
  waitIdle();
  if (f.imguiTex) ImGui_ImplVulkan_RemoveTexture(f.imguiTex);
  if (f.set) vkFreeDescriptorSets(device, descPool, 1, &f.set);
  destroyImage(device, f.image);
  f = {};
}

bool Renderer::stamp(int layer, const Floating& f, const double inv[9], int rx0, int ry0, int rx1, int ry1, std::string& err) {
  if (layer < 0 || layer >= int(layers.size()) || strokeActive) return false;
  rx0 = std::max(rx0, 0); ry0 = std::max(ry0, 0);
  rx1 = std::min(rx1, int(docW)); ry1 = std::min(ry1, int(docH));
  if (rx0 >= rx1 || ry0 >= ry1) return true;
  Layer& L = layers[layer];
  UndoEntry e;
  e.layerId = L.id;
  e.tiles = tilesForRect(rx0, ry0, rx1, ry1);
  VkCommandBuffer cmd = beginOneShot();
  bool haveUndo = copyTiles(cmd, L, e.tiles, e.chunks, e.bytes, true);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, kCS, kRW);
  struct { int32_t origin[2], size[2]; float h0[4], h1[4], h2[4]; } pc{};
  pc.origin[0] = rx0; pc.origin[1] = ry0;
  pc.size[0] = rx1 - rx0; pc.size[1] = ry1 - ry0;
  for (int k = 0; k < 3; ++k) {
    pc.h0[k] = float(inv[k]);
    pc.h1[k] = float(inv[3 + k]);
    pc.h2[k] = float(inv[6 + k]);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, stampPipe);
  VkDescriptorSet sets[3] = {slots[0].set0, L.set, f.set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 3, sets, 0, nullptr);
  vkCmdPushConstants(cmd, pipeLayout, kPcStages, 0, sizeof pc, &pc);
  vkCmdDispatch(cmd, uint32_t(pc.size[0] + 15) / 16, uint32_t(pc.size[1] + 15) / 16, 1);
  endOneShot(cmd);
  if (haveUndo) pushUndo(std::move(e));
  else err = "Not enough memory to keep undo for the transform.";
  cachesDirty = true;
  return true;
}
