// Edit menu operations: clipboard, clear outside, tonal corrections with live preview, layer
// flip / rotate, selection grow / shrink and the selection action bar under the selection.
#include "app.h"
#include "fileio.h"
#include <algorithm>
#include <cmath>
#include <cstring>

void App::showToast(const std::string& msg) {
  toast = msg;
  toastUntil = SDL_GetTicksNS() + 2500000000ull;
}

// ---------------------------------------------------------------------------
// Clipboard. Copy reads the area back asynchronously (the UI never waits), keeps the pixels for
// an internal paste at the same position, and offers them to other apps as PNG.

void App::copySelection(bool cut, bool pasteAfter) {
  if (!R.hasDocument() || R.busy() || clipping || xf.active) return;
  if (hoverPreviewOn || previewing) cancelPreviews();
  const Layer& L = R.layers[active];
  int x0 = L.bx0, y0 = L.by0, x1 = L.bx1, y1 = L.by1;
  if (selActive) { x0 = std::max(x0, selX0); y0 = std::max(y0, selY0); x1 = std::min(x1, selX1); y1 = std::min(y1, selY1); }
  if (x0 >= x1 || y0 >= y1) { showToast("Nothing to copy - the area is empty"); return; }
  std::string err;
  clipRead = R.readRegionAsync(false, active, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), err);
  if (!clipRead) { error(err.empty() ? "Read-back failed" : err); return; }
  // the selection coverage of the area, for soft / partial selections
  std::vector<uint8_t> sel;
  if (selActive) {
    int W = int(R.docW);
    sel.resize(size_t(x1 - x0) * (y1 - y0));
    for (int y = y0; y < y1; ++y) memcpy(&sel[size_t(y - y0) * (x1 - x0)], &selCpu[size_t(y) * W + x0], size_t(x1 - x0));
  }
  clipping = true;
  clipPasteAfter = pasteAfter;
  std::shared_ptr<AsyncRead> rd = clipRead;
  std::string name = L.name;
  clipJob = std::async(std::launch::async, [rd, sel = std::move(sel), x0, y0, x1, y1, name]() {
    ClipImage c;
    c.x = x0; c.y = y0; c.w = uint32_t(x1 - x0); c.h = uint32_t(y1 - y0);
    c.name = name;
    if (!rd->wait()) return c;
    c.rgba.resize(size_t(c.w) * c.h * 4);
    for (uint32_t y = 0; y < c.h; ++y)
      for (uint32_t x = 0; x < c.w; ++x) {
        const uint8_t* s = rd->pixel(x0 + int(x), y0 + int(y));
        uint8_t* d = &c.rgba[(size_t(y) * c.w + x) * 4];
        uint32_t k = sel.empty() ? 255 : sel[size_t(y) * c.w + x];
        uint8_t a = uint8_t((s[3] * k + 127) / 255);
        if (a == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
        for (int i = 0; i < 3; ++i) d[i] = uint8_t(std::min(255u, (uint32_t(s[i]) * 255 + s[3] / 2) / std::max<uint32_t>(1, s[3])));
        d[3] = a;
      }
    ImageRGBA img{c.w, c.h, c.rgba};
    encodePngMemory(img, c.png);
    c.ok = true;
    return c;
  });
  if (cut) fillSelection(true);  // the read-back was submitted first, so it still sees the pixels
}

static const char* kPngMime[] = {"image/png"};

void App::pollClipboard() {
  if (!clipping || clipJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  clipping = false;
  ClipImage c = clipJob.get();
  R.finishAsyncRead(clipRead);
  if (!c.ok) { showToast("Copy failed"); return; }
  // the system clipboard owns its own copy of the PNG
  auto* png = new std::vector<uint8_t>(std::move(c.png));
  SDL_SetClipboardData(
      [](void* ud, const char* mime, size_t* size) -> const void* {
        auto* v = static_cast<std::vector<uint8_t>*>(ud);
        if (!mime || std::strcmp(mime, "image/png") != 0) { *size = 0; return nullptr; }
        *size = v->size();
        return v->data();
      },
      [](void* ud) { delete static_cast<std::vector<uint8_t>*>(ud); }, png, kPngMime, 1);
  clipExternal = false;
  clip = std::move(c);
  if (clipPasteAfter) pasteClipboard(true);
  else showToast("Copied " + std::to_string(clip.w) + " x " + std::to_string(clip.h) + " px");
}

void App::pasteClipboard(bool internalOnly) {
  if (!R.hasDocument() || R.busy() || xf.active) return;
  if (hoverPreviewOn || previewing) cancelPreviews();
  ClipImage c;
  const ClipImage* src = &clip;
  if (!internalOnly && (clipExternal || !clip.ok)) {  // another app copied an image after us
    const char* mimes[] = {"image/png", "image/bmp"};
    for (const char* m : mimes) {
      if (!SDL_HasClipboardData(m)) continue;
      size_t n = 0;
      void* data = SDL_GetClipboardData(m, &n);
      if (!data) continue;
      ImageRGBA img;
      std::string err;
      bool ok = decodeImageMemory(data, n, img, err);
      SDL_free(data);
      if (!ok) continue;
      c.w = img.w; c.h = img.h;
      c.rgba = std::move(img.rgba);
      double cx, cy;  // centred on the canvas view
      screenToDoc(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, cx, cy);
      c.x = int(std::lround(cx - c.w * 0.5));
      c.y = int(std::lround(cy - c.h * 0.5));
      c.name = "Pasted image";
      c.ok = true;
      src = &c;
      break;
    }
  }
  if (!src->ok || !src->w) { showToast("The clipboard has no image"); return; }
  std::string err;
  int idx = R.addLayer(active + 1, err);
  if (idx < 0) { error(err); return; }
  active = idx;
  std::vector<uint8_t> px = src->rgba;
  premultiply(px);
  if (!R.uploadLayerPixels(idx, src->x, src->y, src->w, src->h, px.data(), err)) error(err);
  R.layers[size_t(idx)].name = src->name.empty() ? "Pasted" : src->name + " copy";
  R.markCachesDirty();
  deselect();
  showToast("Pasted on a new layer");
}

// ---------------------------------------------------------------------------
// Clear outside the selection: one undo step, erases where the selection is not.

void App::clearOutsideSelection() {
  if (!selActive || !R.hasDocument() || R.busy() || xf.active) return;
  if (hoverPreviewOn || previewing) cancelPreviews();
  const Layer& L = R.layers[active];
  if (L.bx0 >= L.bx1) return;
  int W = int(R.docW);
  int x0 = L.bx0, y0 = L.by0, x1 = L.bx1, y1 = L.by1;
  std::vector<uint8_t> cov(size_t(x1 - x0) * (y1 - y0));
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) cov[size_t(y - y0) * (x1 - x0) + (x - x0)] = uint8_t(255 - selCpu[size_t(y) * W + x]);
  StrokeStyle st = currentStyle(true);
  st.opacity = 1;
  st.ignoreSelection = true;
  std::string err;
  if (!R.paintCoverage(active, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), cov.data(), st, err) && !err.empty()) error(err);
}

// ---------------------------------------------------------------------------
// Tonal corrections: the frame composite previews the adjustment on the screen pixels of the
// active layer (no copies, instant at any document size); OK applies the same shader code to
// the layer (one undo step).

enum { ADJ_BC = 1, ADJ_HSL = 2, ADJ_INVERT = 3, ADJ_POSTER = 4, ADJ_THRESH = 5, ADJ_B2A = 6, ADJ_TOCOLOR = 7 };

void App::openAdjust(int type) {
  if (!R.hasDocument() || R.busy() || R.stroking()) return;
  if (hoverPreviewOn || previewing) cancelPreviews();
  if (xf.active) applyTransform();
  const Layer& L = R.layers[active];
  if (L.bx0 >= L.bx1) { showToast("The layer is empty"); return; }
  float p[4] = {0, 0, 0, 0};
  if (type == ADJ_INVERT || type == ADJ_B2A || type == ADJ_TOCOLOR) {  // no settings: apply at once
    if (type != ADJ_INVERT) { p[0] = color[0]; p[1] = color[1]; p[2] = color[2]; }
    std::string err;
    if (!R.applyAdjust(active, type, p, err) && !err.empty()) error(err);
    else if (!err.empty()) error(err);
    return;
  }
  adj = AdjustDlg{};
  adj.type = type;
  if (type == ADJ_POSTER) adj.p[0] = 6;
  if (type == ADJ_THRESH) adj.p[0] = 0.5f;
  adj.open = true;
}

void App::drawAdjustDialog() {
  if (!adj.open) { R.adjType = 0; return; }
  const char* titles[] = {"", "Brightness / Contrast", "Hue / Saturation / Luminosity", "", "Posterize", "Threshold"};
  float sc = ImGui::GetStyle().FontScaleDpi;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.25f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(380 * sc, 0), ImGuiCond_Appearing);
  bool open = true;
  char title[96];
  snprintf(title, sizeof title, "%s###adjust", titles[adj.type]);
  ImGui::SetNextWindowFocus();
  if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    auto slider = [&](const char* id, float* v, float mn, float mx, const char* fmt) {
      ImGui::SetNextItemWidth(300 * sc);
      return ImGui::SliderFloat(id, v, mn, mx, fmt);
    };
    float disp[3];
    switch (adj.type) {
      case ADJ_BC:
        disp[0] = adj.p[0] * 100; disp[1] = adj.p[1] * 100;
        if (slider("Brightness", &disp[0], -100, 100, "%.0f")) adj.p[0] = disp[0] / 100;
        if (slider("Contrast", &disp[1], -100, 100, "%.0f")) adj.p[1] = disp[1] / 100;
        break;
      case ADJ_HSL:
        disp[0] = adj.p[0] * 360; disp[1] = adj.p[1] * 100; disp[2] = adj.p[2] * 100;
        if (slider("Hue", &disp[0], -180, 180, "%.0f deg")) adj.p[0] = disp[0] / 360;
        if (slider("Saturation", &disp[1], -100, 100, "%.0f")) adj.p[1] = disp[1] / 100;
        if (slider("Luminosity", &disp[2], -100, 100, "%.0f")) adj.p[2] = disp[2] / 100;
        break;
      case ADJ_POSTER: {
        int n = int(adj.p[0]);
        ImGui::SetNextItemWidth(300 * sc);
        if (ImGui::SliderInt("Levels", &n, 2, 32)) adj.p[0] = float(n);
        break;
      }
      case ADJ_THRESH:
        disp[0] = adj.p[0] * 255;
        if (slider("Threshold", &disp[0], 0, 255, "%.0f")) adj.p[0] = disp[0] / 255;
        break;
      default: break;
    }
    ImGui::Checkbox("Preview", &adj.preview);
    ImGui::TextDisabled(selActive ? "Changes the active layer inside the selection." : "Changes the whole active layer.");
    ImGui::Spacing();
    float bw = 110 * sc;
    bool ok = ImGui::Button("OK", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(bw, 0))) {
      int t = adj.type;
      adj.p[0] = t == ADJ_POSTER ? 6 : t == ADJ_THRESH ? 0.5f : 0;
      adj.p[1] = adj.p[2] = adj.p[3] = 0;
    }
    if (ok) {
      std::string err;
      R.adjType = 0;
      if (!R.applyAdjust(active, adj.type, adj.p, err) || !err.empty()) { if (!err.empty()) error(err); }
      adj.open = false;
    }
    if (cancel) adj.open = false;
  }
  ImGui::End();
  if (!open) adj.open = false;
  // live preview in the frame composite
  R.adjType = adj.open && adj.preview ? adj.type : 0;
  for (int k = 0; k < 4; ++k) R.adjP[k] = adj.p[k];
  if (adj.open) framesToRender = std::max(framesToRender, 2);
}

// ---------------------------------------------------------------------------
// Layer flip / rotate: through the transform (lifts the pixels, moves the corners, applies).

void App::transformLayer(int kind) {
  if (!R.hasDocument()) return;
  bool wasActive = xf.active;
  if (!wasActive) {
    if (hoverPreviewOn || previewing) cancelPreviews();
    startTransform();
    if (!xf.active) return;
  }
  double (*q)[2] = xf.q;
  double cx = (q[0][0] + q[1][0] + q[2][0] + q[3][0]) / 4, cy = (q[0][1] + q[1][1] + q[2][1] + q[3][1]) / 4;
  auto swapc = [&](int a, int b) { for (int k = 0; k < 2; ++k) std::swap(q[a][k], q[b][k]); };
  if (kind == 0) { swapc(0, 1); swapc(3, 2); }       // flip horizontally
  else if (kind == 1) { swapc(0, 3); swapc(1, 2); }  // flip vertically
  else {
    double ang = kind == 2 ? 0.5 : kind == 3 ? -0.5 : 1.0;  // turns of pi
    double c = std::cos(ang * 3.14159265358979323846), s = std::sin(ang * 3.14159265358979323846);
    for (int i = 0; i < 4; ++i) {
      double x = q[i][0] - cx, y = q[i][1] - cy;
      q[i][0] = cx + std::round((c * x - s * y) * 2) / 2;  // stay on half pixels: no resampling blur
      q[i][1] = cy + std::round((s * x + c * y) * 2) / 2;
    }
  }
  if (!wasActive) applyTransform();  // from the menu: done in one go (one undo step)
}

// ---------------------------------------------------------------------------
// Selection grow / shrink by a distance in pixels (Euclidean, anti-aliased edge).

static void edt1d(const float* f, int n, float* d, int* v, float* z) {
  int k = 0;
  v[0] = 0;
  z[0] = -1e30f;
  z[1] = 1e30f;
  for (int q = 1; q < n; ++q) {
    float s;
    while (true) {
      s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * q - 2.0f * v[k]);
      if (s <= z[k] && k > 0) --k;
      else break;
    }
    if (s <= z[k]) { v[0] = q; z[0] = -1e30f; z[1] = 1e30f; k = 0; continue; }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = 1e30f;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    d[q] = (float(q) - v[k]) * (float(q) - v[k]) + f[v[k]];
  }
}

// squared distance to the nearest "on" pixel, for every pixel of a w x h grid
static void distanceTransform(const std::vector<uint8_t>& on, int w, int h, std::vector<float>& out) {
  const float INF = 1e20f;
  out.assign(size_t(w) * h, 0);
  int n = std::max(w, h);
  std::vector<float> f(n), d(n), z(n + 1);
  std::vector<int> v(n);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[y] = on[size_t(y) * w + x] ? 0 : INF;
    edt1d(f.data(), h, d.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) out[size_t(y) * w + x] = d[y];
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) f[x] = out[size_t(y) * w + x];
    edt1d(f.data(), w, d.data(), v.data(), z.data());
    for (int x = 0; x < w; ++x) out[size_t(y) * w + x] = d[x];
  }
}

void App::growSelection(int px) {
  if (!selActive || px == 0 || !R.hasDocument()) return;
  int W = int(R.docW), H = int(R.docH);
  int r = std::abs(px);
  int x0 = std::max(0, selX0 - r - 1), y0 = std::max(0, selY0 - r - 1);
  int x1 = std::min(W, selX1 + r + 1), y1 = std::min(H, selY1 + r + 1);
  int w = x1 - x0, h = y1 - y0;
  std::vector<uint8_t> in(size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      bool s = selCpu[size_t(y0 + y) * W + x0 + x] >= 128;
      in[size_t(y) * w + x] = px > 0 ? s : !s;  // shrink = grow the outside
    }
  std::vector<float> d2;
  distanceTransform(in, w, h, d2);
  std::vector<uint8_t> cov(size_t(w) * h);
  for (size_t i = 0; i < cov.size(); ++i) {
    float a = std::clamp(float(r) + 0.5f - std::sqrt(d2[i]), 0.0f, 1.0f);  // 1 = inside the grown area
    if (px < 0) a = 1.0f - a;
    cov[i] = uint8_t(a * 255 + 0.5f);
  }
  combineSelection(x0, y0, uint32_t(w), uint32_t(h), cov, 0);
}

// ---------------------------------------------------------------------------
// Selection action bar: a small tool strip under the selection (like Clip Studio Paint).

void App::drawSelectionBar() {
  if (!selActive || xf.active || toolDrag || hideUI || !R.hasDocument() || adj.open) return;
  // bottom-centre of the selection on screen
  double cx[4] = {double(selX0), double(selX1), double(selX1), double(selX0)}, cy[4] = {double(selY0), double(selY0), double(selY1), double(selY1)};
  float minX = 1e9f, maxX = -1e9f, maxY = -1e9f;
  for (int i = 0; i < 4; ++i) {
    float sx, sy;
    docToScreen(cx[i], cy[i], sx, sy);
    minX = std::min(minX, sx); maxX = std::max(maxX, sx); maxY = std::max(maxY, sy);
  }
  float fb = ImGui::GetIO().DisplayFramebufferScale.x;
  float sc = ImGui::GetStyle().FontScaleDpi;
  float bs = 26 * sc;
  const int nButtons = 10;
  ImVec2 size(nButtons * (bs + 2) + 12 * sc, bs + 12 * sc);
  float x = (minX + maxX) * 0.5f / fb - size.x * 0.5f, y = maxY / fb + 10 * sc;
  float cx0 = canvasX / fb, cy0 = canvasY / fb, cx1 = (canvasX + canvasW) / fb, cy1 = (canvasY + canvasH) / fb;
  x = std::clamp(x, cx0 + 4, std::max(cx0 + 4, cx1 - size.x - 4));
  y = std::clamp(y, cy0 + 4, std::max(cy0 + 4, cy1 - size.y - 4));
  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowBgAlpha(0.94f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * sc, 6 * sc));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * sc);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##selectionbar", nullptr, fl)) {
    bool busy = R.busy() || clipping;
    if (iconButton("##d", "x", bs, false, ("Deselect  " + comboLabel(keys[int(Act::Deselect)][0])).c_str())) deselect();
    ImGui::SameLine();
    if (iconButton("##i", "contrast", bs, false, "Invert selection")) invertSelection();
    ImGui::SameLine();
    if (iconButton("##g", "maximize-2", bs, false, "Grow selection...")) { selGrowShrink = 1; ImGui::OpenPopup("growshrink"); }
    ImGui::SameLine();
    if (iconButton("##s", "minimize-2", bs, false, "Shrink selection...")) { selGrowShrink = -1; ImGui::OpenPopup("growshrink"); }
    ImGui::SameLine();
    if (iconButton("##c", "eraser", bs, false, "Clear inside the selection", !busy)) fillSelection(true);
    ImGui::SameLine();
    if (iconButton("##o", "brush-cleaning", bs, false, "Clear outside the selection", !busy)) clearOutsideSelection();
    ImGui::SameLine();
    if (iconButton("##f", "paint-bucket", bs, false, "Fill with the drawing colour", !busy)) fillSelection(false);
    ImGui::SameLine();
    if (iconButton("##cp", "copy", bs, false, "Copy to a new layer", !busy)) copySelection(false, true);
    ImGui::SameLine();
    if (iconButton("##ct", "scissors", bs, false, "Cut to a new layer", !busy)) copySelection(true, true);
    ImGui::SameLine();
    if (iconButton("##t", "move", bs, false, "Transform the selection", !busy)) setTool(ToolId::Transform);
    if (ImGui::BeginPopup("growshrink")) {
      ImGui::TextUnformatted(selGrowShrink > 0 ? "Grow selection by" : "Shrink selection by");
      ImGui::SetNextItemWidth(160 * sc);
      ImGui::SliderInt("##px", &selGrowPx, 1, 200, "%d px", ImGuiSliderFlags_Logarithmic);
      if (ImGui::Button("Apply", ImVec2(160 * sc, 0))) { growSelection(selGrowShrink * selGrowPx); ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}
