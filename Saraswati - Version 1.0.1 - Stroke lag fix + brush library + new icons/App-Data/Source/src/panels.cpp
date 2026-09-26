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
    if (!R.whitePaper) {  // transparent paper: checkerboard behind
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
// Sublayers

uint32_t App::topLevelId(int index) const {
  const Layer& l = R.layers[size_t(index)];
  return isSublayer(l) ? l.parentId : l.id;
}

// top-level order (bottom to top) -> full order with every sublayer right below its parent
void App::applyTopOrder(const std::vector<uint32_t>& top) {
  std::vector<uint32_t> ids;
  for (uint32_t t : top) {
    for (const Layer& l : R.layers)
      if (isSublayer(l) && l.parentId == t) ids.push_back(l.id);
    ids.push_back(t);
  }
  uint32_t act = R.layers[size_t(active)].id;
  R.reorderLayers(ids);
  active = std::max(0, R.indexOf(act));
}

static std::vector<uint32_t> topOrder(const Renderer& R) {
  std::vector<uint32_t> top;
  for (const Layer& l : R.layers)
    if (!(l.parentId && R.indexOf(l.parentId) >= 0)) top.push_back(l.id);
  return top;
}

void App::moveLayerBlock(int index, int dir) {
  std::vector<uint32_t> top = topOrder(R);
  auto it = std::find(top.begin(), top.end(), topLevelId(index));
  if (it == top.end()) return;
  size_t i = size_t(it - top.begin());
  if (dir > 0 && i + 1 < top.size()) std::swap(top[i], top[i + 1]);
  else if (dir < 0 && i > 0) std::swap(top[i], top[i - 1]);
  else return;
  applyTopOrder(top);
}

void App::dropLayerBlock(int from, int onto) {
  if (from == onto) return;
  std::vector<uint32_t> top = topOrder(R);
  uint32_t a = topLevelId(from), b = topLevelId(onto);
  if (a == b) return;
  top.erase(std::find(top.begin(), top.end(), a));
  auto pos = std::find(top.begin(), top.end(), b);
  top.insert(from < onto ? pos + 1 : pos, a);  // dragged up: above the target, down: below it
  applyTopOrder(top);
}

void App::deleteLayerBlock(int index) {
  uint32_t id = R.layers[size_t(index)].id;
  std::vector<uint32_t> kids;
  for (const Layer& l : R.layers)
    if (l.parentId == id) kids.push_back(l.id);
  if (R.layers.size() <= 1 + kids.size()) return;
  for (uint32_t k : kids) R.deleteLayer(R.indexOf(k));
  R.deleteLayer(R.indexOf(id));
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
}
