#include "app.h"
#include "fileio.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>

namespace fs = std::filesystem;
static constexpr double kPi = 3.14159265358979323846;

static const char* kTipNames[3] = {"Hard Round", "Textured Pen", "Soft Round"};

static std::string findUserData() {
  const char* base = SDL_GetBasePath();
  fs::path p = base ? fs::path(base) : fs::current_path();
  for (fs::path cur = p; !cur.empty(); cur = cur.parent_path()) {
    std::error_code ec;
    if (fs::is_directory(cur / "User-Data", ec)) return (cur / "User-Data").string();
    if (cur == cur.parent_path()) break;
  }
  return p.string();
}

static std::string fmtBytes(double b) {
  char buf[64];
  if (b >= 1024.0 * 1024 * 1024) snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
  else snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
  return buf;
}

App::App(SDL_Window* w, Renderer& r, const Options& o) : window(w), R(r), opt(o) {
  density = SDL_GetWindowPixelDensity(window);
  if (density <= 0) density = 1;
  userData = findUserData();
  // per-tip defaults
  brushes[0] = BrushSettings{};
  brushes[0].tip = Tip::Hard;
  brushes[0].size = 12;
  brushes[1] = BrushSettings{};
  brushes[1].tip = Tip::Textured;
  brushes[1].size = 24;
  brushes[1].spacing = 0.05f;
  brushes[2] = BrushSettings{};
  brushes[2].tip = Tip::Soft;
  brushes[2].size = 150;
  brushes[2].flow = 0.15f;
  brushes[2].hardness = 0.0f;
  brushes[2].spacing = 0.1f;
  brushes[2].pressureSize = false;
  brushes[2].pressureOpacity = true;
}

// ---------------------------------------------------------------------------
// View

void App::screenToDoc(double sx, double sy, double& dx, double& dy) const {
  double cx = R.extent.width * 0.5, cy = R.extent.height * 0.5;
  double vx = (sx - cx) / view.zoom, vy = (sy - cy) / view.zoom;
  double c = std::cos(view.rotation), s = std::sin(view.rotation);
  dx = view.panX + c * vx + s * vy;
  dy = view.panY - s * vx + c * vy;
}

void App::zoomAt(double sx, double sy, double factor) {
  double bx, by, ax, ay;
  screenToDoc(sx, sy, bx, by);
  view.zoom = std::clamp(view.zoom * factor, 0.005, 64.0);
  screenToDoc(sx, sy, ax, ay);
  view.panX += bx - ax;
  view.panY += by - ay;
}

void App::fitView() {
  if (!R.hasDocument()) return;
  // fit into the free canvas area between the docked panels
  double cw = canvasW > 50 ? canvasW : R.extent.width, ch = canvasH > 50 ? canvasH : R.extent.height;
  double cx = canvasW > 50 ? canvasX + canvasW * 0.5 : R.extent.width * 0.5;
  double cy = canvasH > 50 ? canvasY + canvasH * 0.5 : R.extent.height * 0.5;
  view.zoom = std::min(std::max(1.0, cw) / R.docW, std::max(1.0, ch) / R.docH) * 0.94;
  view.rotation = 0;
  view.panX = R.docW * 0.5 - (cx - R.extent.width * 0.5) / view.zoom;
  view.panY = R.docH * 0.5 - (cy - R.extent.height * 0.5) / view.zoom;
}

// ---------------------------------------------------------------------------
// Strokes

void App::strokeBegin(const PenSample& s, bool eraser) {
  if (!R.hasDocument() || R.stroking()) return;
  const BrushSettings& b = brushes[tipIndex];
  StrokeStyle st;
  st.tip = b.tip;
  st.hardness = b.hardness;
  st.texStrength = b.texStrength;
  st.texScale = b.texScale;
  st.color[0] = color[0];
  st.color[1] = color[1];
  st.color[2] = color[2];
  st.opacity = b.opacity;
  st.eraser = eraser || eraserToggle;
  R.beginStroke(active, st);
  engine.begin(s, b);
}

void App::strokeAdd(const PenSample& s) {
  if (engine.active()) engine.add(s);
}

void App::strokeEnd() {
  if (!engine.active()) return;
  engine.end();
  flushDabs(0);
  R.endStroke();
}

void App::flushDabs(uint64_t inputNs) {
  if (engine.out.empty()) return;
  R.queueDabs(engine.out);
  engine.out.clear();
  if (inputNs && !pendingInputNs) pendingInputNs = inputNs;
}

// ---------------------------------------------------------------------------
// Input

void App::pointerDown(float x, float y, float pressure, bool eraser, bool pen, uint64_t tNs) {
  float sx = x * density, sy = y * density;
  lastX = sx;
  lastY = sy;
  if (ImGui::GetIO().WantCaptureMouse) return;
  if (spaceDown) {
    drag = shiftDown ? Drag::Rotate : Drag::Pan;
    dragX = sx;
    dragY = sy;
    dragStartAngle = std::atan2(sy - R.extent.height * 0.5, sx - R.extent.width * 0.5);
    dragStartRot = view.rotation;
    return;
  }
  double dx, dy;
  screenToDoc(sx, sy, dx, dy);
  strokeFromPen = pen;
  strokeBegin({dx, dy, pressure}, eraser);
  flushDabs(tNs);
}

void App::pointerMove(float x, float y, float pressure, bool pen, uint64_t tNs) {
  float sx = x * density, sy = y * density;
  lastX = sx;
  lastY = sy;
  if (drag == Drag::Pan) {
    double ddx = sx - dragX, ddy = sy - dragY;
    double c = std::cos(view.rotation), s = std::sin(view.rotation);
    view.panX -= (c * ddx + s * ddy) / view.zoom;
    view.panY -= (-s * ddx + c * ddy) / view.zoom;
    dragX = sx;
    dragY = sy;
    return;
  }
  if (drag == Drag::Rotate) {
    double a = std::atan2(sy - R.extent.height * 0.5, sx - R.extent.width * 0.5);
    view.rotation = dragStartRot + (a - dragStartAngle);
    return;
  }
  if (engine.active() && strokeFromPen == pen) {
    double dx, dy;
    screenToDoc(sx, sy, dx, dy);
    strokeAdd({dx, dy, pressure});
    flushDabs(tNs);
  }
}

void App::pointerUp(bool pen) {
  if (drag != Drag::None) { drag = Drag::None; return; }
  if (engine.active() && strokeFromPen == pen) strokeEnd();
}

void App::handleKey(const SDL_KeyboardEvent& k, bool down) {
  if (k.key == SDLK_SPACE) spaceDown = down;
  shiftDown = (k.mod & SDL_KMOD_SHIFT) != 0;
  ctrlDown = (k.mod & SDL_KMOD_CTRL) != 0;
  if (!down || ImGui::GetIO().WantTextInput) return;
  bool ctrl = ctrlDown, shift = shiftDown;
  switch (k.key) {
    case SDLK_1: if (ctrl) { view.zoom = 1; } else tipIndex = 0; break;
    case SDLK_2: if (!ctrl) tipIndex = 1; break;
    case SDLK_3: if (!ctrl) tipIndex = 2; break;
    case SDLK_0: if (ctrl) fitView(); break;
    case SDLK_E: if (!ctrl) eraserToggle = !eraserToggle; break;
    case SDLK_LEFTBRACKET: brushes[tipIndex].size = std::max(1.0f, brushes[tipIndex].size / 1.15f); break;
    case SDLK_RIGHTBRACKET: brushes[tipIndex].size = std::min(5000.0f, brushes[tipIndex].size * 1.15f); break;
    case SDLK_Z: if (ctrl) { if (shift) R.redo(); else R.undo(); } break;
    case SDLK_Y: if (ctrl) R.redo(); break;
    case SDLK_R: if (!ctrl) view.rotation = 0; break;
    case SDLK_N: if (ctrl) showNewDoc = true; break;
    case SDLK_O: if (ctrl && !saving) showDialog(DlgOpen); break;
    case SDLK_I: if (ctrl && !saving && R.hasDocument()) showDialog(DlgImport); break;
    case SDLK_S:
      if (ctrl && !saving && R.hasDocument()) {
        if (shift || documentPath.empty()) showDialog(DlgSave); else saveFile(documentPath);
      }
      break;
    default: break;
  }
}

void App::handleEvent(const SDL_Event& e) {
  ImGui_ImplSDL3_ProcessEvent(&e);
  framesToRender = std::max(framesToRender, 2);
  switch (e.type) {
    case SDL_EVENT_QUIT:
      running = false;
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
      R.onResize();
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      density = SDL_GetWindowPixelDensity(window);
      break;
    case SDL_EVENT_PEN_AXIS:
      if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        penPressure = e.paxis.value;
        if (penDownPending) {
          penDownPending = false;
          pointerDown(penDownX, penDownY, penPressure, penDownEraser, true, penDownNs);
        } else if (engine.active() && strokeFromPen) {
          pointerMove(e.paxis.x, e.paxis.y, penPressure, true, e.paxis.timestamp);
        }
      }
      break;
    case SDL_EVENT_PEN_DOWN:
      // Windows Ink may deliver PEN_DOWN before the first pressure value of the new touch;
      // start the stroke on the next pressure/motion sample so the first dab gets real pressure.
      penDownPending = true;
      penDownX = e.ptouch.x;
      penDownY = e.ptouch.y;
      penDownEraser = e.ptouch.eraser;
      penDownNs = e.ptouch.timestamp;
      break;
    case SDL_EVENT_PEN_MOTION:
      if (penDownPending) {
        penDownPending = false;
        pointerDown(penDownX, penDownY, penPressure, penDownEraser, true, penDownNs);
      }
      pointerMove(e.pmotion.x, e.pmotion.y, penPressure, true, e.pmotion.timestamp);
      break;
    case SDL_EVENT_PEN_UP:
      if (penDownPending) {  // a tap: draw a single dab
        penDownPending = false;
        pointerDown(penDownX, penDownY, penPressure, penDownEraser, true, penDownNs);
      }
      pointerUp(true);
      break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
      penDownPending = false;
      pointerUp(true);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.which == SDL_PEN_MOUSEID || e.button.which == SDL_TOUCH_MOUSEID) break;
      if (e.button.button == SDL_BUTTON_LEFT) {
        pointerDown(e.button.x, e.button.y, 1.0f, false, false, e.button.timestamp);
      } else if (e.button.button == SDL_BUTTON_MIDDLE && !ImGui::GetIO().WantCaptureMouse) {
        drag = Drag::Pan;
        dragX = e.button.x * density;
        dragY = e.button.y * density;
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (e.motion.which == SDL_PEN_MOUSEID || e.motion.which == SDL_TOUCH_MOUSEID) {
        lastX = e.motion.x * density;
        lastY = e.motion.y * density;
        break;
      }
      pointerMove(e.motion.x, e.motion.y, 1.0f, false, e.motion.timestamp);
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e.button.which == SDL_PEN_MOUSEID || e.button.which == SDL_TOUCH_MOUSEID) break;
      if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE) pointerUp(false);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (!ImGui::GetIO().WantCaptureMouse && e.wheel.y != 0)
        zoomAt(e.wheel.mouse_x * density, e.wheel.mouse_y * density, std::pow(1.2, e.wheel.y));
      break;
    case SDL_EVENT_DROP_FILE:
      if (e.drop.data) {
        // dropping onto an existing document imports as a layer; hold Ctrl to open instead
        if (R.hasDocument() && !ctrlDown) importAsLayer(e.drop.data);
        else openFile(e.drop.data);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      handleKey(e.key, e.type == SDL_EVENT_KEY_DOWN);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Actions

void App::error(const std::string& msg) {
  SDL_Log("%s", msg.c_str());
  errorMsg = msg;
  errorOpen = true;
}

bool App::newDocument(uint32_t w, uint32_t h, bool white) {
  std::string err;
  if (!R.newDocument(w, h, white, err)) {
    error("Could not create a " + std::to_string(w) + " x " + std::to_string(h) + " document: " + err);
    return false;
  }
  active = 0;
  documentPath.clear();
  fitView();
  return true;
}

void App::addLayer() {
  std::string err;
  int i = R.addLayer(active + 1, err);
  if (i < 0) error(err);
  else active = i;
}

// ---------------------------------------------------------------------------
// UI

void App::drawBrushPanel() {
  if (!ImGui::Begin("Brush", &showBrush)) { ImGui::End(); return; }
  for (int i = 0; i < 3; ++i) {
    if (i) ImGui::SameLine();
    if (ImGui::RadioButton(kTipNames[i], tipIndex == i)) tipIndex = i;
  }
  ImGui::Checkbox("Eraser (E)", &eraserToggle);
  BrushSettings& b = brushes[tipIndex];
  ImGui::SliderFloat("Size", &b.size, 1.0f, 5000.0f, "%.1f px", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Opacity", &b.opacity, 0.0f, 1.0f, "%.2f");
  ImGui::SliderFloat("Flow", &b.flow, 0.01f, 1.0f, "%.2f");
  float sp = b.spacing * 100;
  if (ImGui::SliderFloat("Spacing", &sp, 1.0f, 100.0f, "%.0f %%")) b.spacing = sp / 100;
  if (b.tip == Tip::Soft) ImGui::SliderFloat("Hardness", &b.hardness, 0.0f, 1.0f, "%.2f");
  if (b.tip == Tip::Textured) {
    ImGui::SliderFloat("Texture", &b.texStrength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Grain scale", &b.texScale, 1.0f, 64.0f, "%.1f px", ImGuiSliderFlags_Logarithmic);
  }
  ImGui::SeparatorText("Pressure");
  ImGui::Checkbox("Pressure -> size", &b.pressureSize);
  if (b.pressureSize) {
    float m = b.minSize * 100;
    if (ImGui::SliderFloat("Min size", &m, 0.0f, 100.0f, "%.0f %%")) b.minSize = m / 100;
  }
  ImGui::Checkbox("Pressure -> opacity", &b.pressureOpacity);
  ImGui::SliderFloat("Curve (gamma)", &b.gamma, 0.2f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
  ImGui::End();
}

void App::drawColorPanel() {
  if (!ImGui::Begin("Colour", &showColor)) { ImGui::End(); return; }
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::SetNextItemWidth(std::min(w, std::max(120.0f, ImGui::GetContentRegionAvail().y - 60)));
  ImGui::ColorPicker3("##colour", color,
                      ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_NoSidePreview);
  ImGui::End();
}

void App::buildDefaultLayout(unsigned int dockId) {
  ImGui::DockBuilderRemoveNode(dockId);
  ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
  ImGuiID centre = dockId, left, right;
  left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.19f, nullptr, &centre);
  right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.23f, nullptr, &centre);
  ImGuiID leftBottom, rightBottom;
  leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
  rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42f, nullptr, &right);
  ImGui::DockBuilderDockWindow("Brush", left);
  ImGui::DockBuilderDockWindow("Colour", leftBottom);
  ImGui::DockBuilderDockWindow("Layers", right);
  ImGui::DockBuilderDockWindow("Performance", rightBottom);
  ImGui::DockBuilderFinish(dockId);
}

void App::drawLayerPanel() {
  if (!ImGui::Begin("Layers", &showLayers)) { ImGui::End(); return; }
  if (!R.hasDocument()) { ImGui::TextUnformatted("No document"); ImGui::End(); return; }
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
  ImGui::Text("Layers: %d / %d  (%s each)", int(R.layers.size()), R.limit.maxLayers, fmtBytes(double(R.limit.layerBytes)).c_str());
  bool busy = R.stroking();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("New")) addLayer();
  ImGui::SameLine();
  if (ImGui::Button("Duplicate")) {
    std::string err;
    int i = R.duplicateLayer(active, err);
    if (i < 0) error(err); else active = i;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(R.layers.size() <= 1);
  if (ImGui::Button("Delete")) { R.deleteLayer(active); active = std::min(active, int(R.layers.size()) - 1); }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::ArrowButton("##up", ImGuiDir_Up) && active + 1 < int(R.layers.size())) { R.moveLayer(active, 1); ++active; }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##down", ImGuiDir_Down) && active > 0) { R.moveLayer(active, -1); --active; }

  Layer& L = R.layers[active];
  if (renameFor != int(L.id)) {
    snprintf(renameBuf, sizeof renameBuf, "%s", L.name.c_str());
    renameFor = int(L.id);
  }
  if (ImGui::InputText("Name", renameBuf, sizeof renameBuf)) L.name = renameBuf;
  int mode = int(L.mode);
  if (ImGui::BeginCombo("Blend", blendModeName(L.mode), ImGuiComboFlags_HeightLarge)) {
    for (int m = 0; m < int(BlendMode::Count); ++m) {
      if (ImGui::Selectable(blendModeName(BlendMode(m)), m == mode)) { L.mode = BlendMode(m); R.markCachesDirty(); }
      if (m == 0 || m == 5 || m == 11 || m == 20 || m == 23) ImGui::Separator();
    }
    ImGui::EndCombo();
  }
  float op = L.opacity * 100;
  if (ImGui::SliderFloat("Opacity##layer", &op, 0.0f, 100.0f, "%.0f %%")) { L.opacity = op / 100; R.markCachesDirty(); }
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::BeginChild("list");
  for (int i = int(R.layers.size()) - 1; i >= 0; --i) {
    Layer& l = R.layers[i];
    ImGui::PushID(int(l.id));
    if (ImGui::Checkbox("##vis", &l.visible)) R.markCachesDirty();
    ImGui::SameLine();
    char label[200];
    snprintf(label, sizeof label, "%s  (%s, %.0f%%)", l.name.c_str(), blendModeName(l.mode), l.opacity * 100);
    if (ImGui::Selectable(label, i == active) && !busy) active = i;
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::End();
}

void App::drawPerfPanel() {
  if (!ImGui::Begin("Performance", &showPerf)) { ImGui::End(); return; }
  ImGui::Text("%s", R.deviceName.c_str());
  ImGui::TextWrapped("%s", R.driverInfo.c_str());
  if (R.cpuEmulation) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "CPU emulation (numbers are not representative)");
  ImGui::Separator();
  ImGui::Text("FPS %.1f   CPU frame %.2f ms", ImGui::GetIO().Framerate, cpuFrameMs);
  const GpuTimings& t = R.lastTimings;
  if (R.timestampsSupported)
    ImGui::Text("GPU ms: dabs %.2f  commit %.2f\n        caches %.2f  composite %.2f\n        total %.2f",
                t.dabs, t.commit, t.caches, t.composite, t.total);
  else
    ImGui::TextUnformatted("GPU timestamps not supported");
  ImGui::Text("Input -> present: last %.2f ms, avg %.2f ms", lastLatencyMs, avgLatencyMs);
  ImGui::Text("Dabs this frame: %u   total %llu", R.lastFrameDabs, (unsigned long long)R.totalDabs);
  ImGui::Text("Layers VRAM: %s / budget %s", fmtBytes(double(R.layerBytesTotal())).c_str(),
              fmtBytes(double(R.memoryBudgetBytes)).c_str());
  ImGui::Text("Undo RAM: %s / %s (%zu steps)", fmtBytes(double(R.undoBytes())).c_str(),
              fmtBytes(double(R.undoBudgetBytes)).c_str(), R.undoSteps());
  ImGui::Text("Stroke mask: %s", R.maskR16 ? "R16_UNORM" : "R32_SFLOAT");
  const char* names[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
  const char* cur = int(R.presentMode) < 4 ? names[R.presentMode] : "?";
  if (ImGui::BeginCombo("Present mode", cur)) {
    for (VkPresentModeKHR m : R.presentModes)
      if (int(m) < 3 && ImGui::Selectable(names[m], m == R.presentMode)) R.setPresentMode(m);
    ImGui::EndCombo();
  }
  ImGui::SeparatorText("Benchmark");
  ImGui::InputInt("Doc width", &benchUiW, 1000);
  ImGui::InputInt("Doc height", &benchUiH, 1000);
  ImGui::InputInt("Brush px", &benchUiBrush, 100);
  ImGui::Combo("Tip", &benchUiTip, kTipNames, 3);
  ImGui::InputInt("Layers", &benchUiLayers);
  ImGui::BeginDisabled(bench.running || R.stroking());
  if (ImGui::Button("Run benchmark"))
    startBenchmark(uint32_t(std::max(1, benchUiW)), uint32_t(std::max(1, benchUiH)), float(std::clamp(benchUiBrush, 1, 5000)),
                   Tip(benchUiTip), std::max(1, benchUiLayers));
  ImGui::EndDisabled();
  if (!bench.report.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Copy report")) ImGui::SetClipboardText(bench.report.c_str());
    ImGui::TextWrapped("%s", bench.report.c_str());
  }
  ImGui::End();
}

void App::drawNewDocDialog() {
  if (showNewDoc) { ImGui::OpenPopup("New Document"); showNewDoc = false; }
  if (!ImGui::BeginPopupModal("New Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::InputInt("Width (px)", &newW, 100);
  ImGui::InputInt("Height (px)", &newH, 100);
  newW = std::clamp(newW, 1, 300000);
  newH = std::clamp(newH, 1, 300000);
  ImGui::Checkbox("White paper (off = transparent)", &newWhite);
  if (newLimitW != newW || newLimitH != newH) {
    newLimit = R.computeLayerLimit(uint32_t(newW), uint32_t(newH));
    newLimitW = newW;
    newLimitH = newH;
  }
  if (newLimit.ok)
    ImGui::Text("Max layers: %d  (%s per layer, %s usable GPU memory)", newLimit.maxLayers,
                fmtBytes(double(newLimit.layerBytes)).c_str(), fmtBytes(double(newLimit.usableBytes)).c_str());
  else
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", newLimit.reason.c_str());
  ImGui::BeginDisabled(!newLimit.ok);
  if (ImGui::Button("Create", ImVec2(120, 0))) {
    newDocument(uint32_t(newW), uint32_t(newH), newWhite);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::drawUI() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("New...", "Ctrl+N")) showNewDoc = true;
      drawFileDialogs();
      ImGui::Separator();
      if (ImGui::MenuItem("Quit")) running = false;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Undo", "Ctrl+Z", false, R.canUndo() && !R.stroking())) R.undo();
      if (ImGui::MenuItem("Redo", "Ctrl+Y", false, R.canRedo() && !R.stroking())) R.redo();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Fit to window", "Ctrl+0")) fitView();
      if (ImGui::MenuItem("100 %", "Ctrl+1")) view.zoom = 1;
      if (ImGui::MenuItem("Reset rotation", "R")) view.rotation = 0;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("Brush", nullptr, &showBrush);
      ImGui::MenuItem("Colour", nullptr, &showColor);
      ImGui::MenuItem("Layers", nullptr, &showLayers);
      ImGui::MenuItem("Performance", nullptr, &showPerf);
      ImGui::Separator();
      if (ImGui::MenuItem("Reset layout")) { resetLayout = true; showBrush = showColor = showLayers = showPerf = true; }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (saving) { ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Saving..."); ImGui::Separator(); }
    ImGui::Text("%u x %u   zoom %.1f %%   rot %.0f deg   %s%s", R.docW, R.docH, view.zoom * 100,
                std::fmod(view.rotation * 180 / kPi + 360 * 100, 360.0), kTipNames[tipIndex], eraserToggle ? " (eraser)" : "");
    ImGui::EndMainMenuBar();
  }
  // Dockspace: panels snap to the edges, resize, and combine into tab stacks; the central
  // node is see-through and is the canvas.
  ImGuiID dockId = ImGui::GetID("MainDock");
  if (resetLayout || !ImGui::DockBuilderGetNode(dockId)) { buildDefaultLayout(dockId); resetLayout = false; }
  ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
  if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(dockId)) {
    float s = ImGui::GetIO().DisplayFramebufferScale.x;
    canvasX = c->Pos.x * s; canvasY = c->Pos.y * s; canvasW = c->Size.x * s; canvasH = c->Size.y * s;
  }
  if (showBrush) drawBrushPanel();
  if (showColor) drawColorPanel();
  if (showLayers) drawLayerPanel();
  if (showPerf) drawPerfPanel();
  drawNewDocDialog();
  if (!R.lastError.empty()) { error(R.lastError); R.lastError.clear(); }
  if (errorOpen) { ImGui::OpenPopup("Message"); errorOpen = false; }
  if (ImGui::BeginPopupModal("Message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(500);
    ImGui::TextUnformatted(errorMsg.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  // brush outline at the cursor
  if (!ImGui::GetIO().WantCaptureMouse && R.hasDocument() && drag == Drag::None) {
    float s = ImGui::GetIO().DisplayFramebufferScale.x;
    float r = float(brushes[tipIndex].size * 0.5 * view.zoom) / s;
    ImVec2 c(lastX / s, lastY / s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddCircle(c, std::max(r, 1.5f) + 1, IM_COL32(255, 255, 255, 160), 0, 1.0f);
    dl->AddCircle(c, std::max(r, 1.5f), IM_COL32(0, 0, 0, 200), 0, 1.0f);
  }
}

// ---------------------------------------------------------------------------
// Benchmark

void App::startBenchmark(uint32_t w, uint32_t h, float brush, Tip tip, int layers) {
  if (!newDocument(w, h, true)) return;
  for (int i = 1; i < layers; ++i) {
    std::string err;
    int li = R.addLayer(int(R.layers.size()), err);
    if (li < 0) { error("Benchmark: " + err); break; }
  }
  active = int(R.layers.size()) / 2;  // layers above and below the active one exercise both caches
  tipIndex = int(tip);
  brushes[tipIndex].size = brush;
  eraserToggle = false;
  bench = Bench{};
  bench.running = true;
  bench.docW = w;
  bench.docH = h;
  bench.brush = brush;
  bench.tip = tip;
  bench.layers = int(R.layers.size());
  // zig-zag across the canvas: 240 Hz simulated pen for 4 s
  const int n = 960;
  double m = std::min<double>(0.05 * std::min(w, h) + brush * 0.5, std::min(w, h) * 0.45);
  const int legs = 8;
  std::vector<std::pair<double, double>> pts;
  for (int k = 0; k <= legs; ++k) pts.push_back({m + (w - 2 * m) * k / legs, (k % 2) ? h - m : m});
  std::vector<double> cum{0};
  for (int k = 1; k <= legs; ++k)
    cum.push_back(cum.back() + std::hypot(pts[k].first - pts[k - 1].first, pts[k].second - pts[k - 1].second));
  for (int i = 0; i < n; ++i) {
    double d = cum.back() * i / (n - 1);
    int k = 1;
    while (k < legs && cum[k] < d) ++k;
    double f = (d - cum[k - 1]) / std::max(1e-9, cum[k] - cum[k - 1]);
    double x = pts[k - 1].first + (pts[k].first - pts[k - 1].first) * f;
    double y = pts[k - 1].second + (pts[k].second - pts[k - 1].second) * f;
    float p = float(0.6 + 0.4 * std::sin(i * 0.05));
    bench.samples.push_back({x, y, p});
    bench.times.push_back(i / 240.0);
  }
  bench.t0 = SDL_GetTicksNS();
  bench.dabs0 = R.totalDabs;
  bench.mp0 = R.totalDabMegapixels;
  SDL_Log("benchmark: %ux%u, brush %.0f px, %s, %d layers", w, h, brush, kTipNames[int(tip)], bench.layers);
}

void App::tickBenchmark() {
  if (!bench.running) return;
  double el = (SDL_GetTicksNS() - bench.t0) * 1e-9;
  uint64_t now = SDL_GetTicksNS();
  while (bench.next < bench.samples.size() && bench.times[bench.next] <= el) {
    if (bench.next == 0) strokeBegin(bench.samples[0], false);
    else strokeAdd(bench.samples[bench.next]);
    ++bench.next;
  }
  flushDabs(now);
  if (bench.next == bench.samples.size() && !bench.ended) {
    strokeEnd();
    bench.ended = true;
  }
  if (bench.ended && !R.busy()) finishBenchmark();
}

static void stats(std::vector<double> v, double& avg, double& p95, double& mx) {
  avg = p95 = mx = 0;
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  avg = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
  p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
  mx = v.back();
}

void App::finishBenchmark() {
  bench.running = false;
  double fa, fp, fm, ga, gp, gm, la, lp, lm;
  stats(bench.frameMs, fa, fp, fm);
  stats(bench.gpuMs, ga, gp, gm);
  stats(bench.latencyMs, la, lp, lm);
  double dabMs = std::accumulate(bench.gpuDabMs.begin(), bench.gpuDabMs.end(), 0.0);
  uint64_t dabs = R.totalDabs - bench.dabs0;
  double mp = R.totalDabMegapixels - bench.mp0;
  char buf[2048];
  std::time_t tt = std::time(nullptr);
  char when[64];
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
  snprintf(buf, sizeof buf,
           "Saraswati %s benchmark  (%s)\n"
           "Device: %s%s\nDriver: %s\n"
           "Document: %u x %u, %d layers, stroke mask %s\n"
           "Brush: %s, %.0f px, spacing %.0f %%\n"
           "Stroke: %zu samples at 240 Hz (%.1f s)\n"
           "Frames: %zu   frame ms avg %.2f / p95 %.2f / max %.2f\n"
           "GPU ms per frame: avg %.2f / p95 %.2f / max %.2f%s\n"
           "Input -> present call ms: avg %.2f / p95 %.2f / max %.2f\n"
           "Dabs: %llu, %.0f dab-megapixels, %.0f dab-MP/s of GPU dab time\n",
           SARASWATI_VERSION, when, R.deviceName.c_str(), R.cpuEmulation ? " (CPU emulation)" : "", R.driverInfo.c_str(),
           bench.docW, bench.docH, bench.layers, R.maskR16 ? "R16" : "R32F", kTipNames[int(bench.tip)], bench.brush,
           brushes[int(bench.tip)].spacing * 100, bench.samples.size(), bench.times.empty() ? 0 : bench.times.back(),
           bench.frameMs.size(), fa, fp, fm, ga, gp, gm, R.timestampsSupported ? "" : " (no timestamps)", la, lp, lm,
           (unsigned long long)dabs, mp, dabMs > 0 ? mp / (dabMs * 1e-3) : 0.0);
  bench.report = buf;
  printf("%s", buf);
  fflush(stdout);
  char name[64];
  std::strftime(name, sizeof name, "benchmark-%Y%m%d-%H%M%S.txt", std::localtime(&tt));
  fs::path logDir = fs::path(userData) / "logs";
  std::error_code ec;
  fs::create_directories(logDir, ec);
  std::ofstream f(logDir / name);
  if (f) {
    f << buf;
    SDL_Log("benchmark report written to %s", (logDir / name).string().c_str());
  }
}

// ---------------------------------------------------------------------------
// Demo script (automated visual test: exercises tips, layers, blend modes, undo, view)

void App::buildDemo() {
  auto stroke = [this](int tip, float size, float r, float g, float b, bool eraser,
                       std::function<PenSample(double)> path, int n = 120) {
    return [=, this] {
      tipIndex = tip;
      brushes[tip].size = size;
      color[0] = r; color[1] = g; color[2] = b;
      strokeBegin(path(0), eraser);
      for (int i = 1; i <= n; ++i) strokeAdd(path(double(i) / n));
      strokeEnd();
    };
  };
  demo.push_back([this] { newDocument(1600, 1000, true); });
  // Layer 1: red hard-round sine wave with pressure
  demo.push_back(stroke(0, 40, 0.85f, 0.1f, 0.1f, false, [](double t) {
    return PenSample{100 + 1400 * t, 250 + 120 * std::sin(t * 12), float(0.2 + 0.8 * std::sin(t * kPi))};
  }));
  // Layer 2: blue textured pen, green soft round, then an eraser cut
  demo.push_back([this] { addLayer(); });
  demo.push_back(stroke(1, 140, 0.1f, 0.3f, 0.9f, false, [](double t) {
    return PenSample{150 + 1300 * t, 150 + 700 * t, float(0.3 + 0.7 * t)};
  }));
  demo.push_back(stroke(2, 260, 0.1f, 0.7f, 0.2f, false, [](double t) {
    return PenSample{200 + 1200 * t, 700, 1.0f};
  }));
  demo.push_back(stroke(0, 50, 0, 0, 0, true, [](double t) { return PenSample{800, 80 + 840 * t, 1.0f}; }));
  // undo test: this black stroke must NOT appear
  demo.push_back(stroke(0, 30, 0, 0, 0, false, [](double t) { return PenSample{100 + 1400 * t, 900, 1.0f}; }));
  demo.push_back([this] { R.undo(); });
  // redo test: this purple stroke MUST appear
  demo.push_back(stroke(0, 30, 0.6f, 0.1f, 0.7f, false, [](double t) { return PenSample{100 + 1400 * t, 950, 1.0f}; }));
  demo.push_back([this] { R.undo(); });
  demo.push_back([this] { R.redo(); });
  // Layer 3 (Multiply, above the active layer): yellow band darkens what is below it
  demo.push_back([this] { addLayer(); R.layers[active].mode = BlendMode::Multiply; R.markCachesDirty(); });
  demo.push_back(stroke(0, 200, 1.0f, 0.85f, 0.1f, false, [](double t) { return PenSample{1100, 60 + 880 * t, 1.0f}; }));
  // Layer 4 (Screen at 60 %): white-ish disc lightens
  demo.push_back([this] { addLayer(); R.layers[active].mode = BlendMode::Screen; R.layers[active].opacity = 0.6f; R.markCachesDirty(); });
  demo.push_back(stroke(0, 300, 0.2f, 0.8f, 0.9f, false, [](double t) { return PenSample{350, 500, 1.0f}; }, 1));
  // Layer 5: hidden, must NOT appear
  demo.push_back([this] { addLayer(); R.layers[active].visible = false; R.markCachesDirty(); });
  demo.push_back(stroke(0, 400, 0, 0, 0, false, [](double t) { return PenSample{800, 500, 1.0f}; }, 1));
  // make Layer 2 active so non-Normal layers sit above it (per-frame blend path), rotate and zoom
  demo.push_back([this] {
    active = 1;
    view.rotation = 0.12;
    zoomAt(R.extent.width * 0.5, R.extent.height * 0.5, 1.1);
  });
  // live (uncommitted) stroke on the active layer: orange, shown via the stroke mask
  demo.push_back([this] {
    tipIndex = 2;
    brushes[2].size = 120;
    color[0] = 1.0f; color[1] = 0.5f; color[2] = 0.0f;
    strokeBegin({300, 850, 1.0f}, false);
    for (int i = 1; i <= 60; ++i) strokeAdd({300 + 1000.0 * i / 60, 850 - 100 * std::sin(i / 60.0 * kPi), 1.0f});
    flushDabs(0);
  });
}

void App::tickDemo() {
  if (demoDone || demo.empty()) return;
  // wait for the GPU to finish the previous step (the last step leaves a stroke open on purpose)
  if (demoStep > 0 && R.busy() && !(demoStep == demo.size())) return;
  if (demoStep < demo.size()) {
    demo[demoStep++]();
    framesToRender = std::max(framesToRender, 3);
  } else {
    demoDone = true;
  }
}

// ---------------------------------------------------------------------------
// Files

static const SDL_DialogFileFilter kOpenFilters[] = {
    {"Documents and images", "psd;psb;png;jpg;jpeg;gif;webp;svg;bmp;tga"},
    {"Photoshop (PSD, PSB)", "psd;psb"},
    {"Images", "png;jpg;jpeg;gif;webp;svg;bmp;tga"}};
static const SDL_DialogFileFilter kImageFilters[] = {
    {"Images", "png;jpg;jpeg;gif;webp;svg;bmp;tga;psd;psb"}};
static const SDL_DialogFileFilter kSaveFilters[] = {{"Photoshop document", "psd;psb"}};

App::~App() {
  if (saveThread.joinable()) saveThread.join();
}

void App::showDialog(int kind) {
  struct Ctx { App* app; int kind; };
  auto* ctx = new Ctx{this, kind};
  auto cb = [](void* ud, const char* const* list, int) {
    Ctx* c = static_cast<Ctx*>(ud);
    if (list && list[0]) {
      std::lock_guard<std::mutex> lock(c->app->dialogMutex);
      c->app->dialogResults.push_back({c->kind, list[0]});
    } else if (!list) {
      std::lock_guard<std::mutex> lock(c->app->dialogMutex);
      c->app->dialogResults.push_back({-1, SDL_GetError()});
    }
    delete c;
  };
  if (kind == DlgSave) {
    std::string def = documentPath.empty() ? (fs::path(userData) / "documents" / "Untitled.psd").string() : documentPath;
    SDL_ShowSaveFileDialog(cb, ctx, window, kSaveFilters, 1, def.c_str());
  } else if (kind == DlgOpen) {
    std::string def = (fs::path(userData) / "documents").string();
    SDL_ShowOpenFileDialog(cb, ctx, window, kOpenFilters, 3, def.c_str(), false);
  } else {
    SDL_ShowOpenFileDialog(cb, ctx, window, kImageFilters, 1, nullptr, false);
  }
}

void App::processDialogResults() {
  std::vector<std::pair<int, std::string>> res;
  {
    std::lock_guard<std::mutex> lock(dialogMutex);
    res.swap(dialogResults);
  }
  for (auto& [kind, path] : res) {
    if (kind == DlgOpen) openFile(path);
    else if (kind == DlgImport) importAsLayer(path);
    else if (kind == DlgSave) {
      std::string p = path;
      if (!isPsdPath(p)) p += ".psd";
      saveFile(p);
    } else error("File dialog failed: " + path);
  }
  std::lock_guard<std::mutex> lock(saveMutex);
  if (saveDone) {
    saveDone = false;
    if (saveThread.joinable()) saveThread.join();
    if (!saveMessage.empty()) error(saveMessage);
  }
}

void App::drawFileDialogs() {
  bool busy = R.stroking() || saving;
  if (ImGui::MenuItem("Open...", "Ctrl+O", false, !busy)) showDialog(DlgOpen);
  if (ImGui::MenuItem("Import image as layer...", "Ctrl+I", false, !busy && R.hasDocument())) showDialog(DlgImport);
  ImGui::Separator();
  if (ImGui::MenuItem("Save", "Ctrl+S", false, !busy && R.hasDocument())) {
    if (documentPath.empty()) showDialog(DlgSave); else saveFile(documentPath);
  }
  if (ImGui::MenuItem("Save as PSD...", "Ctrl+Shift+S", false, !busy && R.hasDocument())) showDialog(DlgSave);
}

// Uploads straight-alpha pixels into layer `index` at (x, y).
static bool uploadStraight(Renderer& R, int index, int x, int y, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba,
                           std::string& err) {
  premultiply(rgba);
  return R.uploadLayerPixels(index, x, y, w, h, rgba.data(), err);
}

void App::openFile(const std::string& path) {
  if (R.stroking() || saving) return;
  std::string err;
  std::string name = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).filename().string();
  if (isPsdPath(path)) {
    DocFile doc;
    if (!loadPsd(path, doc, err)) { error("Could not open " + name + ":\n" + err); return; }
    if (!newDocument(doc.w, doc.h, false)) return;
    int maxL = R.limit.maxLayers;
    if (int(doc.layers.size()) > maxL) {
      doc.warnings.push_back("Only the bottom " + std::to_string(maxL) + " of " + std::to_string(doc.layers.size()) +
                             " layers fit in GPU memory at this size; the rest were not loaded.");
      doc.layers.resize(size_t(maxL));
    }
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      DocLayer& L = doc.layers[i];
      int idx = 0;
      if (i > 0) {
        idx = R.addLayer(int(i), err);
        if (idx < 0) { doc.warnings.push_back(err); break; }
      }
      if (!uploadStraight(R, idx, L.x, L.y, L.w, L.h, L.rgba, err)) { doc.warnings.push_back(err); }
      L.rgba = {};
      Layer& dst = R.layers[size_t(idx)];
      dst.name = L.name.empty() ? dst.name : L.name;
      dst.visible = L.visible;
      dst.opacity = L.opacity;
      dst.mode = L.mode;
    }
    active = int(R.layers.size()) - 1;
    documentPath = path;
    R.markCachesDirty();
    if (!doc.warnings.empty()) {
      std::string m = "Opened " + name + " with notes:";
      for (auto& w : doc.warnings) m += "\n- " + w;
      error(m);
    }
  } else {
    ImageRGBA img;
    if (!loadImageFile(path, img, err)) { error("Could not open " + name + ":\n" + err); return; }
    if (!newDocument(img.w, img.h, true)) return;
    if (!uploadStraight(R, 0, 0, 0, img.w, img.h, img.rgba, err)) error(err);
    R.layers[0].name = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).stem().string();
    documentPath.clear();  // images are imported; saving asks for a .psd name
  }
  SDL_SetWindowTitle(window, ("Saraswati " SARASWATI_VERSION " - " + name).c_str());
}

void App::importAsLayer(const std::string& path) {
  if (!R.hasDocument() || R.stroking() || saving) return;
  std::string err;
  std::string name = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).stem().string();
  std::vector<DocLayer> layers;
  if (isPsdPath(path)) {
    DocFile doc;
    if (!loadPsd(path, doc, err)) { error("Could not import " + name + ":\n" + err); return; }
    layers = std::move(doc.layers);
  } else {
    ImageRGBA img;
    if (!loadImageFile(path, img, err)) { error("Could not import " + name + ":\n" + err); return; }
    DocLayer L;
    L.name = name;
    L.w = img.w;
    L.h = img.h;
    L.x = (int32_t(R.docW) - int32_t(img.w)) / 2;  // centred
    L.y = (int32_t(R.docH) - int32_t(img.h)) / 2;
    L.rgba = std::move(img.rgba);
    layers.push_back(std::move(L));
  }
  for (auto& L : layers) {
    int idx = R.addLayer(active + 1, err);
    if (idx < 0) { error(err); return; }
    active = idx;
    if (!uploadStraight(R, idx, L.x, L.y, L.w, L.h, L.rgba, err)) error(err);
    Layer& dst = R.layers[size_t(idx)];
    if (!L.name.empty()) dst.name = L.name;
    dst.visible = L.visible;
    dst.opacity = L.opacity;
    dst.mode = L.mode;
  }
  R.markCachesDirty();
}

void App::saveFile(const std::string& path) {
  if (!R.hasDocument() || R.stroking() || saving) return;
  // GPU readback happens here (fast on a real GPU); encoding and disk I/O run on a worker thread.
  R.waitIdle();
  auto doc = std::make_shared<DocFile>();
  auto merged = std::make_shared<ImageRGBA>();
  doc->w = R.docW;
  doc->h = R.docH;
  std::string err;
  for (size_t i = 0; i < R.layers.size(); ++i) {
    DocLayer L;
    const Layer& src = R.layers[i];
    L.name = src.name;
    L.visible = src.visible;
    L.opacity = src.opacity;
    L.mode = src.mode;
    L.w = R.docW;
    L.h = R.docH;
    if (!R.readLayerPixels(int(i), L.rgba, err)) { error("Save failed: " + err); return; }
    doc->layers.push_back(std::move(L));
  }
  merged->w = R.docW;
  merged->h = R.docH;
  if (!R.readMergedPixels(merged->rgba, err)) { error("Save failed: " + err); return; }
  if (saveThread.joinable()) saveThread.join();
  saving = true;
  saveProgress = 0;
  documentPath = path;
  saveThread = std::thread([this, doc, merged, path] {
    for (auto& L : doc->layers) unpremultiply(L.rgba);
    unpremultiply(merged->rgba);
    std::string e;
    float prog = 0;
    bool ok = savePsd(path, *doc, *merged, e, &prog);
    std::lock_guard<std::mutex> lock(saveMutex);
    saveMessage = ok ? std::string() : "Save failed: " + e;
    if (ok) SDL_Log("saved %s", path.c_str());
    saveDone = true;
    saving = false;
  });
  std::string name = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).filename().string();
  SDL_SetWindowTitle(window, ("Saraswati " SARASWATI_VERSION " - " + name).c_str());
}

// ---------------------------------------------------------------------------
// Main loop

int App::run() {
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  bool testRun = opt.demo || opt.benchmark || !opt.screenshot.empty() || !opt.save.empty();
  if (testRun) {
    io.IniFilename = nullptr;  // tests always use the default layout
  } else {
    std::error_code ec;
    fs::create_directories(fs::path(userData) / "settings", ec);
    iniPath = (fs::path(userData) / "settings" / "layout.ini").string();
    io.IniFilename = iniPath.c_str();
  }
  R.onResize();
  for (int i = 0; i < 2; ++i) {
    // warm-up frames: swapchain extent and the docked canvas area are needed by fitView
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawUI();
    ImGui::Render();
    Renderer::FrameParams p;
    p.imgui = ImGui::GetDrawData();
    R.renderFrame(p);
  }
  uint32_t w = opt.docW ? opt.docW : 3000, h = opt.docH ? opt.docH : 2000;
  if (opt.demo) buildDemo();
  else if (opt.benchmark)
    startBenchmark(opt.docW ? opt.docW : 8000, opt.docH ? opt.docH : 8000, opt.brushPx > 0 ? opt.brushPx : 1000.0f,
                   opt.tipSet ? opt.tip : Tip::Hard, std::max(1, opt.layers));
  else {
    if (!newDocument(w, h, true)) newDocument(1000, 1000, true);
    if (opt.brushPx > 0) brushes[tipIndex].size = opt.brushPx;
    if (!opt.open.empty()) openFile(opt.open);
    for (auto& f : opt.imports) importAsLayer(f);
  }

  bool wantShot = !opt.screenshot.empty();
  while (running) {
    bool animating = bench.running || saving || (!opt.save.empty() && !savedForTest) || (!demo.empty() && !demoDone) || framesToRender > 0 || R.busy() ||
                     (wantShot && !screenshotTaken);
    SDL_Event e;
    if (!animating) {
      if (!SDL_WaitEventTimeout(&e, 250)) continue;
      handleEvent(e);
    }
    while (SDL_PollEvent(&e)) handleEvent(e);
    if (!running) break;
    processDialogResults();
    tickBenchmark();
    tickDemo();
    if (!opt.save.empty() && !savedForTest && (!opt.demo || demoDone) && engine.active() && (!wantShot || screenshotTaken))
      strokeEnd();  // the demo leaves its last stroke open; finish it before saving
    if (!opt.save.empty() && !savedForTest && (!opt.demo || demoDone) && !R.busy() && !bench.running &&
        (!wantShot || screenshotTaken)) {
      savedForTest = true;
      saveFile(opt.save);
    }

    uint64_t t = SDL_GetTicksNS();
    if (lastFrameNs) cpuFrameMs = (t - lastFrameNs) * 1e-6;
    lastFrameNs = t;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawUI();
    ImGui::Render();

    Renderer::FrameParams p;
    p.view = view;
    p.activeLayer = active;
    p.imgui = ImGui::GetDrawData();
    bool shotNow = false;
    if (wantShot && !screenshotTaken) {
      bool ready = opt.demo ? (demoDone && framesRendered > 2)
                 : opt.benchmark ? (!bench.running && !bench.report.empty())
                 : framesRendered >= opt.frames;
      if (ready) { p.screenshotPath = opt.screenshot; shotNow = true; }
    }
    uint64_t inputNs = pendingInputNs;
    bool rendered = R.renderFrame(p);
    if (rendered) {
      ++framesRendered;
      if (inputNs) {
        lastLatencyMs = (SDL_GetTicksNS() - inputNs) * 1e-6;
        avgLatencyMs = avgLatencyMs == 0 ? lastLatencyMs : avgLatencyMs * 0.95 + lastLatencyMs * 0.05;
        pendingInputNs = 0;
        if (bench.running) bench.latencyMs.push_back(lastLatencyMs);
      }
      if (bench.running) {
        if (framesRendered > 1) bench.frameMs.push_back(cpuFrameMs);
        if (R.lastTimings.total >= 0) {
          bench.gpuMs.push_back(R.lastTimings.total);
          bench.gpuDabMs.push_back(R.lastTimings.dabs);
        }
      }
      if (shotNow) {
        screenshotTaken = true;
        if (opt.exitAfter && opt.save.empty()) running = false;
      }
    }
    if (framesToRender > 0) --framesToRender;
    if (opt.exitAfter && (!wantShot || screenshotTaken) && !saving && (opt.save.empty() || savedForTest)) {
      if ((opt.benchmark && !bench.running && !bench.report.empty()) || (opt.demo && demoDone) || !opt.save.empty())
        running = false;
    }
  }
  R.waitIdle();
  return 0;
}
