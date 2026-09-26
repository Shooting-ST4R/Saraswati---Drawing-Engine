// Navigator (whole-picture preview, zoom / rotation / flip) and Tool Group (sub tools with stroke
// previews), laid out like Clip Studio Paint's panels.
#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static constexpr double kPiP = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// View helpers

// Changes the view while the point under the canvas centre stays put.
void App::keepCanvasCentre(const std::function<void()>& change) {
  double cx = canvasX + canvasW * 0.5, cy = canvasY + canvasH * 0.5, bx, by, ax, ay;
  screenToDoc(cx, cy, bx, by);
  change();
  screenToDoc(cx, cy, ax, ay);
  view.panX += bx - ax;
  view.panY += by - ay;
}

void App::rotateView(double radians) {
  keepCanvasCentre([&] {
    double r = std::remainder(view.rotation + radians, 2 * kPiP);
    if (std::abs(r) < 1e-9) r = 0;
    view.rotation = r;
  });
}

// Flipping only changes how the picture is shown (like a mirror); the layers are never touched.
void App::flipView() { keepCanvasCentre([&] { view.flipX = !view.flipX; }); }
void App::flipViewV() {
  keepCanvasCentre([&] {
    view.flipX = !view.flipX;
    double r = std::remainder(view.rotation + kPiP, 2 * kPiP);
    view.rotation = std::abs(r) < 1e-9 ? 0 : r;
  });
}

// ---------------------------------------------------------------------------
// Navigator

static void drawFlipGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool vertical = false) {
  if (vertical) {
    float h = s * 0.36f, w = s * 0.3f, g = s * 0.07f;
    for (float x = -h; x < h; x += s * 0.14f) dl->AddLine(ImVec2(c.x + x, c.y), ImVec2(c.x + std::min(h, x + s * 0.07f), c.y), col, 1.0f);
    dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - g), ImVec2(c.x + h * 0.7f, c.y - g), ImVec2(c.x + h * 0.7f, c.y - g - w), col);
    dl->AddTriangle(ImVec2(c.x - h * 0.7f, c.y + g), ImVec2(c.x + h * 0.7f, c.y + g), ImVec2(c.x + h * 0.7f, c.y + g + w), col, 1.2f);
    return;
  }
  float h = s * 0.36f, w = s * 0.3f, g = s * 0.07f;
  for (float y = -h; y < h; y += s * 0.14f) dl->AddLine(ImVec2(c.x, c.y + y), ImVec2(c.x, c.y + std::min(h, y + s * 0.07f)), col, 1.0f);
  dl->AddTriangleFilled(ImVec2(c.x - g, c.y - h * 0.7f), ImVec2(c.x - g, c.y + h * 0.7f), ImVec2(c.x - g - w, c.y + h * 0.7f), col);
  dl->AddTriangle(ImVec2(c.x + g, c.y - h * 0.7f), ImVec2(c.x + g, c.y + h * 0.7f), ImVec2(c.x + g + w, c.y + h * 0.7f), col, 1.2f);
}

void App::drawNavigator() {
  R.navWanted = false;
  if (!ImGui::Begin("Navigator", &showNav)) { ImGui::End(); return; }
  if (!R.hasDocument()) { ImGui::TextDisabled("No document"); ImGui::End(); return; }
  R.navWanted = true;
  const ImGuiStyle& st = ImGui::GetStyle();
  float fh = ImGui::GetFrameHeight();
  float ctrlH = fh * 3 + st.ItemSpacing.y * 3;
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float ph = std::max(fh * 2, avail.y - ctrlH);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##navpreview", ImVec2(std::max(avail.x, 10.0f), ph));
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(p0.x + avail.x, p0.y + ph);
  dl->AddRectFilled(p0, p1, IM_COL32(38, 38, 38, 255));
  double dw = R.docW, dh = R.docH;
  float s = float(std::min((avail.x - 8) / dw, (ph - 8) / dh));
  if (s > 0) {
    float w = float(dw * s), h = float(dh * s);
    ImVec2 o(p0.x + (avail.x - w) * 0.5f, p0.y + (ph - h) * 0.5f);
    dl->PushClipRect(p0, p1, true);
    if (!R.whitePaper || R.paperColor[0] < 1 || R.paperColor[1] < 1 || R.paperColor[2] < 1) {  // checkerboard behind
      float cs = 6;
      dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(200, 200, 200, 255));
      for (float y = 0; y < h; y += cs)
        for (float x = (int(y / cs) & 1) ? cs : 0; x < w; x += 2 * cs)
          dl->AddRectFilled(ImVec2(o.x + x, o.y + y), ImVec2(o.x + std::min(w, x + cs), o.y + std::min(h, y + cs)),
                            IM_COL32(150, 150, 150, 255));
    }
    if (R.navTexture()) {
      ImVec2 uv0(view.flipX ? 1.0f : 0.0f, 0), uv1(view.flipX ? 0.0f : 1.0f, 1);
      dl->AddImage(ImTextureRef(ImTextureID(uint64_t(R.navTexture()))), o, ImVec2(o.x + w, o.y + h), uv0, uv1);
    }
    dl->AddRect(ImVec2(o.x - 1, o.y - 1), ImVec2(o.x + w + 1, o.y + h + 1), IM_COL32(20, 20, 20, 255));
    // the part of the picture the canvas shows
    auto toNav = [&](double sx, double sy) {
      double dx, dy;
      screenToDoc(sx, sy, dx, dy);
      if (view.flipX) dx = dw - dx;
      return ImVec2(o.x + float(dx * s), o.y + float(dy * s));
    };
    ImVec2 q[4] = {toNav(canvasX, canvasY), toNav(canvasX + canvasW, canvasY), toNav(canvasX + canvasW, canvasY + canvasH),
                   toNav(canvasX, canvasY + canvasH)};
    dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(0, 0, 0, 160), 3.0f);
    dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(235, 70, 60, 255), 1.6f);
    dl->PopClipRect();
    // drag / click: centre the canvas on that point
    if (active) {
      ImVec2 m = ImGui::GetIO().MousePos;
      double dx = (m.x - o.x) / s, dy = (m.y - o.y) / s;
      if (view.flipX) dx = dw - dx;
      double cx, cy;
      screenToDoc(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, cx, cy);
      view.panX += dx - cx;
      view.panY += dy - cy;
    }
    if (hovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      float wheel = ImGui::GetIO().MouseWheel;
      if (wheel != 0) zoomAt(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, std::pow(prefs.wheelZoom, wheel));
    }
  }
  // zoom
  float bs = fh;
  float sliderW = ImGui::GetContentRegionAvail().x - 2 * (bs + st.ItemSpacing.x);
  if (iconButton("##zo", "zoom-out", bs, false, "Zoom out")) zoomAt(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, 0.8);
  ImGui::SameLine();
  float zp = float(view.zoom * 100);
  ImGui::SetNextItemWidth(sliderW);
  if (ImGui::SliderFloat("##zoom", &zp, 1.0f, 6400.0f, "%.1f %%", ImGuiSliderFlags_Logarithmic)) {
    double f = std::clamp(zp / 100.0, 0.005, 64.0) / view.zoom;
    zoomAt(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, f);
  }
  ImGui::SameLine();
  if (iconButton("##zi", "zoom-in", bs, false, "Zoom in")) zoomAt(canvasX + canvasW * 0.5, canvasY + canvasH * 0.5, 1.25);
  // rotation
  if (iconButton("##rl", "rotate-ccw", bs, false, "Rotate left 15 deg")) rotateView(-kPiP / 12);
  ImGui::SameLine();
  float deg = float(view.rotation * 180 / kPiP);
  ImGui::SetNextItemWidth(sliderW);
  if (ImGui::SliderFloat("##rot", &deg, -180.0f, 180.0f, "%.0f deg")) rotateView(deg * kPiP / 180 - view.rotation);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate the view (the picture itself is not changed)");
  ImGui::SameLine();
  if (iconButton("##rr", "rotate-cw", bs, false, "Rotate right 15 deg")) rotateView(kPiP / 12);
  // buttons
  if (iconButton("##fit", "maximize", bs, false, "Fit to window")) fitView();
  ImGui::SameLine();
  if (ImGui::Button("1:1", ImVec2(bs * 1.4f, bs))) keepCanvasCentre([&] { view.zoom = 1; });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("100 %% (one picture pixel per screen pixel)");
  ImGui::SameLine();
  if (iconButton("##r0", "refresh-ccw", bs, false, "Reset rotation")) rotateView(-view.rotation);
  ImGui::SameLine();
  {
    ImGui::PushStyleColor(ImGuiCol_Button, view.flipX ? st.Colors[ImGuiCol_ButtonActive] : ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##flip", ImVec2(bs, bs))) flipView();
    ImGui::PopStyleColor();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    drawFlipGlyph(ImGui::GetWindowDrawList(), ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), bs,
                  ImGui::GetColorU32(ImGuiCol_Text, view.flipX ? 1.0f : 0.72f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip the view horizontally (to check proportions; the picture is not changed)");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##flipv", ImVec2(bs, bs))) flipViewV();
    ImGui::PopStyleColor();
    mn = ImGui::GetItemRectMin(); mx = ImGui::GetItemRectMax();
    drawFlipGlyph(ImGui::GetWindowDrawList(), ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), bs, ImGui::GetColorU32(ImGuiCol_Text, 0.72f), true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flip the view vertically (view only - the picture is not changed; text shows mirrored)");
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// Brush stroke previews (CPU): the real brush engine lays the dabs of an S-curve with a pressure
// taper; they are rasterised with the same tip model as the GPU (dabs.comp).

static float hashNoise(int x, int y) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
static float valueNoise(float x, float y) {
  int ix = int(std::floor(x)), iy = int(std::floor(y));
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  float a = hashNoise(ix, iy), b = hashNoise(ix + 1, iy), c = hashNoise(ix, iy + 1), d = hashNoise(ix + 1, iy + 1);
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
static float grainAt(float x, float y, float scale) {
  float qx = x / std::max(scale, 0.25f), qy = y / std::max(scale, 0.25f);
  float n = 0.5f * valueNoise(qx, qy) + 0.25f * valueNoise(qx * 2.03f + 17.1f, qy * 2.03f + 17.1f) +
            0.125f * valueNoise(qx * 4.01f + 31.7f, qy * 4.01f + 31.7f);
  float t = std::clamp((n / 0.875f - 0.25f) / 0.5f, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

static void renderStrokePreview(const BrushSettings& src, int W, int H, uint8_t* rgba, int stride, float grey) {
  BrushSettings b = src;
  float d = H * 0.42f;  // every preview uses the same stroke width
  float k = d / std::max(1.0f, src.size);
  b.size = d;
  b.particleSize = std::max(0.6f, src.particleSize * k);
  b.texScale = std::max(1.0f, src.texScale * std::min(1.0f, k * 4));
  BrushEngine e;
  const int N = 48;
  float pad = d * 0.7f;
  auto sample = [&](int i) {
    float t = float(i) / N;
    PenSample p;
    p.x = pad + t * (W - 2 * pad);
    p.y = H * 0.5 + std::sin(t * 2 * kPiP) * H * 0.2;
    p.pressure = 0.2f + 0.8f * float(std::sin(t * kPiP));
    return p;
  };
  e.begin(sample(0), b, 12345u, true);
  for (int i = 1; i <= N; ++i) e.add(sample(i));
  e.end();
  std::vector<float> m(size_t(W) * H, 0.0f);
  for (const Dab& dab : e.out) {
    int x0 = std::max(0, int(dab.x - dab.radius - 1)), x1 = std::min(W - 1, int(dab.x + dab.radius + 1));
    int y0 = std::max(0, int(dab.y - dab.radius - 1)), y1 = std::min(H - 1, int(dab.y + dab.radius + 1));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        float vx = x + 0.5f - dab.x, vy = y + 0.5f - dab.y;
        if (vx * vx + vy * vy >= (dab.radius + 1) * (dab.radius + 1)) continue;
        float rx = dab.cosA * vx + dab.sinA * vy, ry = (-dab.sinA * vx + dab.cosA * vy) * dab.invRound;
        float dist = std::sqrt(rx * rx + ry * ry);
        float cov = std::clamp(dab.radius - dist + 0.5f, 0.0f, 1.0f);
        if (b.hardness < 0.999f) {
          float inner = b.hardness * dab.radius;
          float t = std::clamp((dist - inner) / std::max(1e-3f, dab.radius + 1e-3f - inner), 0.0f, 1.0f);
          cov = std::min(cov, 1.0f - t * t * (3 - 2 * t));
        }
        float tex = b.texStrength > 0 ? 1.0f + (grainAt(x + 0.5f, y + 0.5f, b.texScale) - 1.0f) * b.texStrength : 1.0f;
        float a = cov * tex * dab.alpha;
        float& v = m[size_t(y) * W + x];
        v = b.buildUp ? v + a * (1 - v) : std::max(v, a);
      }
  }
  uint8_t g = uint8_t(std::clamp(grey, 0.0f, 1.0f) * 255);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      uint8_t* p = rgba + size_t(y) * stride + size_t(x) * 4;
      p[0] = p[1] = p[2] = g;
      p[3] = uint8_t(std::clamp(m[size_t(y) * W + x] * b.opacity, 0.0f, 1.0f) * 255 + 0.5f);
    }
}

// One atlas row per brush; rebuilt (throttled) when any brush's settings change.
void App::updateBrushPreviews() {
  float sc = ImGui::GetStyle().FontScaleDpi;
  int W = std::clamp(int(220 * sc), 120, 640), H = std::clamp(int(30 * sc), 16, 90);
  std::string key = std::to_string(W) + "x" + std::to_string(H);
  for (const BrushSettings& b : brushes) {
    char buf[256];
    snprintf(buf, sizeof buf, "|%g %g %g %g %g %g %g %d %g %g %g %g %g %d %d %g %d %g %d %g", b.size, b.opacity, b.flow,
             b.spacing, b.hardness, b.roundness, b.angle, int(b.followStroke), b.texStrength, b.texScale, b.scatter,
             b.sizeJitter, b.flowJitter, int(b.buildUp), b.particles, b.particleSize, int(b.pressureSize), b.minSize,
             int(b.pressureOpacity), b.gamma);
    key += buf;
  }
  if (key == previewKey && previewAtlas.imguiTex) return;
  uint64_t now = SDL_GetTicksNS();
  if (previewAtlas.imguiTex && now - previewBuiltNs < 250000000ull) return;  // a slider is moving: at most 4 per second
  if (engine.active() || R.busy()) return;
  std::vector<uint8_t> px(size_t(W) * H * 4 * brushes.size(), 0);
  for (size_t i = 0; i < brushes.size(); ++i)
    renderStrokePreview(brushes[i], W, H, px.data() + i * size_t(W) * H * 4, W * 4, 0.88f);
  R.destroyFloating(previewAtlas);
  std::string err;
  if (!R.createFloating(uint32_t(W), uint32_t(H * brushes.size()), px.data(), previewAtlas, err)) return;
  previewKey = key;
  previewBuiltNs = now;
  previewW = W;
  previewH = H;
}

// ---------------------------------------------------------------------------
// Tool Group

static const char* brushGroupIcon(const std::string& g) {
  if (g == "Pencil") return "pencil";
  if (g == "Ink") return "pen-tool";
  if (g == "Marker") return "highlighter";
  if (g == "Paint") return "brush";
  if (g == "Airbrush") return "spray-can";
  if (g == "Eraser") return "eraser";
  return "feather";
}

void App::drawToolGroup() {
  if (!ImGui::Begin("Tool Group", &showToolGroup)) { ImGui::End(); return; }
  const ImGuiStyle& st = ImGui::GetStyle();
  float fh = ImGui::GetFrameHeight();
  if (tool != ToolId::Brush) {
    // the tools that belong together, as a list (like CSP's sub tools)
    struct Row { Act act; const char* icon; };
    static const Row fill[] = {{Act::ToolFill, "paint-bucket"}, {Act::ToolGradient, nullptr}};
    static const Row figure[] = {{Act::ToolLine, "slash"}, {Act::ToolRect, "square"}, {Act::ToolEllipse, "circle"}};
    static const Row select[] = {{Act::ToolSelRect, "square-dashed"}, {Act::ToolSelEllipse, "circle-dashed"},
                                 {Act::ToolLasso, "lasso"}, {Act::ToolWand, "wand-sparkles"}};
    static const Row pick[] = {{Act::ToolEyedropper, "pipette"}};
    static const Row move[] = {{Act::ToolTransform, "move"}, {Act::ToolHand, "hand"}};
    static const Row text[] = {{Act::ToolText, "type"}};
    const Row* rows = pick;
    int n = 1;
    const char* title = "Eyedropper";
    switch (tool) {
      case ToolId::Fill: case ToolId::Gradient: rows = fill; n = 2; title = "Fill"; break;
      case ToolId::Line: case ToolId::Rect: case ToolId::Ellipse: rows = figure; n = 3; title = "Figure"; break;
      case ToolId::SelRect: case ToolId::SelEllipse: case ToolId::Lasso: case ToolId::Wand: rows = select; n = 4; title = "Selection"; break;
      case ToolId::Transform: case ToolId::Hand: rows = move; n = 2; title = "Move & view"; break;
      case ToolId::Text: rows = text; n = 1; title = "Text"; break;
      default: break;
    }
    ImGui::SeparatorText(title);
    for (int i = 0; i < n; ++i) {
      ImGui::PushID(i);
      bool sel = toolActActive(rows[i].act);
      ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##t", sel, 0, ImVec2(0, fh * 1.3f))) selectToolAct(rows[i].act);
      float ic = ImGui::GetTextLineHeight();
      ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ic * 2.0f, p.y + (fh * 1.3f - ic) * 0.5f),
                                          ImGui::GetColorU32(ImGuiCol_Text, sel ? 1.0f : 0.8f), actionInfo(rows[i].act).name);
      ImVec2 c(p.x + ic * 0.9f, p.y + fh * 0.65f);
      if (rows[i].icon) drawIcon(ImGui::GetWindowDrawList(), rows[i].icon, c, ic * 1.1f, ImGui::GetColorU32(ImGuiCol_Text, sel ? 1.0f : 0.72f));
      else {  // gradient
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(ImVec2(c.x - ic * 0.5f, c.y - ic * 0.35f), ImVec2(c.x + ic * 0.5f, c.y + ic * 0.35f),
                                    IM_COL32(230, 230, 230, 255), IM_COL32(60, 60, 60, 255), IM_COL32(60, 60, 60, 255), IM_COL32(230, 230, 230, 255));
      }
      std::string k = comboLabel(keys[int(rows[i].act)][0]);
      if (!k.empty()) {
        ImVec2 ks = ImGui::CalcTextSize(k.c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMax().x - ks.x - 6, p.y + (fh * 1.3f - ks.y) * 0.5f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), k.c_str());
      }
      ImGui::PopID();
    }
    ImGui::End();
    return;
  }
  // brush groups as icon tabs
  std::vector<std::string> groups;
  for (const BrushSettings& b : brushes)
    if (std::find(groups.begin(), groups.end(), b.group) == groups.end()) groups.push_back(b.group);
  if (toolGroupShown.empty() || std::find(groups.begin(), groups.end(), toolGroupShown) == groups.end() ||
      lastTipForGroup != tipIndex) {
    toolGroupShown = brushes[tipIndex].group;  // follow the current brush (e.g. keys 1-9, the eraser tool)
    lastTipForGroup = tipIndex;
  }
  float bs = fh * 1.25f;
  int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + st.ItemSpacing.x) / (bs + st.ItemSpacing.x)));
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    ImGui::PushID(int(gi));
    if (gi % perRow) ImGui::SameLine();
    if (iconButton("##g", brushGroupIcon(groups[gi]), bs, groups[gi] == toolGroupShown, groups[gi].c_str())) toolGroupShown = groups[gi];
    ImGui::PopID();
  }
  ImGui::SeparatorText(toolGroupShown.c_str());
  updateBrushPreviews();
  // brushes of the shown group: name + stroke preview
  float rowH = ImGui::GetTextLineHeight() + (previewH > 0 ? previewH : fh) + st.ItemSpacing.y * 2;
  float listH = ImGui::GetContentRegionAvail().y - fh - st.ItemSpacing.y * 2;
  ImGui::BeginChild("subtools", ImVec2(0, std::max(listH, rowH)), ImGuiChildFlags_Borders);
  for (int i = 0; i < int(brushes.size()); ++i) {
    const BrushSettings& b = brushes[i];
    if (b.group != toolGroupShown) continue;
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##b", i == tipIndex, 0, ImVec2(w, rowH))) tipIndex = i;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float tp = st.ItemSpacing.y;
    dl->AddText(ImVec2(p.x + 4, p.y + tp), ImGui::GetColorU32(ImGuiCol_Text, i == tipIndex ? 1.0f : 0.8f), b.name.c_str());
    for (int s = 0; s < 9; ++s)
      if (s == i) {
        std::string k = comboLabel(keys[int(Act::Brush1) + s][0]);
        ImVec2 ks = ImGui::CalcTextSize(k.c_str());
        dl->AddText(ImVec2(p.x + w - ks.x - 6, p.y + tp), ImGui::GetColorU32(ImGuiCol_TextDisabled), k.c_str());
      }
    if (previewAtlas.imguiTex && previewH > 0) {
      float pw = std::min(w - 8, float(previewW)), ph = float(previewH) * pw / float(previewW);
      ImVec2 a(p.x + 4, p.y + tp + ImGui::GetTextLineHeight());
      float v0 = float(i) / brushes.size(), v1 = float(i + 1) / brushes.size();
      dl->AddImage(ImTextureRef(ImTextureID(uint64_t(previewAtlas.imguiTex))), a, ImVec2(a.x + pw, a.y + ph), ImVec2(0, v0), ImVec2(1, v1));
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  drawBrushLibraryButtons();
  ImGui::End();
}

// ---------------------------------------------------------------------------
// Layer tree: folders (contents stored directly below the folder entry) and attached sublayers
// (a speech bubble right below its text layer). Every move / delete works on whole nodes and is
// written back as one reorder (one undo step, folder membership included).

uint32_t App::topLevelId(int index) const {
  const Layer& l = R.layers[size_t(index)];
  return isSublayer(l) ? l.parentId : l.id;
}

namespace {
struct TNode {
  uint32_t id = 0;
  bool folder = false;
  std::vector<uint32_t> attached;  // sublayers kept right below
  std::vector<TNode> kids;         // folder contents, bottom to top
};

std::vector<TNode> buildTree(const Renderer& R) {
  auto isSub = [&](const Layer& l) { return l.parentId && R.indexOf(l.parentId) >= 0; };
  std::map<uint32_t, std::vector<int>> byFolder;
  for (int i = 0; i < int(R.layers.size()); ++i) {
    const Layer& l = R.layers[size_t(i)];
    if (isSub(l)) continue;
    uint32_t f = l.folderId && R.indexOf(l.folderId) >= 0 && R.layers[size_t(R.indexOf(l.folderId))].folder ? l.folderId : 0;
    byFolder[f].push_back(i);
  }
  std::function<std::vector<TNode>(uint32_t, int)> build = [&](uint32_t f, int depth) {
    std::vector<TNode> out;
    if (depth > 32) return out;
    for (int i : byFolder[f]) {
      const Layer& l = R.layers[size_t(i)];
      TNode n;
      n.id = l.id;
      n.folder = l.folder;
      for (const Layer& s : R.layers)
        if (isSub(s) && s.parentId == l.id) n.attached.push_back(s.id);
      if (l.folder) n.kids = build(l.id, depth + 1);
      out.push_back(std::move(n));
    }
    return out;
  };
  return build(0, 0);
}

void flattenTree(const std::vector<TNode>& nodes, uint32_t folder, std::vector<uint32_t>& ids, std::vector<uint32_t>& fids) {
  for (const TNode& n : nodes) {
    if (n.folder) flattenTree(n.kids, n.id, ids, fids);
    for (uint32_t a : n.attached) { ids.push_back(a); fids.push_back(folder); }
    ids.push_back(n.id);
    fids.push_back(folder);
  }
}

// finds the list holding node `id` and its position there
bool findNode(std::vector<TNode>& list, uint32_t id, std::vector<TNode>*& owner, size_t& pos, TNode*& parent, TNode* up = nullptr) {
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].id == id) { owner = &list; pos = i; parent = up; return true; }
    if (findNode(list[i].kids, id, owner, pos, parent, &list[i])) return true;
  }
  return false;
}

bool subtreeHas(const TNode& n, uint32_t id) {
  if (n.id == id) return true;
  for (const TNode& k : n.kids)
    if (subtreeHas(k, id)) return true;
  return false;
}
}  // namespace

void App::applyTree(const void* treePtr) {
  const auto& tree = *static_cast<const std::vector<TNode>*>(treePtr);
  std::vector<uint32_t> ids, fids;
  flattenTree(tree, 0, ids, fids);
  if (ids.size() != R.layers.size()) return;  // never lose a layer
  uint32_t act = R.layers[size_t(active)].id;
  R.reorderLayers(ids, fids);
  active = std::max(0, R.indexOf(act));
}

void App::moveLayerBlock(int index, int dir) {
  std::vector<TNode> tree = buildTree(R);
  std::vector<TNode>* owner = nullptr;
  size_t pos = 0;
  TNode* parent = nullptr;
  if (!findNode(tree, topLevelId(index), owner, pos, parent)) return;
  if (dir > 0 && pos + 1 < owner->size()) std::swap((*owner)[pos], (*owner)[pos + 1]);
  else if (dir < 0 && pos > 0) std::swap((*owner)[pos], (*owner)[pos - 1]);
  else if (parent) {  // at the edge of a folder: move out of it, next to the folder
    TNode n = std::move((*owner)[pos]);
    owner->erase(owner->begin() + long(pos));
    std::vector<TNode>* powner = nullptr;
    size_t ppos = 0;
    TNode* pp = nullptr;
    if (!findNode(tree, parent->id, powner, ppos, pp)) return;
    powner->insert(powner->begin() + long(dir > 0 ? ppos + 1 : ppos), std::move(n));
  } else {
    return;
  }
  applyTree(&tree);
}

void App::dropLayerBlock(int from, int onto) {
  if (from == onto) return;
  uint32_t a = topLevelId(from), b = topLevelId(onto);
  if (a == b) return;
  std::vector<TNode> tree = buildTree(R);
  std::vector<TNode>* owner = nullptr;
  size_t pos = 0;
  TNode* parent = nullptr;
  if (!findNode(tree, a, owner, pos, parent)) return;
  if (subtreeHas((*owner)[pos], b)) return;  // not into itself
  TNode n = std::move((*owner)[pos]);
  owner->erase(owner->begin() + long(pos));
  std::vector<TNode>* towner = nullptr;
  size_t tpos = 0;
  TNode* tparent = nullptr;
  if (!findNode(tree, b, towner, tpos, tparent)) return;
  TNode& target = (*towner)[tpos];
  if (target.folder) target.kids.push_back(std::move(n));  // onto a folder: inside, on top
  else towner->insert(towner->begin() + long(from < onto ? tpos + 1 : tpos), std::move(n));
  applyTree(&tree);
}

void App::deleteLayerBlock(int index) {
  std::vector<TNode> tree = buildTree(R);
  std::vector<TNode>* owner = nullptr;
  size_t pos = 0;
  TNode* parent = nullptr;
  if (!findNode(tree, topLevelId(index), owner, pos, parent)) return;
  std::vector<uint32_t> ids, fids;
  flattenTree(std::vector<TNode>{(*owner)[pos]}, 0, ids, fids);
  int imageLayersLeft = 0;
  for (const Layer& l : R.layers)
    if (!l.folder && std::find(ids.begin(), ids.end(), l.id) == ids.end()) ++imageLayersLeft;
  if (imageLayersLeft == 0) { showToast("The last layer cannot be deleted"); return; }
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) R.deleteLayer(R.indexOf(*it));
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
  if (R.layers[size_t(active)].folder) {  // prefer a drawable layer
    for (int i = active; i >= 0; --i) if (!R.layers[size_t(i)].folder) { active = i; break; }
  }
}

int App::layerDepth(int index) const {
  int d = 0;
  uint32_t f = R.layers[size_t(index)].folderId;
  while (f && d < 32) {
    int i = R.indexOf(f);
    if (i < 0) break;
    ++d;
    f = R.layers[size_t(i)].folderId;
  }
  return d;
}

bool App::layerShown(int index) const {  // false when an enclosing folder is collapsed
  uint32_t f = R.layers[size_t(index)].folderId;
  int guard = 0;
  while (f && guard++ < 32) {
    int i = R.indexOf(f);
    if (i < 0) break;
    if (!R.layers[size_t(i)].expanded) return false;
    f = R.layers[size_t(i)].folderId;
  }
  if (isSublayer(R.layers[size_t(index)])) {
    int p = R.indexOf(R.layers[size_t(index)].parentId);
    if (p >= 0) return layerShown(p);
  }
  return true;
}

bool App::needPixelLayer() {
  if (!R.hasDocument()) return false;
  if (paperSelected) {
    showToast("The paper is selected - pick a layer to draw on");
    return false;
  }
  if (R.layers[size_t(active)].folder) {
    showToast("A folder is selected - pick a layer inside it to draw or edit");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Layer Properties: non-destructive effects of the active layer (like Clip Studio Paint)

void App::drawLayerProperties() {
  if (!ImGui::Begin("Layer Properties", &showLayerProps)) { ImGui::End(); return; }
  if (!R.hasDocument()) { ImGui::TextDisabled("No document"); ImGui::End(); return; }
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
  if (paperSelected) {  // the paper has one option: its colour
    ImGui::TextDisabled("Paper");
    ImGui::SeparatorText("Paper colour");
    if (ImGui::ColorEdit3("##paper", R.paperColor, ImGuiColorEditFlags_NoAlpha)) { R.markCachesDirty(); R.bumpRevision(); }
    if (ImGui::Button("White")) { R.paperColor[0] = R.paperColor[1] = R.paperColor[2] = 1; R.markCachesDirty(); R.bumpRevision(); }
    ImGui::SameLine();
    if (ImGui::Button("Use drawing colour")) { for (int k = 0; k < 3; ++k) R.paperColor[k] = color[k]; R.markCachesDirty(); R.bumpRevision(); }
    ImGui::TextDisabled("The paper is not pixels: one colour under the whole\npicture. It is always the bottom of the stack.");
    ImGui::End();
    return;
  }
  Layer& L = R.layers[size_t(active)];
  ImGui::TextDisabled("%s", L.name.c_str());
  bool changed = false;
  // one undo step per edit: record the old values when a control is grabbed
  auto track = [&](bool edited) {
    if (ImGui::IsItemActivated()) { propsBefore = {L.tone, L.lcolor}; propsEditing = true; }
    if (edited) changed = true;
    if (ImGui::IsItemDeactivatedAfterEdit() && propsEditing) {
      ToneFx nt = L.tone;
      LayerColorFx nc = L.lcolor;
      L.tone = propsBefore.first;
      L.lcolor = propsBefore.second;
      R.recordLayerProps(active);
      L.tone = nt;
      L.lcolor = nc;
      propsEditing = false;
    }
  };
  auto instant = [&](bool edited, const std::function<void()>& apply) {  // checkboxes / combos: one undo step each
    if (!edited) return;
    ToneFx nt = L.tone;
    LayerColorFx nc = L.lcolor;
    apply();
    std::swap(nt, L.tone);
    std::swap(nc, L.lcolor);
    R.recordLayerProps(active);
    L.tone = nt;
    L.lcolor = nc;
    changed = true;
  };
  ImGui::SeparatorText("Effect");
  float fh = ImGui::GetFrameHeight();
  {
    bool lc = L.lcolor.on, tn = L.tone.on;
    if (iconButton("##fxlc", "palette", fh * 1.3f, lc, "Layer colour: show the layer in one colour (non-destructive)"))
      instant(true, [&] { L.lcolor.on = !lc; });
    ImGui::SameLine();
    if (iconButton("##fxtone", "grip", fh * 1.3f, tn, "Tone: show the layer as manga screentone dots (non-destructive)"))
      instant(true, [&] { L.tone.on = !tn; });
  }
  if (L.lcolor.on) {
    ImGui::SeparatorText("Layer colour");
    float m[3] = {L.lcolor.main[0], L.lcolor.main[1], L.lcolor.main[2]};
    bool e = ImGui::ColorEdit3("Colour", m, ImGuiColorEditFlags_NoInputs);
    if (e) for (int k = 0; k < 3; ++k) L.lcolor.main[k] = m[k];
    track(e);
    ImGui::SameLine();
    float s[3] = {L.lcolor.sub[0], L.lcolor.sub[1], L.lcolor.sub[2]};
    ImGui::BeginDisabled(!L.lcolor.useSub);
    e = ImGui::ColorEdit3("Sub colour", s, ImGuiColorEditFlags_NoInputs);
    if (e) for (int k = 0; k < 3; ++k) L.lcolor.sub[k] = s[k];
    track(e);
    ImGui::EndDisabled();
    bool us = L.lcolor.useSub;
    if (ImGui::Checkbox("Light parts in the sub colour", &us)) instant(true, [&] { L.lcolor.useSub = us; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: light / white parts become transparent");
  }
  if (L.tone.on) {
    ToneFx& t = L.tone;
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SeparatorText("Tone");
    ImGui::SetNextItemWidth(w);
    char ff[64];
    snprintf(ff, sizeof ff, "Frequency %%.1f lpi (%.0f dpi)", R.docDpi);
    track(ImGui::SliderFloat("##freq", &t.frequency, 5.0f, 200.0f, ff, ImGuiSliderFlags_Logarithmic));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines per inch: higher = smaller, finer dots.\nThe document resolution is set below.");
    const char* dens[] = {"Density: use colour of image", "Density: use opacity of image"};
    int dm = t.density;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##dens", &dm, dens, 2)) instant(true, [&] { t.density = dm; });
    bool ro = t.reflectOpacity;
    if (ImGui::Checkbox("Reflect layer opacity", &ro)) instant(true, [&] { t.reflectOpacity = ro; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The layer opacity makes the dots smaller instead of fading them");
    bool po = t.posterize;
    if (ImGui::Checkbox("Posterization", &po)) instant(true, [&] { t.posterize = po; });
    if (t.posterize) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
      track(ImGui::SliderInt("##lv", &t.levels, 2, 16, "%d levels"));
    }
    ImGui::SeparatorText("Dot settings");
    int sh = t.shape;
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##shape", toneShapeNames()[std::clamp(sh, 0, kToneShapes - 1)], ImGuiComboFlags_HeightLargest)) {
      for (int i = 0; i < kToneShapes; ++i)
        if (ImGui::Selectable(toneShapeNames()[i], i == sh)) instant(true, [&] { t.shape = i; });
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(w);
    track(ImGui::SliderFloat("##ang", &t.angle, -90.0f, 90.0f, "Angle %.0f deg"));
    ImGui::BeginDisabled(t.shape != 6);
    float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::SetNextItemWidth(half);
    track(ImGui::SliderFloat("##ns", &t.noiseSize, 1.0f, 20.0f, "Noise size %.1f"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    track(ImGui::SliderFloat("##nf", &t.noiseFactor, 0.0f, 1.0f, "Noise factor %.2f"));
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(half);
    track(ImGui::DragFloat("##ox", &t.offX, 0.25f, -10000, 10000, "Dot position X %.1f"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    track(ImGui::DragFloat("##oy", &t.offY, 0.25f, -10000, 10000, "Dot position Y %.1f"));
    ImGui::SeparatorText("Expression colour");
    const char* ex[] = {"Colour of the image", "Monochrome (black)"};
    int em = t.express;
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("##expr", &em, ex, 2)) instant(true, [&] { t.express = em; });
    ImGui::SeparatorText("Document");
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##dpi", &R.docDpi, 1.0f, 72.0f, 1200.0f, "Resolution %.0f dpi", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Print resolution of the page (manga pages: usually 600 dpi for line art\nand tones). Used for the tone frequency.");
  }
  if (!L.tone.on && !L.lcolor.on) ImGui::TextDisabled("Pick an effect above. Effects change only how the\nlayer looks - its pixels stay as they are.");
  if (changed) R.markCachesDirty();
  ImGui::End();
}
