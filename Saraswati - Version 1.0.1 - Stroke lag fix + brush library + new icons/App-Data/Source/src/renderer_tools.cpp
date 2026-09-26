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
  if (strokeActive || layer < 0 || layer >= int(layers.size()) || layers[size_t(layer)].folder) return false;
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
  if (index < 0 || index >= int(layers.size()) || !w || !h || layers[size_t(index)].folder) return false;
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
  if (layer < 0 || layer >= int(layers.size()) || strokeActive || layers[size_t(layer)].folder) return false;
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
  growBounds(L, rx0, ry0, rx1, ry1);
  cachesDirty = true;
  if (!haveUndo) bumpRevision();
  return true;
}

// Renders at most one dirty layer thumbnail (max side 64 px) into this frame's command buffer,
// using the cache shader in document space. No extra submissions and no GPU waits.
void Renderer::recordThumbnails(VkCommandBuffer cmd) {
  uint64_t now = SDL_GetTicksNS();
  if (strokeActive || now - lastThumbNs < 150000000ull) return;
  const uint32_t T = 64;
  for (auto& l : layers) {
    if (!l.thumbDirty || l.folder) continue;
    uint64_t t0 = SDL_GetTicksNS();
    l.thumbDirty = false;
    lastThumbNs = now;
    uint32_t tw = docW >= docH ? T : std::max(1u, T * docW / docH);
    uint32_t th = docW >= docH ? std::max(1u, T * docH / docW) : T;
    bool fresh = false;
    if (!l.thumb.image) {
      if (createImage(device, memProps, tw, th, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, l.thumb) != VK_SUCCESS)
        return;
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dai.descriptorPool = descPool;
      dai.descriptorSetCount = 1;
      dai.pSetLayouts = &set0Layout;
      if (vkAllocateDescriptorSets(device, &dai, &l.thumbSet0) != VK_SUCCESS) { destroyImage(device, l.thumb); return; }
      writeFxBinding(l.thumbSet0);
      VkDescriptorImageInfo ii{VK_NULL_HANDLE, l.thumb.view, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w[3] = {};
      for (int k = 0; k < 3; ++k) {
        w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[k].dstSet = l.thumbSet0;
        w[k].dstBinding = uint32_t(2 + k);
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[k].pImageInfo = &ii;
      }
      vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
      l.thumbTex = ImGui_ImplVulkan_AddTexture(l.thumb.view, VK_IMAGE_LAYOUT_GENERAL);
      fresh = true;
    }
    if (fresh)
      imageBarrier(cmd, l.thumb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                   kCS, kRW);
    else  // the previous frame's UI may still sample it: order the rewrite after that
      memoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, kCS, kRW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &l.thumbSet0, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &l.set, 0, nullptr);
    View v;
    v.panX = docW * 0.5;
    v.panY = docH * 0.5;
    v.zoom = double(l.thumb.width) / docW;
    VkExtent2D ext{l.thumb.width, l.thumb.height};
    forceTransparentPaper = true;
    pushView(cmd, v, ext, 32 /*INIT*/ | 128 /*ONLY_BELOW*/, 1, BlendMode::Normal);
    vkCmdDispatch(cmd, (ext.width + 15) / 16, (ext.height + 15) / 16, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    pushView(cmd, v, ext, 0, 1, BlendMode::Normal, int(&l - layers.data()));
    vkCmdDispatch(cmd, (ext.width + 15) / 16, (ext.height + 15) / 16, 1);
    forceTransparentPaper = false;
    memoryBarrier(cmd, kCS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    lastCpu.thumbMs += (SDL_GetTicksNS() - t0) * 1e-6;
    return;  // one per frame
  }
}

bool Renderer::applyAdjust(int layer, int type, const float p[4], std::string& err) {
  if (layer < 0 || layer >= int(layers.size()) || strokeActive || layers[size_t(layer)].folder) return false;
  Layer& L = layers[layer];
  int rx0 = std::max(L.bx0, 0), ry0 = std::max(L.by0, 0), rx1 = std::min(L.bx1, int(docW)), ry1 = std::min(L.by1, int(docH));
  if (rx0 >= rx1 || ry0 >= ry1) return true;  // empty layer: nothing to change
  UndoEntry e;
  e.layerId = L.id;
  e.tiles = tilesForRect(rx0, ry0, rx1, ry1);
  VkCommandBuffer cmd = beginOneShot();
  bool haveUndo = copyTiles(cmd, L, e.tiles, e.chunks, e.bytes, true);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, kCS, kRW);
  struct { int32_t origin[2], size[2]; int32_t type, useSel; int32_t pad[2]; float p[4]; } pc{};
  pc.origin[0] = rx0; pc.origin[1] = ry0;
  pc.size[0] = rx1 - rx0; pc.size[1] = ry1 - ry0;
  pc.type = type;
  pc.useSel = selectionActive ? 1 : 0;
  for (int k = 0; k < 4; ++k) pc.p[k] = p[k];
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, adjPipe);
  VkDescriptorSet sets[2] = {slots[0].set0, L.set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 2, sets, 0, nullptr);
  vkCmdPushConstants(cmd, pipeLayout, kPcStages, 0, sizeof pc, &pc);
  vkCmdDispatch(cmd, uint32_t(pc.size[0] + 15) / 16, uint32_t(pc.size[1] + 15) / 16, 1);
  endOneShot(cmd);
  if (haveUndo) pushUndo(std::move(e));
  else err = "Not enough memory to keep undo for this change.";
  L.thumbDirty = true;
  cachesDirty = true;
  if (!haveUndo) bumpRevision();
  return true;
}

void Renderer::destroyNavigator() {
  GpuImage img = navImg;
  VkDescriptorSet set = navSet0, tex = navTexSet;
  VkDevice dev = device;
  VkDescriptorPool pool = descPool;
  if (!img.image && !set && !tex) return;
  defer([dev, img, set, tex, pool]() mutable {
    if (tex) ImGui_ImplVulkan_RemoveTexture(tex);
    destroyImage(dev, img);
    if (set) vkFreeDescriptorSets(dev, pool, 1, &set);
  });
  navImg = {};
  navSet0 = navTexSet = VK_NULL_HANDLE;
  navKey = 0;
}

// The whole picture, merged, at <= 512 px: blend every visible layer into one small image.
void Renderer::recordNavigator(VkCommandBuffer cmd) {
  if (!navWanted || !docW || strokeActive) return;
  uint64_t now = SDL_GetTicksNS();
  uint64_t key = revision * 1000003ull + docSerial * 7919ull + (whitePaper ? 1 : 2) +
                 uint64_t(paperColor[0] * 255) * 3 + uint64_t(paperColor[1] * 255) * 1031 + uint64_t(paperColor[2] * 255) * 65537;
  for (auto& l : layers)
    key = key * 1099511628211ull ^ (uint64_t(l.id) << 20 ^ uint64_t(l.visible) << 1 ^ uint64_t(l.opacity * 1000) << 8 ^
                                     uint64_t(l.mode) << 40 ^ uint64_t(l.thumbDirty));
  if (key == navKey && navImg.image) return;
  if (now - lastNavNs < 100000000ull && navImg.image) return;  // at most 10 per second
  const uint32_t T = 512;
  uint32_t tw = docW >= docH ? T : std::max(1u, uint32_t(uint64_t(T) * docW / docH));
  uint32_t th = docW >= docH ? std::max(1u, uint32_t(uint64_t(T) * docH / docW)) : T;
  if (navImg.image && (navImg.width != tw || navImg.height != th)) destroyNavigator();
  bool fresh = false;
  if (!navImg.image) {
    if (createImage(device, memProps, tw, th, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    navImg) != VK_SUCCESS)
      return;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set0Layout;
    if (vkAllocateDescriptorSets(device, &dai, &navSet0) != VK_SUCCESS) { destroyImage(device, navImg); navImg = {}; return; }
    writeFxBinding(navSet0);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, navImg.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {};
    for (int k = 0; k < 3; ++k) {
      w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[k].dstSet = navSet0;
      w[k].dstBinding = uint32_t(2 + k);
      w[k].descriptorCount = 1;
      w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[k].pImageInfo = &ii;
    }
    vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
    navTexSet = ImGui_ImplVulkan_AddTexture(navImg.view, VK_IMAGE_LAYOUT_GENERAL);
    fresh = true;
  }
  uint64_t t0 = now;
  navKey = key;
  lastNavNs = now;
  if (fresh)
    imageBarrier(cmd, navImg.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, kCS,
                 kRW);
  else
    memoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, kCS, kRW);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &navSet0, 0, nullptr);
  View v;
  v.panX = docW * 0.5;
  v.panY = docH * 0.5;
  v.zoom = double(tw) / docW;
  VkExtent2D ext{tw, th};
  uint32_t gx = (tw + 15) / 16, gy = (th + 15) / 16;
  VkDescriptorSet anySet = anyLayerSet();
  if (anySet) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &anySet, 0, nullptr);
    pushView(cmd, v, ext, 32 /*INIT*/ | 128 /*ONLY_BELOW*/, 1, BlendMode::Normal);  // paper (white or transparent)
    vkCmdDispatch(cmd, gx, gy, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    std::vector<CompItem> items = buildItems();
    compositeItems(cmd, items, 0, items.size(), CompTarget{navSet0, navImg.view, 0}, v, ext, 0, false, false);
  }
  memoryBarrier(cmd, kCS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  lastCpu.thumbMs += (SDL_GetTicksNS() - t0) * 1e-6;
}

std::shared_ptr<AsyncRead> Renderer::readRegionAsync(bool merged, int layer, int x, int y, uint32_t w, uint32_t h, std::string& err) {
  auto r = std::make_shared<AsyncRead>();
  r->device = device;
  r->x = x; r->y = y; r->w = w; r->h = h;
  if (!w || !h || (!merged && (layer < 0 || layer >= int(layers.size()) || layers[size_t(layer)].folder))) return nullptr;
  const uint32_t T = 2048;
  // bands of rows, <= ~256 MB each; merged bands are whole flatten-tile rows
  r->bandRows = merged ? T : uint32_t(std::clamp<VkDeviceSize>((256ull << 20) / (VkDeviceSize(w) * 4), 1, h));
  auto fail = [&](const std::string& m) { err = m; finishAsyncRead(r); return nullptr; };
  for (uint32_t y0 = 0; y0 < h; y0 += r->bandRows) {
    GpuBuffer b;
    uint32_t n = std::min(r->bandRows, h - y0);
    if (createBuffer(device, memProps, VkDeviceSize(w) * n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b, true) != VK_SUCCESS)
      return fail("Not enough memory for the read-back.");
    r->bands.push_back(b);
  }
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pci.queueFamilyIndex = queueFamily;
  VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &r->pool));
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = r->pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  const VkAccessFlags mem = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  memoryBarrier(cmd, all, mem, all, mem);
  if (!merged) {
    for (size_t i = 0; i < r->bands.size(); ++i) {
      uint32_t y0 = uint32_t(i) * r->bandRows, n = std::min(r->bandRows, h - y0);
      VkBufferImageCopy c{};
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.imageOffset = {x, y + int32_t(y0), 0};
      c.imageExtent = {w, n, 1};
      vkCmdCopyImageToBuffer(cmd, layers[layer].image.image, VK_IMAGE_LAYOUT_GENERAL, r->bands[i].buffer, 1, &c);
    }
  } else {
    // flatten 2048 x 2048 tiles with the cache shader (same maths as the screen), copy each out
    if (createImage(device, memProps, T, T, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    r->tile) != VK_SUCCESS) {
      vkEndCommandBuffer(cmd);
      return fail("Not enough GPU memory for the read-back.");
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set0Layout;
    VK_CHECK(vkAllocateDescriptorSets(device, &dai, &r->tileSet));
    writeFxBinding(r->tileSet);
    updateFx();
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, r->tile.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {};
    for (int k = 0; k < 3; ++k) {
      wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[k].dstSet = r->tileSet;
      wr[k].dstBinding = uint32_t(2 + k);
      wr[k].descriptorCount = 1;
      wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      wr[k].pImageInfo = &ii;
    }
    vkUpdateDescriptorSets(device, 3, wr, 0, nullptr);
    imageBarrier(cmd, r->tile.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, all, 0, all, mem);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &r->tileSet, 0, nullptr);
    VkExtent2D ext{T, T};
    for (uint32_t ty = 0; ty < h; ty += T)
      for (uint32_t tx = 0; tx < w; tx += T) {
        View v;
        v.panX = x + tx + T / 2.0;
        v.panY = y + ty + T / 2.0;
        v.zoom = 1;
        memoryBarrier(cmd, all, mem, kCS, kRW);  // previous tile's copy must finish before reuse
        VkDescriptorSet anySet = anyLayerSet();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &r->tileSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &anySet, 0, nullptr);
        pushView(cmd, v, ext, 32 /*INIT*/ | 128 /*ONLY_BELOW*/, 1, BlendMode::Normal);
        vkCmdDispatch(cmd, T / 16, T / 16, 1);
        memoryBarrier(cmd, kCS, kRW, kCS, kRW);
        std::vector<CompItem> items = buildItems();
        compositeItems(cmd, items, 0, items.size(), CompTarget{r->tileSet, r->tile.view, 0}, v, ext, 0, true, false);
        memoryBarrier(cmd, kCS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(tx) * 4;
        c.bufferRowLength = w;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {std::min(T, w - tx), std::min(T, h - ty), 1};
        vkCmdCopyImageToBuffer(cmd, r->tile.image, VK_IMAGE_LAYOUT_GENERAL, r->bands[ty / T].buffer, 1, &c);
      }
  }
  memoryBarrier(cmd, all, mem, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  VK_CHECK(vkEndCommandBuffer(cmd));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device, &fci, nullptr, &r->fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, r->fence));  // no wait: frames keep flowing
  return r;
}

void Renderer::finishAsyncRead(std::shared_ptr<AsyncRead>& r) {
  if (!r) return;
  if (r->fence) {
    vkWaitForFences(device, 1, &r->fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, r->fence, nullptr);
  }
  if (r->pool) vkDestroyCommandPool(device, r->pool, nullptr);
  for (auto& b : r->bands) destroyBuffer(device, b);
  if (r->tileSet) vkFreeDescriptorSets(device, descPool, 1, &r->tileSet);
  destroyImage(device, r->tile);
  r.reset();
}

// ---------------------------------------------------------------------------
// Live previews

void Renderer::clearMaskRect(VkCommandBuffer cmd, int x0, int y0, int x1, int y1) {
  x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, int(docW)); y1 = std::min(y1, int(docH));
  if (x0 >= x1 || y0 >= y1) return;
  struct { int32_t origin[2], size[2]; float p0[2], p1[2]; int32_t clear; } pc{{x0, y0}, {x1 - x0, y1 - y0}, {0, 0}, {1, 0}, 1};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradPipe);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &slots[0].set0, 0, nullptr);
  vkCmdPushConstants(cmd, pipeLayout, kPcStages, 0, sizeof pc, &pc);
  vkCmdDispatch(cmd, uint32_t(pc.size[0] + 15) / 16, uint32_t(pc.size[1] + 15) / 16, 1);
  memoryBarrier(cmd, kCS, kRW, kCS, kRW);
}

void Renderer::previewClear() {
  if (!strokeActive) return;
  pendingDabs.clear();
  pendingOffset = 0;
  maskClearPending = true;
}

void Renderer::recordPreviewOps(VkCommandBuffer cmd) {
  if (!strokeActive) { maskClearPending = gradPending = false; return; }
  if (maskClearPending) {
    clearMaskRect(cmd, sx0, sy0, sx1, sy1);
    maskClearPending = false;
  }
  if (gradPending) {
    struct { int32_t origin[2], size[2]; float p0[2], p1[2]; int32_t clear; } pc{
        {gradR[0], gradR[1]}, {gradR[2] - gradR[0], gradR[3] - gradR[1]}, {gradP[0], gradP[1]}, {gradP[2], gradP[3]}, 0};
    if (pc.size[0] > 0 && pc.size[1] > 0) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradPipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &slots[frameCounter % kFrames].set0, 0, nullptr);
      vkCmdPushConstants(cmd, pipeLayout, kPcStages, 0, sizeof pc, &pc);
      vkCmdDispatch(cmd, uint32_t(pc.size[0] + 15) / 16, uint32_t(pc.size[1] + 15) / 16, 1);
      memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    }
    gradPending = false;
  }
}

void Renderer::previewGradient(float x0, float y0, float x1, float y1, int rx0, int ry0, int rx1, int ry1) {
  if (!strokeActive) return;
  rx0 = std::max(rx0, 0); ry0 = std::max(ry0, 0); rx1 = std::min(rx1, int(docW)); ry1 = std::min(ry1, int(docH));
  gradP[0] = x0; gradP[1] = y0; gradP[2] = x1; gradP[3] = y1;
  gradR[0] = rx0; gradR[1] = ry0; gradR[2] = rx1; gradR[3] = ry1;
  gradPending = true;
  markDirtyRect(rx0, ry0, rx1, ry1);  // the gradient overwrites the whole rect every time
}

bool Renderer::previewCoverage(int x, int y, uint32_t w, uint32_t h, const uint8_t* cov, std::string& err) {
  if (!strokeActive) return false;
  pendingDabs.clear();
  pendingOffset = 0;
  VkCommandBuffer cmd = beginOneShot();  // ordered after the frames already submitted
  clearMaskRect(cmd, sx0, sy0, sx1, sy1);
  endOneShot(cmd);
  maskClearPending = false;
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
  markDirtyRect(x0, y0, x1, y1);
  return ok;
}

void Renderer::abortStroke() {
  if (!strokeActive) return;
  pendingDabs.clear();
  pendingOffset = 0;
  VkCommandBuffer cmd = beginOneShot();
  clearMaskRect(cmd, sx0, sy0, sx1, sy1);
  endOneShot(cmd);
  for (uint32_t t : dirtyTiles) tileDirty[t] = 0;
  dirtyTiles.clear();
  undoCopied = 0;
  releaseStrokeUndo();
  strokeActive = strokeEnding = false;
  maskClearPending = gradPending = false;
}

// ---------------------------------------------------------------------------
// Per-layer effects

void Renderer::writeFxBinding(VkDescriptorSet set) {
  if (!fxBuf.buffer || !set) return;
  VkDescriptorBufferInfo bi{fxBuf.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set;
  w.dstBinding = 6;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &bi;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

float Renderer::compositeOpacity(const Layer& l) const {
  return l.tone.on && l.tone.reflectOpacity ? 1.0f : l.opacity;
}

void Renderer::updateFx() {
  if (!fxBuf.mapped) return;
  struct FxGpu {
    int32_t flags, shape, density, levels;
    float cell, cosA, sinA, offX, offY, noiseSize, noiseFactor, area, opacity, pad[3];
    float main[4], sub[4];
  };
  static_assert(sizeof(FxGpu) == 96);
  auto* out = static_cast<FxGpu*>(fxBuf.mapped);
  for (size_t i = 0; i < layers.size() && i < fxCapacity; ++i) {
    const Layer& l = layers[i];
    FxGpu g{};
    if (l.lcolor.on) {
      g.flags |= 2;
      for (int k = 0; k < 3; ++k) { g.main[k] = l.lcolor.main[k]; g.sub[k] = l.lcolor.sub[k]; }
      g.main[3] = 1;
      g.sub[3] = l.lcolor.useSub ? 1.0f : 0.0f;
    }
    if (l.tone.on) {
      const ToneFx& t = l.tone;
      g.flags |= 1 | (t.reflectOpacity ? 4 : 0) | (t.express == 1 ? 8 : 0);
      g.shape = std::clamp(t.shape, 0, kToneShapes - 1);
      g.density = t.density;
      g.levels = t.posterize ? std::max(2, t.levels) : 0;
      g.cell = std::max(1.0f, docDpi / std::max(1.0f, t.frequency));
      double a = t.angle * 3.14159265358979323846 / 180.0;
      g.cosA = float(std::cos(a));
      g.sinA = float(std::sin(a));
      g.offX = t.offX;
      g.offY = t.offY;
      g.noiseSize = std::max(1.0f, t.noiseSize);
      g.noiseFactor = std::clamp(t.noiseFactor, 0.0f, 1.0f);
      g.area = toneShapeArea(g.shape);
      g.opacity = l.opacity;
    }
    out[i] = g;
  }
}
