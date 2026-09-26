// Layer folders: composite tree, group images and the recursive compositor used by the screen
// caches, the per-frame composite, the navigator, flattening and merged read-backs.
#include "renderer.h"
#include <algorithm>
#include <functional>
#include <imgui_impl_vulkan.h>

namespace {
constexpr VkPipelineStageFlags kCS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags kRW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
enum : int {
  FLAG_STROKE = 1, FLAG_ERASER = 2, FLAG_INIT = 32, FLAG_WORK = 64, FLAG_ONLY_BELOW = 128, FLAG_SEL = 256, FLAG_LOCK = 512,
  FLAG_NOCLIP = 1024, FLAG_ADJ = 2048, FLAG_GROUP_BLEND = 8192, FLAG_CLIP = 32768
};
}  // namespace

std::vector<Renderer::CompItem> Renderer::buildItems() const {
  // children of each container, in stack order (bottom to top)
  std::vector<std::vector<int>> kids(1);
  std::vector<uint32_t> ids(1, 0);
  auto slot = [&](uint32_t id) {
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return int(i);
    ids.push_back(id);
    kids.emplace_back();
    return int(ids.size() - 1);
  };
  for (int i = 0; i < int(layers.size()); ++i) {
    uint32_t f = layers[i].folderId;
    if (f && findLayer(f) < 0) f = 0;  // orphan: top level
    kids[size_t(slot(f))].push_back(i);
  }
  std::function<std::vector<CompItem>(uint32_t, float, bool, int)> build = [&](uint32_t folder, float opMul, bool hidden, int depth) {
    std::vector<CompItem> out;
    int s = slot(folder);
    std::vector<int> list = kids[size_t(s)];
    for (int idx : list) {
      const Layer& l = layers[size_t(idx)];
      bool h = hidden || !l.visible || l.opacity <= 0;
      if (l.folder) {
        if (depth > 32) continue;  // defensive: a folder loop
        if (l.passThrough) {
          std::vector<CompItem> inner = build(l.id, opMul * l.opacity, h, depth + 1);
          if (inner.empty() || h) {  // keep a marker so the structure stays findable
            CompItem g;
            g.layer = idx;
            g.group = true;
            g.hidden = true;
            g.kids = std::move(inner);
            out.push_back(std::move(g));
          } else {
            for (auto& k : inner) out.push_back(std::move(k));
          }
        } else {
          CompItem g;
          g.layer = idx;
          g.group = true;
          g.hidden = h;
          g.opMul = opMul;
          g.kids = build(l.id, 1.0f, h, depth + 1);
          out.push_back(std::move(g));
        }
      } else {
        CompItem it;
        it.layer = idx;
        it.hidden = h;
        it.opMul = opMul;
        out.push_back(std::move(it));
      }
      // clipping: this item joins the clip group of the base below it (same container)
      if (l.clip && out.size() >= 2) {
        CompItem mine = std::move(out.back());
        out.pop_back();
        CompItem& prev = out.back();
        if (!prev.clipGroup) {  // turn the base into a clip group
          CompItem base = std::move(prev);
          CompItem g;
          g.layer = base.layer;
          g.group = true;
          g.clipGroup = true;
          g.hidden = base.hidden;
          g.opMul = base.opMul;
          base.asBase = true;
          base.opMul = 1;
          g.kids.push_back(std::move(base));
          prev = std::move(g);
        }
        mine.clipped = true;
        mine.hidden = mine.hidden || prev.hidden;  // a hidden base hides what is clipped to it
        prev.kids.push_back(std::move(mine));
      }
    }
    return out;
  };
  return build(0, 1.0f, false, 0);
}

bool Renderer::itemContains(const CompItem& it, int layer) {
  if (it.layer == layer) return true;
  for (const CompItem& k : it.kids)
    if (itemContains(k, layer)) return true;
  return false;
}

VkDescriptorSet Renderer::anyLayerSet() const {
  for (const Layer& l : layers)
    if (l.set) return l.set;
  return VK_NULL_HANDLE;
}

int Renderer::liveFlags(const Layer& l) const {
  int f = 0;
  if (strokeActive && l.id == strokeLayerId) f |= FLAG_STROKE | (style.eraser ? FLAG_ERASER : 0) | (style.overlay ? FLAG_NOCLIP : 0);
  if (l.lockAlpha) f |= FLAG_LOCK;
  if (selectionActive) f |= FLAG_SEL;
  return f;
}

bool Renderer::ensureGroupImage(GroupStack& g, int depth, uint32_t w, uint32_t h, VkCommandBuffer cmd) {
  if (depth >= kMaxGroupDepth) return false;
  if (g.img[depth].image && (g.w < w || g.h < h)) destroyGroups();  // grew: rebuild the whole stack
  if (g.w < w || g.h < h) { g.w = std::max(g.w, w); g.h = std::max(g.h, h); }
  if (!g.img[depth].image) {
    if (createImage(device, memProps, g.w, g.h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT, g.img[depth]) != VK_SUCCESS)
      return false;
    imageBarrier(cmd, g.img[depth].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 kCS, kRW);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set0Layout;
    if (vkAllocateDescriptorSets(device, &dai, &g.set[depth]) != VK_SUCCESS) return false;
    writeGroupSet(g, depth);  // only the new set: the others may be in use by frames in flight
  }
  return true;
}

// group target sets: binding 2 = the group image, plus the stroke mask, selection and effects
void Renderer::refreshGroupSets() {  // after a new document (the GPU is idle then)
  for (GroupStack* g : {&gScreen, &gOff})
    for (int d = 0; d < kMaxGroupDepth; ++d) writeGroupSet(*g, d);
}

void Renderer::writeGroupSet(GroupStack& gs, int d) {
  GroupStack* g = &gs;
  {
      if (!g->set[d] || !g->img[d].image) return;
      VkDescriptorImageInfo gi{VK_NULL_HANDLE, g->img[d].view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo mi{VK_NULL_HANDLE, mask.view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo si{VK_NULL_HANDLE, sel.view, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w[5] = {};
      int n = 0;
      auto img = [&](uint32_t binding, const VkDescriptorImageInfo* ii) {
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = g->set[d];
        w[n].dstBinding = binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[n].pImageInfo = ii;
        ++n;
      };
      img(2, &gi);
      img(3, &gi);  // unused here, but every binding the shader has must be valid
      img(4, &gi);
      if (mask.view) img(0, &mi);
      if (sel.view) img(5, &si);
      vkUpdateDescriptorSets(device, uint32_t(n), w, 0, nullptr);
      writeFxBinding(g->set[d]);
    }
}

VkDescriptorSet Renderer::blendSet(VkImageView target, VkImageView src) {
  for (auto& e : blendSets)
    if (e.first.first == target && e.first.second == src) return e.second;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = descPool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &set0Layout;
  if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo ti{VK_NULL_HANDLE, target, VK_IMAGE_LAYOUT_GENERAL}, si{VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[3] = {};
  for (int k = 0; k < 3; ++k) {
    w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[k].dstSet = set;
    w[k].dstBinding = uint32_t(2 + k);
    w[k].descriptorCount = 1;
    w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[k].pImageInfo = k == 1 ? &si : &ti;  // 2 = target, 3 = folder image, 4 = (unused) target
  }
  vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
  writeFxBinding(set);
  blendSets.push_back({{target, src}, set});
  return set;
}

void Renderer::destroyGroups() {
  std::vector<VkDescriptorSet> sets;
  std::vector<GpuImage> imgs;
  for (auto& e : blendSets) sets.push_back(e.second);
  blendSets.clear();
  for (GroupStack* g : {&gScreen, &gOff}) {
    for (int d = 0; d < kMaxGroupDepth; ++d) {
      if (g->set[d]) sets.push_back(g->set[d]);
      if (g->img[d].image) imgs.push_back(g->img[d]);
      g->set[d] = VK_NULL_HANDLE;
      g->img[d] = {};
    }
    g->w = g->h = 0;
  }
  VkDevice dev = device;
  VkDescriptorPool pool = descPool;
  defer([dev, pool, sets, imgs]() mutable {
    for (VkDescriptorSet s : sets) vkFreeDescriptorSets(dev, pool, 1, &s);
    for (auto& i : imgs) destroyImage(dev, i);
  });
}

// Blends items [from, to) into the target. Folders that are not "Through" are composited on their
// own into a group image first (one per nesting depth), then blended with the folder's mode.
void Renderer::compositeItems(VkCommandBuffer cmd, const std::vector<CompItem>& items, size_t from, size_t to,
                              const CompTarget& t, const View& v, VkExtent2D ext, int depth, bool offscreen, bool live) {
  uint32_t gx = (ext.width + 15) / 16, gy = (ext.height + 15) / 16;
  VkDescriptorSet anySet = anyLayerSet();
  if (!anySet) return;
  for (size_t i = from; i < to && i < items.size(); ++i) {
    const CompItem& it = items[i];
    if (it.hidden) continue;
    const Layer& l = layers[size_t(it.layer)];
    if (!it.group) {
      bool useLive = live && liveFlags(l) != 0;
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useLive ? cacheStrokePipe : cachePipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &t.set, 0, nullptr);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &l.set, 0, nullptr);
      float op = it.asBase ? (l.tone.on && l.tone.reflectOpacity ? l.opacity : 1.0f) : compositeOpacity(l) * it.opMul;
      pushView(cmd, v, ext, t.flags | (useLive ? liveFlags(l) : 0) | (it.clipped ? FLAG_CLIP : 0), op,
               it.asBase ? BlendMode::Normal : l.mode, it.layer);
      vkCmdDispatch(cmd, gx, gy, 1);
      memoryBarrier(cmd, kCS, kRW, kCS, kRW);
      continue;
    }
    GroupStack& g = offscreen ? gOff : gScreen;
    if (!ensureGroupImage(g, depth, std::max(ext.width, 2048u), std::max(ext.height, 2048u), cmd)) {
      compositeItems(cmd, it.kids, 0, it.kids.size(), t, v, ext, depth, offscreen, live);  // too deep: as "Through"
      continue;
    }
    // clear the group image, composite the children into it, blend it into the target
    CompTarget gt{g.set[depth], g.img[depth].view, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &gt.set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &anySet, 0, nullptr);
    bool paper = forceTransparentPaper;
    forceTransparentPaper = true;
    pushView(cmd, v, ext, FLAG_INIT | FLAG_ONLY_BELOW, 1, BlendMode::Normal);
    forceTransparentPaper = paper;
    vkCmdDispatch(cmd, gx, gy, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
    compositeItems(cmd, it.kids, 0, it.kids.size(), gt, v, ext, depth + 1, offscreen, live);
    VkDescriptorSet bs = blendSet(t.view, g.img[depth].view);
    if (!bs) continue;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cachePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &bs, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 1, 1, &anySet, 0, nullptr);
    // a clip group takes the mode and opacity of its base; a folder its own; as a base: Normal, full
    float gop = it.asBase ? 1.0f : (it.clipGroup ? compositeOpacity(l) : l.opacity) * it.opMul;
    if (it.clipGroup && l.folder) gop = it.asBase ? 1.0f : l.opacity * it.opMul;
    BlendMode gm = it.asBase ? BlendMode::Normal : l.mode;
    if (!it.asBase && l.folder && l.passThrough && !it.clipGroup) gm = BlendMode::Normal;
    pushView(cmd, v, ext, FLAG_GROUP_BLEND | (it.clipped ? FLAG_CLIP : 0), gop, gm);
    vkCmdDispatch(cmd, gx, gy, 1);
    memoryBarrier(cmd, kCS, kRW, kCS, kRW);
  }
}

void Renderer::eraseLayerNoUndo(int index) {
  if (index < 0 || index >= int(layers.size())) return;
  destroyLayer(layers[size_t(index)]);
  layers.erase(layers.begin() + index);
  cachesDirty = true;
}

int Renderer::addFolder(int index, std::string& err, bool undoable) {
  (void)err;
  Layer l;
  l.id = nextLayerId++;
  l.folder = true;
  l.name = "Folder " + std::to_string(l.id);
  index = std::clamp(index, 0, int(layers.size()));
  // same container as the layer right above the new position (inside a folder when that is its entry)
  if (index < int(layers.size())) l.folderId = layers[size_t(index)].folder ? layers[size_t(index)].id : layers[size_t(index)].folderId;
  uint32_t id = l.id;
  layers.insert(layers.begin() + index, std::move(l));
  cachesDirty = true;
  if (undoable) {
    UndoEntry e;
    e.kind = UndoKind::Added;
    e.layerId = id;
    pushUndo(std::move(e));
  } else {
    bumpRevision();
  }
  return index;
}
