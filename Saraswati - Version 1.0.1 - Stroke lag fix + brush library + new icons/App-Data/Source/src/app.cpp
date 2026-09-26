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

static const char* kTipNames[3] = {"Hard round", "Textured pen", "Soft round"};

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

std::string fmtBytes(double b) {
  char buf[64];
  if (b >= 1024.0 * 1024 * 1024) snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
  else snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
  return buf;
}

App::App(SDL_Window* w, Renderer& r, const Options& o) : window(w), R(r), opt(o) {
  resetShortcuts();
  density = SDL_GetWindowPixelDensity(window);
  if (density <= 0) density = 1;
  userData = findUserData();
  // per-tip defaults
  brushes = builtInBrushes();
  // borderless window: drag by the empty part of the menu bar, resize at the edges
  SDL_SetWindowHitTest(window, [](SDL_Window* win, const SDL_Point* pt, void* data) -> SDL_HitTestResult {
    App* a = static_cast<App*>(data);
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    bool maximized = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
    const int e = 6;
    if (!maximized) {
      bool l = pt->x < e, r = pt->x >= w - e, t = pt->y < e, b = pt->y >= h - e;
      if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
      if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
      if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
      if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
      if (t) return SDL_HITTEST_RESIZE_TOP;
      if (b) return SDL_HITTEST_RESIZE_BOTTOM;
      if (l) return SDL_HITTEST_RESIZE_LEFT;
      if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    float d = a->density > 0 ? a->density : 1.0f, fs = ImGui::GetIO().DisplayFramebufferScale.x;
    float px = pt->x * d / (fs > 0 ? fs : 1.0f), py = pt->y * d / (fs > 0 ? fs : 1.0f);
    if (py < a->dragH && px >= a->dragX0 && px < a->dragX1 && !ImGui::IsAnyItemHovered()) return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
  }, this);
  tipIndex = brushIndex("G-Pen");
  // selection undo: swap a stored region (and bbox state) with the current selection
  R.selectionSwap = [this](std::vector<uint8_t>& data, int x, int y, int w, int h, int* st) {
    int W = int(R.docW);
    if (selCpu.size() != size_t(W) * R.docH) selCpu.assign(size_t(W) * R.docH, 0);
    for (int yy = 0; yy < h; ++yy)
      std::swap_ranges(data.begin() + size_t(yy) * w, data.begin() + size_t(yy + 1) * w, selCpu.begin() + size_t(y + yy) * W + x);
    int cur[5] = {selX0, selY0, selX1, selY1, selActive ? 1 : 0};
    selX0 = st[0]; selY0 = st[1]; selX1 = st[2]; selY1 = st[3]; selActive = st[4] != 0;
    for (int i = 0; i < 5; ++i) st[i] = cur[i];
    std::string err;
    R.uploadSelection(selCpu.data(), x, y, uint32_t(w), uint32_t(h), err);
    R.setSelectionActive(selActive);
  };
}

int App::brushIndex(const std::string& name) const {
  for (size_t i = 0; i < brushes.size(); ++i)
    if (brushes[i].name == name) return int(i);
  return 0;
}

int App::legacyTip(int t) const {
  return brushIndex(t == 1 ? "Textured pen" : t == 2 ? "Soft round" : "Hard round");
}

// ---------------------------------------------------------------------------
// View

void App::screenToDoc(double sx, double sy, double& dx, double& dy) const {
  double cx = R.extent.width * 0.5, cy = R.extent.height * 0.5;
  double vx = (sx - cx) / view.zoom, vy = (sy - cy) / view.zoom;
  if (view.flipX) vx = -vx;
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

void App::strokeBegin(const PenSample& s, bool eraser, bool spline) {
  if (!R.hasDocument() || R.stroking()) return;
  const BrushSettings& b = brushes[tipIndex];
  StrokeStyle st;
  st.hardness = b.hardness;
  st.texStrength = b.texStrength;
  st.texScale = b.texScale;
  st.buildUp = b.buildUp;
  st.color[0] = color[0];
  st.color[1] = color[1];
  st.color[2] = color[2];
  st.opacity = b.opacity;
  st.eraser = eraser || eraserToggle || b.eraser;
  if (!st.eraser) noteColorUsed();
  R.beginStroke(active, st);
  engine.begin(s, b, strokeSeed++, spline && prefs.smoothStrokes);
  // mouse positions are whole screen pixels (pen positions are sub-pixel via Windows Ink HIMETRIC)
  engine.setQuantization(strokeFromPen || !prefs.mousePixelFix ? 0.0 : 0.5 / std::max(1e-6, view.zoom));
}

void App::strokeAdd(const PenSample& s) {
  if (engine.active()) engine.add(s);
}

void App::strokeEnd() {
  if (!engine.active()) return;
  lastEndX = engine.lastSample().x;
  lastEndY = engine.lastSample().y;
  haveLastStroke = true;
  engine.end();
  flushDabs(0);
  R.endStroke();
}

void App::flushDabs(uint64_t inputNs) {
  if (engine.out.empty()) return;
  R.queueDabs(engine.out);
  engine.out.clear();
  // latency is measured from when the app received the event: SDL's Windows event timestamps come
  // from the OS message clock, which only ticks every ~15.6 ms and made the numbers jump around
  if (inputNs && !pendingInputNs) pendingInputNs = SDL_GetTicksNS();
}

// ---------------------------------------------------------------------------
// Input

void App::pointerDown(float x, float y, float pressure, bool eraser, bool pen, uint64_t tNs) {
  float sx = x * density, sy = y * density;
  lastX = sx;
  lastY = sy;
  if (ImGui::GetIO().WantCaptureMouse || adj.open) return;
  if (ctrlDown && altDown && !spaceDown && !rotateDown) {  // Ctrl+Alt+drag: brush size
    sizeDrag = true;
    sizeDragX = sx;
    sizeDragY = sy;
    sizeDragStart = brushes[tipIndex].size;
    return;
  }
  if (spaceDown || rotateDown) {
    drag = (rotateDown || shiftDown) ? Drag::Rotate : Drag::Pan;
    dragFromPen = pen;
    dragX = sx;
    dragY = sy;
    dragStartAngle = std::atan2(sy - R.extent.height * 0.5, sx - R.extent.width * 0.5);
    dragStartRot = view.rotation;
    return;
  }
  double dx, dy;
  screenToDoc(sx, sy, dx, dy);
  if (toolHold.active) toolHold.used = true;  // the held tool was used: going back on release
  if (toolDown(dx, dy, sx, sy)) return;
  strokeFromPen = pen;
  if (shiftDown && haveLastStroke) {
    // Shift+click: straight line from the end of the previous stroke
    strokeBegin({lastEndX, lastEndY, pressure}, eraser, false);
    strokeAdd({dx, dy, pressure});
    flushDabs(tNs);
    strokeEnd();
    return;
  }
  strokeBegin({dx, dy, pressure}, eraser);
  flushDabs(tNs);
}

void App::pointerMove(float x, float y, float pressure, bool pen, uint64_t tNs) {
  float sx = x * density, sy = y * density;
  lastX = sx;
  lastY = sy;
  if (sizeDrag) {
    brushes[tipIndex].size = std::clamp(float(sizeDragStart * std::pow(1.01, (sx - sizeDragX) / density)), 1.0f, 5000.0f);
    return;
  }
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
  if (toolDrag) {
    double dx, dy;
    screenToDoc(sx, sy, dx, dy);
    toolMove(dx, dy, sx, sy);
    return;
  }
  if (!engine.active() && !toolDrag && (tool == ToolId::Fill || tool == ToolId::Wand)) {
    if (ImGui::GetIO().WantCaptureMouse) {
      if (hoverPreviewOn) cancelPreviews();
      hoverX = hoverY = -1;
    } else {
      double dx, dy;
      screenToDoc(sx, sy, dx, dy);
      updateHover(dx, dy);
    }
  }
  if (engine.active() && strokeFromPen == pen) {
    double dx, dy;
    screenToDoc(sx, sy, dx, dy);
    strokeAdd({dx, dy, pressure});
    flushDabs(tNs);
  }
}

void App::pointerUp(bool pen) {
  if (sizeDrag) { sizeDrag = false; return; }
  if (drag != Drag::None) {
    if (dragFromPen == pen) drag = Drag::None;  // the pen leaving range must not end a mouse drag
    return;
  }
  if (toolDrag) {
    double dx, dy;
    screenToDoc(lastX, lastY, dx, dy);
    toolUp(dx, dy);
    return;
  }
  if (engine.active() && strokeFromPen == pen) strokeEnd();
}

void App::handleKey(const SDL_KeyboardEvent& k, bool down) {
  shiftDown = (k.mod & SDL_KMOD_SHIFT) != 0;
  ctrlDown = (k.mod & SDL_KMOD_CTRL) != 0;
  altDown = (k.mod & SDL_KMOD_ALT) != 0;
  if (down && captureAct >= 0) { captureKey(k); return; }
  if (down && textEdit.active && !ImGui::GetIO().WantTextInput && textKey(k)) return;
  if (!down) {  // releases always count, even while a panel has the keyboard
    if (panKeyHeld && k.key == panKeyHeld) { panKeyHeld = 0; spaceDown = false; }
    if (rotateKeyHeld && k.key == rotateKeyHeld) { rotateKeyHeld = 0; rotateDown = false; }
    if (toolHold.active && k.key == toolHold.key) releaseToolHold(false);
    return;
  }
  if (ImGui::GetIO().WantTextInput) return;
  // while a panel widget has the keyboard, only the hide-panels key reaches the canvas shortcuts
  KeyCombo c{k.key, modsFromSdl(k.mod)};
  int a = findAction(c);
  if (ImGui::GetIO().WantCaptureKeyboard && a != int(Act::HidePanels)) return;
  if (a >= 0) {
    runAction(Act(a), k.key, k.repeat);
    return;
  }
  // context keys that are not rebindable: Enter / Esc (transform, previews, selection)
  if (!engine.active() && !toolDrag) toolKey(k.key, ctrlDown, shiftDown, altDown);
}

void App::handleEvent(const SDL_Event& e) {
  lastActivityNs = SDL_GetTicksNS();
  ImGui_ImplSDL3_ProcessEvent(&e);
  framesToRender = std::max(framesToRender, 2);
  switch (e.type) {
    case SDL_EVENT_QUIT:
      if (opt.exitAfter) running = false;
      else requestAction(PA_Quit);
      break;
    case SDL_EVENT_TEXT_INPUT:
      if (textEdit.active && !ImGui::GetIO().WantTextInput && !ctrlDown) textInsert(utf8ToU32(e.text.text));
      break;
    case SDL_EVENT_CLIPBOARD_UPDATE:
      if (!e.clipboard.owner) clipExternal = true;  // another app copied something after us
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      releaseAllKeys();  // key-ups are not delivered while another window has focus
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
      R.onResize();
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      density = SDL_GetWindowPixelDensity(window);
      if (density <= 0) density = 1;
      applyUiScale(uiScaleNow());
      R.onResize();
      break;
    case SDL_EVENT_PEN_AXIS:
      if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        penPressure = e.paxis.value;
        penHasPressure = true;
        if (penDownPending) {
          // first real pressure of this touch: start the stroke, replaying any buffered motion
          penDownPending = false;
          pointerDown(penDownX, penDownY, penPressure, penDownEraser, true, penDownNs);
          for (auto& m : penDownMoves) pointerMove(m.first, m.second, penPressure, true, e.paxis.timestamp);
          penDownMoves.clear();
        } else if (engine.active() && strokeFromPen) {
          pointerMove(e.paxis.x, e.paxis.y, penPressure, true, e.paxis.timestamp);
        }
      }
      break;
    case SDL_EVENT_PEN_DOWN:
      // Windows Ink may deliver PEN_DOWN (and even motion) before the first pressure value of the
      // new touch: wait for it so the first dab never uses the hover / previous-stroke pressure.
      penDownPending = true;
      penDownMoves.clear();
      penDownX = e.ptouch.x;
      penDownY = e.ptouch.y;
      penDownEraser = e.ptouch.eraser;
      penDownNs = e.ptouch.timestamp;
      break;
    case SDL_EVENT_PEN_MOTION:
      if (penDownPending) {
        penDownMoves.push_back({e.pmotion.x, e.pmotion.y});
        if (penDownMoves.size() < 4) break;
        // no pressure after several samples: a pen without pressure support (use full pressure)
        penDownPending = false;
        float p = penHasPressure ? penPressure : 1.0f;
        pointerDown(penDownX, penDownY, p, penDownEraser, true, penDownNs);
        for (auto& m : penDownMoves) pointerMove(m.first, m.second, p, true, e.pmotion.timestamp);
        penDownMoves.clear();
        break;
      }
      pointerMove(e.pmotion.x, e.pmotion.y, penHasPressure ? penPressure : 1.0f, true, e.pmotion.timestamp);
      break;
    case SDL_EVENT_PEN_UP:
      if (penDownPending) {  // a tap: draw a single dab
        penDownPending = false;
        pointerDown(penDownX, penDownY, penHasPressure ? std::max(penPressure, 0.3f) : 1.0f, penDownEraser, true, penDownNs);
        penDownMoves.clear();
      }
      pointerUp(true);
      break;
    case SDL_EVENT_PEN_BUTTON_DOWN:
      // barrel buttons: lower (1) = hold to pan, upper (2) = pick colour
      if (ImGui::GetIO().WantCaptureMouse) break;
      penButton(e.pbutton.button == 1 ? prefs.barrelLower : e.pbutton.button == 2 ? prefs.barrelUpper : 0, e.pbutton.x,
                e.pbutton.y, true);
      break;
    case SDL_EVENT_PEN_BUTTON_UP:
      penButton(e.pbutton.button == 1 ? prefs.barrelLower : e.pbutton.button == 2 ? prefs.barrelUpper : 0, e.pbutton.x,
                e.pbutton.y, false);
      break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
      penDownPending = false;
      penDownMoves.clear();
      pointerUp(true);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.which == SDL_PEN_MOUSEID || e.button.which == SDL_TOUCH_MOUSEID) break;
      if (e.button.button == SDL_BUTTON_LEFT) {
        pointerDown(e.button.x, e.button.y, 1.0f, false, false, e.button.timestamp);
      } else if (e.button.button == SDL_BUTTON_MIDDLE && !ImGui::GetIO().WantCaptureMouse) {
        drag = Drag::Pan;
        dragFromPen = false;
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
        zoomAt(e.wheel.mouse_x * density, e.wheel.mouse_y * density, std::pow(double(prefs.wheelZoom), e.wheel.y));
      break;
    case SDL_EVENT_DROP_FILE:
      if (e.drop.data) {
        // dropping onto an existing document imports as a layer; hold Ctrl to open instead
        if (R.hasDocument() && !ctrlDown) importAsLayer(e.drop.data);
        else requestAction(PA_OpenPath, e.drop.data);
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
  if (xf.active) cancelTransform();
  std::string err;
  if (!R.newDocument(w, h, white, err)) {
    error("Could not create a " + std::to_string(w) + " x " + std::to_string(h) + " document: " + err);
    return false;
  }
  active = 0;
  documentPath.clear();
  resetSelection();
  haveLastStroke = false;
  fitView();
  savedRevision = R.revision;
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

// Full-width slider with its label inside the value text (no clipped labels on the right).
static bool sliderF(const char* id, float* v, float mn, float mx, const char* fmt, ImGuiSliderFlags f = 0) {
  ImGui::SetNextItemWidth(-FLT_MIN);
  return ImGui::SliderFloat(id, v, mn, mx, fmt, f);
}

static const char* groupIcon(const std::string& g) {
  if (g == "Pencil") return "pencil";
  if (g == "Ink") return "pen-tool";
  if (g == "Marker") return "highlighter";
  if (g == "Paint") return "brush";
  if (g == "Airbrush") return "spray-can";
  if (g == "Eraser") return "eraser";
  return "feather";
}

void App::drawBrushLibraryButtons() {
  BrushSettings& b = brushes[tipIndex];
  // add / reset / delete
  {
    float bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    if (ImGui::Button("New", ImVec2(bw, 0))) {
      snprintf(newBrushName, sizeof newBrushName, "%s copy", b.name.c_str());
      ImGui::OpenPopup("New brush");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current settings as a new brush");
    ImGui::SameLine();
    ImGui::BeginDisabled(!b.builtIn);
    if (ImGui::Button("Reset", ImVec2(bw, 0))) {
      for (auto& d : builtInBrushes())
        if (d.name == b.name) b = d;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Restore this built-in brush's settings");
    ImGui::SameLine();
    ImGui::BeginDisabled(b.builtIn);
    if (ImGui::Button("Delete", ImVec2(bw, 0))) {
      brushes.erase(brushes.begin() + tipIndex);
      tipIndex = std::max(0, tipIndex - 1);
      ImGui::EndDisabled();
      return;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Delete this custom brush (built-in brushes can only be reset)");
    if (ImGui::BeginPopup("New brush")) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      bool ok = ImGui::InputText("##bname", newBrushName, sizeof newBrushName, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      ok |= ImGui::Button("Create");
      if (ok && newBrushName[0]) {
        BrushSettings nb = b;
        nb.name = newBrushName;
        nb.group = "Custom";
        nb.builtIn = false;
        brushes.push_back(nb);
        tipIndex = int(brushes.size()) - 1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }
}

void App::drawBrushPanel() {
  if (!ImGui::Begin("Tool Settings", &showBrush)) { ImGui::End(); return; }
  drawToolOptions();
  if (tool == ToolId::Text) { drawTextSettings(); ImGui::End(); return; }
  bool usesBrush = tool == ToolId::Brush || tool == ToolId::Line || ((tool == ToolId::Rect || tool == ToolId::Ellipse) && !shapeFilled);
  if (!usesBrush) {
    if (tool == ToolId::Fill || tool == ToolId::Gradient || tool == ToolId::Rect || tool == ToolId::Ellipse) {
      sliderF("##op", &brushes[tipIndex].opacity, 0.0f, 1.0f, "Opacity  %.2f");
      ImGui::Checkbox("Erase instead of paint", &eraserToggle);
    }
    ImGui::End();
    return;
  }
  // current brush (the full library with stroke previews is the Tool Group panel)
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##brushsel", brushes[tipIndex].name.c_str(), ImGuiComboFlags_HeightLarge)) {
    std::string lastGroup;
    for (int i = 0; i < int(brushes.size()); ++i) {
      if (brushes[i].group != lastGroup) { lastGroup = brushes[i].group; ImGui::SeparatorText(lastGroup.c_str()); }
      ImGui::PushID(i);
      if (ImGui::Selectable(brushes[i].name.c_str(), i == tipIndex)) tipIndex = i;
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  BrushSettings& cb = brushes[tipIndex];
  ImGui::Checkbox("Eraser", &eraserToggle);
  if (cb.eraser) { ImGui::SameLine(); ImGui::TextDisabled("(eraser brush)"); }
  sliderF("##size", &cb.size, 1.0f, 5000.0f, "Size  %.1f px", ImGuiSliderFlags_Logarithmic);
  sliderF("##opacity", &cb.opacity, 0.0f, 1.0f, "Opacity  %.2f");
  sliderF("##flow", &cb.flow, 0.01f, 1.0f, "Flow  %.2f");
  float sp = cb.spacing * 100;
  if (sliderF("##spacing", &sp, 1.0f, 200.0f, "Spacing  %.0f %%", ImGuiSliderFlags_Logarithmic)) cb.spacing = sp / 100;
  if (ImGui::CollapsingHeader("Tip shape")) {
    sliderF("##hard", &cb.hardness, 0.0f, 1.0f, "Hardness  %.2f");
    float rp = cb.roundness * 100;
    if (sliderF("##round", &rp, 5.0f, 100.0f, "Roundness  %.0f %%")) cb.roundness = rp / 100;
    sliderF("##angle", &cb.angle, -180.0f, 180.0f, "Angle  %.0f deg");
    ImGui::Checkbox("Angle follows the stroke", &cb.followStroke);
  }
  if (ImGui::CollapsingHeader("Texture")) {
    sliderF("##tex", &cb.texStrength, 0.0f, 1.0f, "Paper grain  %.2f");
    sliderF("##grain", &cb.texScale, 1.0f, 64.0f, "Grain scale  %.1f px", ImGuiSliderFlags_Logarithmic);
  }
  if (ImGui::CollapsingHeader("Scatter & spray")) {
    sliderF("##scatter", &cb.scatter, 0.0f, 2.0f, "Scatter  %.2f");
    sliderF("##sj", &cb.sizeJitter, 0.0f, 1.0f, "Size jitter  %.2f");
    sliderF("##fj", &cb.flowJitter, 0.0f, 1.0f, "Flow jitter  %.2f");
    ImGui::Checkbox("Build up (airbrush)", &cb.buildUp);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##parts", &cb.particles, 0, 64, cb.particles ? "Spray particles  %d" : "Spray particles  off");
    if (cb.particles) sliderF("##psize", &cb.particleSize, 1.0f, 40.0f, "Particle size  %.1f px", ImGuiSliderFlags_Logarithmic);
  }
  if (!ImGui::CollapsingHeader("Pen pressure", ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::End(); return; }
  BrushSettings& pb = cb;
  ImGui::Checkbox("Size", &pb.pressureSize);
  ImGui::SameLine();
  ImGui::Checkbox("Opacity", &pb.pressureOpacity);
  if (pb.pressureSize) {
    float m = pb.minSize * 100;
    if (sliderF("##min", &m, 0.0f, 100.0f, "Min size  %.0f %%")) pb.minSize = m / 100;
  }
  sliderF("##gamma", &pb.gamma, 0.2f, 5.0f, "Curve  %.2f  (soft < 1 < firm)", ImGuiSliderFlags_Logarithmic);
  // curve preview: pen pressure (x) -> effect (y)
  float w = ImGui::GetContentRegionAvail().x, h = std::min(70.0f, w * 0.35f);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
  ImVec2 pts[33];
  for (int i = 0; i <= 32; ++i) {
    float x = i / 32.0f, y = std::pow(x, pb.gamma);
    pts[i] = ImVec2(p0.x + 4 + x * (w - 8), p0.y + h - 4 - y * (h - 8));
  }
  dl->AddPolyline(pts, 33, ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
  float pp = std::pow(std::clamp(penPressure, 0.0f, 1.0f), pb.gamma);
  dl->AddCircleFilled(ImVec2(p0.x + 4 + penPressure * (w - 8), p0.y + h - 4 - pp * (h - 8)), 3.5f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
  ImGui::Dummy(ImVec2(w, h));
  ImGui::TextDisabled("Dot = current pen pressure");
  ImGui::End();
}

void App::drawColorPanel() {
  if (!ImGui::Begin("Colour", &showColor)) { ImGui::End(); return; }
  float line = ImGui::GetFrameHeightWithSpacing();
  float reserve = line * (showColorSliders ? 4.4f : 1.4f) + (recentColors.empty() ? 0 : line);
  ImVec2 avail = ImGui::GetContentRegionAvail();
  // square SV + hue bar (CSP style) by default, hue wheel optional; as large as the panel allows
  float side = std::max(80.0f, std::min(avail.x - (pickerWheel ? 0 : ImGui::GetFrameHeight() * 1.2f), avail.y - reserve));
  ImGui::SetNextItemWidth(side + (pickerWheel ? 0 : ImGui::GetFrameHeight() * 1.2f));
  ImGui::ColorPicker3("##picker", color,
                      (pickerWheel ? ImGuiColorEditFlags_PickerHueWheel : ImGuiColorEditFlags_PickerHueBar) |
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoLabel |
                          ImGuiColorEditFlags_NoSmallPreview);
  if (ImGui::BeginPopupContextItem("##pickmode")) {
    if (ImGui::MenuItem("Square + hue bar", nullptr, !pickerWheel)) pickerWheel = false;
    if (ImGui::MenuItem("Hue wheel", nullptr, pickerWheel)) pickerWheel = true;
    ImGui::EndPopup();
  }
  // foreground / background swatches, hex, slider toggle - one row
  float sw = ImGui::GetFrameHeight();
  ImVec2 c0 = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(c0.x + sw * 0.6f, c0.y + sw * 0.35f));
  if (ImGui::ColorButton("##bg", ImVec4(bgColor[0], bgColor[1], bgColor[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(sw, sw)))
    for (int k = 0; k < 3; ++k) std::swap(color[k], bgColor[k]);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background colour - click or X to swap");
  ImGui::SetCursorScreenPos(c0);
  ImGui::ColorButton("##fg", ImVec4(color[0], color[1], color[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(sw, sw));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current colour");
  ImGui::SetCursorScreenPos(ImVec2(c0.x + sw * 1.9f, c0.y));
  char hex[8];
  snprintf(hex, sizeof hex, "%02X%02X%02X", int(color[0] * 255 + 0.5f), int(color[1] * 255 + 0.5f), int(color[2] * 255 + 0.5f));
  ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - sw * 2.2f));
  if (ImGui::InputText("##hex", hex, sizeof hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
    unsigned v = 0;
    if (strlen(hex) == 6 && sscanf(hex, "%x", &v) == 1)
      for (int k = 0; k < 3; ++k) color[k] = ((v >> (16 - 8 * k)) & 255) / 255.0f;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hex colour (RRGGBB)");
  ImGui::SameLine();
  if (ImGui::Button(showColorSliders ? "HSV -" : "HSV +", ImVec2(-FLT_MIN, 0))) showColorSliders = !showColorSliders;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show / hide the H, S, V sliders (right-click the picker for the wheel)");
  ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + sw * 1.45f));
  ImGui::Dummy(ImVec2(0, 0));
  if (showColorSliders) {
    float h, sat, val;
    ImGui::ColorConvertRGBtoHSV(color[0], color[1], color[2], h, sat, val);
    if (sat > 0.0f && val > 0.0f) lastHue = h;
    float H = lastHue * 360, S = sat * 100, V = val * 100;
    bool ch = sliderF("##h", &H, 0, 360, "H  %.0f deg");
    ch |= sliderF("##s", &S, 0, 100, "S  %.0f %%");
    ch |= sliderF("##v", &V, 0, 100, "V  %.0f %%");
    if (ch) {
      lastHue = std::clamp(H / 360.0f, 0.0f, 0.9999f);
      ImGui::ColorConvertHSVtoRGB(lastHue, S / 100, V / 100, color[0], color[1], color[2]);
    }
  }
  if (!recentColors.empty()) {
    float bs = ImGui::GetFrameHeight() * 0.8f;
    int perRow = std::max(1, int(ImGui::GetContentRegionAvail().x / (bs + 3)));
    for (size_t i = 0; i < recentColors.size() && i < size_t(perRow); ++i) {
      if (i) ImGui::SameLine(0, 3);
      ImGui::PushID(int(i));
      auto& c = recentColors[i];
      if (ImGui::ColorButton("##r", ImVec4(c[0], c[1], c[2], 1), 0, ImVec2(bs, bs)))
        for (int k = 0; k < 3; ++k) color[k] = c[k];
      ImGui::PopID();
    }
  }
  ImGui::End();
}

void App::noteColorUsed() {
  std::array<float, 3> c{color[0], color[1], color[2]};
  auto same = [&](const std::array<float, 3>& o) {
    return std::abs(o[0] - c[0]) < 0.002f && std::abs(o[1] - c[1]) < 0.002f && std::abs(o[2] - c[2]) < 0.002f;
  };
  recentColors.erase(std::remove_if(recentColors.begin(), recentColors.end(), same), recentColors.end());
  recentColors.insert(recentColors.begin(), c);
  if (recentColors.size() > 16) recentColors.resize(16);
}

void App::buildDefaultLayout(unsigned int dockId) {
  // [tools] [navigator / tool group] [ canvas ....... ] [ colour / tool settings / layers / performance ]
  ImGui::DockBuilderRemoveNode(dockId);
  ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
  ImGuiID centre = dockId;
  ImGuiID tools = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.032f, nullptr, &centre);
  ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.17f, nullptr, &centre);
  ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, nullptr, &centre);
  ImGuiID leftBottom;
  ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.36f, nullptr, &leftBottom);
  ImGuiID rightRest;
  ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.33f, nullptr, &rightRest);
  ImGuiID rightPerf = ImGui::DockBuilderSplitNode(rightRest, ImGuiDir_Down, 0.27f, nullptr, &rightRest);
  ImGuiID rightLayers = ImGui::DockBuilderSplitNode(rightRest, ImGuiDir_Down, 0.48f, nullptr, &rightRest);
  ImGui::DockBuilderDockWindow("Tools", tools);
  ImGui::DockBuilderDockWindow("Navigator", leftTop);
  ImGui::DockBuilderDockWindow("Tool Group", leftBottom);
  ImGui::DockBuilderDockWindow("Colour", rightTop);
  ImGui::DockBuilderDockWindow("Tool Settings", rightRest);
  ImGui::DockBuilderDockWindow("Layer Properties", rightRest);
  ImGui::DockBuilderDockWindow("Layers", rightLayers);
  ImGui::DockBuilderDockWindow("Performance", rightPerf);
  ImGui::DockBuilderFinish(dockId);
}

static void drawEye(ImDrawList* dl, ImVec2 c, float r, bool open, ImU32 col) {
  if (open) {
    dl->AddEllipse(c, ImVec2(r, r * 0.55f), col, 0, 0, 1.5f);
    dl->AddCircleFilled(c, r * 0.3f, col);
  } else {
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.5f);
  }
}

void App::drawLayerPanel() {
  if (!ImGui::Begin("Layers", &showLayers)) { ImGui::End(); return; }
  if (!R.hasDocument()) { ImGui::TextUnformatted("No document"); ImGui::End(); return; }
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
  R.updateThumbnails(2);
  bool locked = R.stroking() || xf.active;
  ImGui::BeginDisabled(locked);
  Layer& L = R.layers[active];
  // one row: blend mode | opacity | lock transparency
  float fh = ImGui::GetFrameHeight();
  float wAvail = ImGui::GetContentRegionAvail().x;
  ImGui::SetNextItemWidth(wAvail * 0.48f);
  if (ImGui::BeginCombo("##blend", blendModeName(L.mode), ImGuiComboFlags_HeightLarge)) {
    for (int m = 0; m < int(BlendMode::Count); ++m) {
      if (ImGui::Selectable(blendModeName(BlendMode(m)), m == int(L.mode)) && m != int(L.mode)) {
        R.recordLayerProps(active);
        L.mode = BlendMode(m);
        R.markCachesDirty();
      }
      if (m == 0 || m == 5 || m == 11 || m == 20 || m == 23) ImGui::Separator();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  float before = L.opacity;
  float op = L.opacity * 100;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fh - ImGui::GetStyle().ItemSpacing.x);
  bool changed = ImGui::SliderFloat("##lop", &op, 0.0f, 100.0f, "%.0f %%");
  if (ImGui::IsItemActivated()) { L.opacity = before; R.recordLayerProps(active); }
  if (changed) { L.opacity = op / 100; R.markCachesDirty(); }
  ImGui::SameLine();
  {
    if (iconButton("##lock", L.lockAlpha ? "lock" : "lock-open", fh, L.lockAlpha, nullptr)) {
      R.recordLayerProps(active);
      L.lockAlpha = !L.lockAlpha;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock transparency: paint only where the layer has pixels");
  }
  // layer list, top layer first: eye | thumbnail | name + mode/opacity
  float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2;
  ImGui::BeginChild("list", ImVec2(0, -footer));
  float rowH = std::max(40.0f, ImGui::GetTextLineHeight() * 2.6f);
  float thumb = rowH - 6;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = int(R.layers.size()) - 1; i >= 0; --i) {
    Layer& l = R.layers[i];
    ImGui::PushID(int(l.id));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable("##row", i == active, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(w, rowH))) {
      active = i;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        renameIndex = i;
        snprintf(renameText, sizeof renameText, "%s", l.name.c_str());
        ImGui::OpenPopup("Rename layer");
      }
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("SARASWATI_LAYER", &i, sizeof i);
      ImGui::Text("Move %s", l.name.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SARASWATI_LAYER")) {
        int from = *static_cast<const int*>(pl->Data);
        active = from;
        dropLayerBlock(from, i);
      }
      ImGui::EndDragDropTarget();
    }
    float ind = 0;
    if (isSublayer(l)) {  // sublayer: thumbnail and name indented, with a connector to its parent above
      ind = 18;
      ImU32 lc = ImGui::GetColorU32(ImGuiCol_TextDisabled);
      dl->AddLine(ImVec2(p.x + 36, p.y - 2), ImVec2(p.x + 36, p.y + rowH / 2), lc, 1.5f);
      dl->AddLine(ImVec2(p.x + 36, p.y + rowH / 2), ImVec2(p.x + 30 + ind, p.y + rowH / 2), lc, 1.5f);
    }
    // eye (visibility)
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2, p.y + (rowH - 22) / 2));
    if (ImGui::InvisibleButton("##eye", ImVec2(22, 22))) { R.recordLayerProps(i); l.visible = !l.visible; R.markCachesDirty(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(l.visible ? "Hide layer" : "Show layer");
    drawIcon(dl, l.visible ? "eye" : "eye-off", ImVec2(p.x + 13, p.y + rowH / 2), 17,
             ImGui::GetColorU32(l.visible ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    // thumbnail on a checkerboard
    float tw = thumb, th = thumb;
    if (l.thumb.width && l.thumb.height) {
      float a = float(l.thumb.width) / float(l.thumb.height);
      if (a >= 1) th = thumb / a; else tw = thumb * a;
    }
    ImVec2 t0(p.x + 30 + ind + (thumb - tw) / 2, p.y + 3 + (thumb - th) / 2), t1(t0.x + tw, t0.y + th);
    dl->AddRectFilled(t0, t1, IM_COL32(205, 205, 205, 255));
    for (float yy = 0; yy < th; yy += 6)
      for (float xx = (int(yy / 6) % 2) * 6.0f; xx < tw; xx += 12)
        dl->AddRectFilled(ImVec2(t0.x + xx, t0.y + yy), ImVec2(std::min(t0.x + xx + 6, t1.x), std::min(t0.y + yy + 6, t1.y)), IM_COL32(245, 245, 245, 255));
    if (l.thumbTex) dl->AddImage(ImTextureRef(ImTextureID(uint64_t(l.thumbTex))), t0, t1);
    dl->AddRect(t0, t1, ImGui::GetColorU32(ImGuiCol_Border));
    // name + details
    float tx = p.x + 38 + ind + thumb;
    dl->AddText(ImVec2(tx, p.y + rowH / 2 - ImGui::GetTextLineHeight() - 1), ImGui::GetColorU32(ImGuiCol_Text), l.name.c_str());
    char info[96];
    snprintf(info, sizeof info, "%s  %.0f%%%s%s%s", blendModeName(l.mode), l.opacity * 100, l.lockAlpha ? "  locked" : "",
             l.tone.on ? "  tone" : "", l.lcolor.on ? "  colour" : "");
    dl->AddText(ImVec2(tx, p.y + rowH / 2 + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), info);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 2));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
  }
  if (renameIndex >= 0) ImGui::OpenPopup("Rename layer");
  if (ImGui::BeginPopup("Rename layer")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool done = ImGui::InputText("##name", renameText, sizeof renameText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    done |= ImGui::Button("OK");
    if (done && renameIndex >= 0 && renameIndex < int(R.layers.size())) {
      R.recordLayerProps(renameIndex);
      R.layers[renameIndex].name = renameText;
      renameIndex = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else {
    renameIndex = -1;
  }
  ImGui::EndChild();
  // footer: new, duplicate, delete, move up, move down (drawn icons)
  {
    float fh2 = ImGui::GetFrameHeight();
    auto iconBtn = [&](const char* id, const char* tip, bool enabled, int icon) {
      static const char* names[5] = {"plus", "copy", "trash", "arrow-up", "arrow-down"};
      return iconButton(id, names[icon], fh2 * 1.25f, false, tip, enabled);
    };
    if (iconBtn("##new", "New layer", true, 0)) addLayer();
    ImGui::SameLine();
    if (iconBtn("##dup", "Duplicate layer", true, 1)) {
      std::string err;
      int i = R.duplicateLayer(active, err);
      if (i < 0) error(err); else active = i;
    }
    ImGui::SameLine();
    if (iconBtn("##del", "Delete layer (undoable)", R.layers.size() > 1, 2)) deleteLayerBlock(active);
    ImGui::SameLine();
    if (iconBtn("##up", "Move layer up", active + 1 < int(R.layers.size()), 3)) moveLayerBlock(active, 1);
    ImGui::SameLine();
    if (iconBtn("##down", "Move layer down", active > 0, 4)) moveLayerBlock(active, -1);
    ImGui::SameLine();
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d / %d", int(R.layers.size()), R.limit.maxLayers);
    if (ImGui::CalcTextSize(cnt).x <= ImGui::GetContentRegionAvail().x) {
      ImGui::TextDisabled("%s", cnt);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Layers / maximum that fit in GPU memory at this size (%s each)%s",
                          fmtBytes(double(R.limit.layerBytes)).c_str(), R.cpuEmulation ? "\nEstimate: CPU emulation" : "");
    }
  }
  ImGui::EndDisabled();
  ImGui::End();
}

void App::drawPerfPanel() {
  if (!ImGui::Begin("Performance", &showPerf)) { ImGui::End(); return; }
  ImGui::TextWrapped("%s", R.deviceName.c_str());
  ImGui::TextDisabled("%s", R.driverInfo.c_str());
  if (R.cpuEmulation) ImGui::TextWrapped("CPU emulation - timings are not representative.");
  ImGui::Separator();
  if (SDL_GetTicksNS() - lastActivityNs > 700000000ull && !bench.running && !R.busy())
    ImGui::TextWrapped("Idle - the canvas redraws only when something changes (0 %% CPU); this panel refreshes "
                       "twice a second. Numbers below are from the last real frame.");
  else
    ImGui::Text("FPS %.1f   CPU frame %.2f ms", ImGui::GetIO().Framerate, cpuFrameMs);
  const GpuTimings& t = R.lastTimings;
  if (R.timestampsSupported) {
    ImGui::Text("GPU  total %.2f ms", t.total);
    ImGui::TextDisabled("dabs %.2f  commit %.2f  caches %.2f  composite %.2f", t.dabs, t.commit, t.caches, t.composite);
  } else {
    ImGui::TextUnformatted("GPU timestamps not supported");
  }
  ImGui::Text("Input -> present  %.2f ms (avg %.2f)", lastLatencyMs, avgLatencyMs);
  ImGui::Text("Dabs this frame %u", R.lastFrameDabs);
  ImGui::Text("Layers VRAM %s / %s%s", fmtBytes(double(R.layerBytesTotal())).c_str(), fmtBytes(double(R.memoryBudgetBytes)).c_str(),
              R.cpuEmulation ? " (est.)" : "");
  ImGui::Text("Undo RAM %s / %s, %zu steps", fmtBytes(double(R.undoBytes())).c_str(), fmtBytes(double(R.undoBudgetBytes)).c_str(),
              R.undoSteps());
  const char* names[] = {"Immediate (lowest latency, tearing)", "Mailbox (low latency)", "FIFO (vsync)", "FIFO relaxed"};
  const char* cur = int(R.presentMode) < 4 ? names[R.presentMode] : "?";
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##present", cur)) {
    for (VkPresentModeKHR m : R.presentModes)
      if (int(m) < 3 && ImGui::Selectable(names[m], m == R.presentMode)) R.setPresentMode(m);
    ImGui::EndCombo();
  }
  ImGui::SeparatorText("Benchmark");
  ImGui::SetNextItemWidth(-FLT_MIN);
  int wh[2] = {benchUiW, benchUiH};
  if (ImGui::InputInt2("##bdoc", wh)) { benchUiW = std::clamp(wh[0], 16, 300000); benchUiH = std::clamp(wh[1], 16, 300000); }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Document width x height (px)");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::SliderInt("##bbrush", &benchUiBrush, 1, 5000, "Brush %d px", ImGuiSliderFlags_Logarithmic);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::Combo("##btip", &benchUiTip, kTipNames, 3);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::SliderInt("##blayers", &benchUiLayers, 1, 16, "%d layers");
  ImGui::SameLine();
  ImGui::BeginDisabled(bench.running || R.stroking());
  if (ImGui::Button("Run benchmark", ImVec2(-FLT_MIN, 0)))
    startBenchmark(uint32_t(benchUiW), uint32_t(benchUiH), float(benchUiBrush), Tip(benchUiTip), benchUiLayers);
  ImGui::EndDisabled();
  if (!bench.report.empty()) {
    if (ImGui::SmallButton("Copy report")) ImGui::SetClipboardText(bench.report.c_str());
    ImGui::TextWrapped("%s", bench.report.c_str());
  }
  ImGui::End();
}

void App::drawNewDocDialog() {
  if (showNewDoc) { ImGui::OpenPopup("New Document"); showNewDoc = false; }
  if (!ImGui::BeginPopupModal("New Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  struct Preset { const char* name; int w, h; };
  static const Preset presets[] = {{"A4 300 dpi", 2480, 3508}, {"A3 300 dpi", 3508, 4961}, {"Full HD", 1920, 1080},
                                   {"4K UHD", 3840, 2160}, {"Square 4000", 4000, 4000}, {"Huge 30000", 30000, 30000}};
  for (int i = 0; i < 6; ++i) {
    if (i % 3) ImGui::SameLine();
    if (ImGui::Button(presets[i].name, ImVec2(120, 0))) { newW = presets[i].w; newH = presets[i].h; }
  }
  ImGui::SetNextItemWidth(250);
  int wh[2] = {newW, newH};
  if (ImGui::InputInt2("Width x height (px)", wh)) { newW = wh[0]; newH = wh[1]; }
  if (ImGui::Button("Swap")) std::swap(newW, newH);
  newW = std::clamp(newW, 1, 300000);
  newH = std::clamp(newH, 1, 300000);
  ImGui::Checkbox("White paper (off = transparent)", &newWhite);
  if (newLimitW != newW || newLimitH != newH) {
    newLimit = R.computeLayerLimit(uint32_t(newW), uint32_t(newH));
    newLimitW = newW;
    newLimitH = newH;
  }
  if (newLimit.ok)
    ImGui::Text("Up to %d layers%s  (%s per layer)", newLimit.maxLayers, R.cpuEmulation ? " (estimate, CPU emulation)" : "",
                fmtBytes(double(newLimit.layerBytes)).c_str());
  else
    ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", newLimit.reason.c_str());
  ImGui::BeginDisabled(!newLimit.ok);
  if (ImGui::Button("Create", ImVec2(120, 0)) || (newLimit.ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
    newDocument(uint32_t(newW), uint32_t(newH), newWhite);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---- unsaved changes ----

void App::requestAction(int a, const std::string& path) {
  pendingAction = a;
  pendingPath = path;
  if (modified() && prefs.confirmUnsaved) askUnsaved = true;
  else performAction();
}

void App::performAction() {
  int a = pendingAction;
  pendingAction = PA_None;
  continueAfterSave = false;
  switch (a) {
    case PA_Quit: running = false; break;
    case PA_New: showNewDoc = true; break;
    case PA_OpenDialog: showDialog(DlgOpen); break;
    case PA_OpenPath: openFile(pendingPath); break;
    default: break;
  }
}

void App::drawUnsavedDialog() {
  if (askUnsaved) { ImGui::OpenPopup("Unsaved changes"); askUnsaved = false; }
  if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  std::string name = documentPath.empty() ? std::string("Untitled")
                                          : fs::path(reinterpret_cast<const char8_t*>(documentPath.c_str())).filename().string();
  ImGui::Text("Save changes to \"%s\" first?", name.c_str());
  ImGui::Spacing();
  if (ImGui::Button("Save", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    continueAfterSave = true;
    if (documentPath.empty()) showDialog(DlgSave); else saveFile(documentPath);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Don't save", ImVec2(110, 0))) { performAction(); ImGui::CloseCurrentPopup(); }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pendingAction = PA_None;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// Title and minimise / maximise / close in the app's own menu bar (the window has no OS frame).
void App::drawWindowButtons() {
  float h = ImGui::GetFrameHeight();
  float bw = h * 1.6f;
  float x0 = ImGui::GetCursorScreenPos().x + 8;
  float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
  float bx = right - bw * 3;
  // document title, centred in the free space (the free space also drags the window)
  std::string name = documentPath.empty() ? std::string("Untitled")
                                          : fs::path(reinterpret_cast<const char8_t*>(documentPath.c_str())).filename().string();
  std::string title = name + (modified() ? " *" : "") + "  -  Saraswati " SARASWATI_VERSION;
  ImVec2 ts = ImGui::CalcTextSize(title.c_str());
  float tx = std::max(x0, (x0 + bx - ts.x) * 0.5f);
  if (tx + ts.x < bx - 8)
    ImGui::GetWindowDrawList()->AddText(ImVec2(tx, ImGui::GetWindowPos().y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        title.c_str());
  dragX0 = x0;
  dragX1 = bx;
  dragH = h;
  bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
  ImGui::SetCursorScreenPos(ImVec2(bx, ImGui::GetWindowPos().y));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  auto btn = [&](const char* id, const char* icon, bool danger) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? ImVec4(0.78f, 0.17f, 0.17f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? ImVec4(0.6f, 0.12f, 0.12f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool r = ImGui::Button(id, ImVec2(bw, h));
    ImGui::PopStyleColor(3);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), h * 0.62f, ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::SameLine();
    return r;
  };
  if (btn("##min", "minus", false)) SDL_MinimizeWindow(window);
  if (btn("##max", maximized ? "copy" : "square", false)) {
    if (maximized) SDL_RestoreWindow(window);
    else SDL_MaximizeWindow(window);
  }
  if (btn("##close", "x", true)) requestAction(PA_Quit);
  ImGui::PopStyleVar(2);
}

// Live preview of the brush size in the middle of the canvas while it is being changed.
void App::drawSizePreview() {
  float size = brushes[tipIndex].size;
  uint64_t now = SDL_GetTicksNS();
  if (lastSizeSeen >= 0 && std::abs(size - lastSizeSeen) > 1e-4f && !R.stroking() && !bench.running && opt.exitAfter == false)
    sizePreviewUntil = now + 900000000ull;
  lastSizeSeen = size;
  if (now >= sizePreviewUntil || !R.hasDocument()) return;
  float s = ImGui::GetIO().DisplayFramebufferScale.x;
  float fade = std::min(1.0f, float(sizePreviewUntil - now) / 300000000.0f);
  ImVec2 c((canvasX + canvasW * 0.5f) / s, (canvasY + canvasH * 0.5f) / s);
  const BrushSettings& b = brushes[tipIndex];
  float r = std::max(1.0f, float(size * 0.5 * view.zoom) / s);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->PushClipRect(ImVec2(canvasX / s, canvasY / s), ImVec2((canvasX + canvasW) / s, (canvasY + canvasH) / s), true);
  float ang = float(b.angle * 3.14159265 / 180.0 + view.rotation);
  ImVec2 rad(r, std::max(1.0f, r * b.roundness));
  dl->AddEllipseFilled(c, rad, IM_COL32(0, 0, 0, int(60 * fade)), ang);
  dl->AddEllipse(c, ImVec2(rad.x + 1, rad.y + 1), IM_COL32(255, 255, 255, int(200 * fade)), ang, 0, 1.5f);
  dl->AddEllipse(c, rad, IM_COL32(0, 0, 0, int(230 * fade)), ang, 0, 1.5f);
  char t[48];
  snprintf(t, sizeof t, "%.1f px", size);
  ImVec2 ts = ImGui::CalcTextSize(t);
  ImVec2 tp(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
  dl->AddRectFilled(ImVec2(tp.x - 6, tp.y - 3), ImVec2(tp.x + ts.x + 6, tp.y + ts.y + 3), IM_COL32(30, 30, 30, int(200 * fade)), 4);
  dl->AddText(tp, IM_COL32(240, 240, 240, int(255 * fade)), t);
  dl->PopClipRect();
}

float App::uiScaleNow() const {
  if (prefs.uiScalePct > 0) return prefs.uiScalePct / 100.0f;
  float d = SDL_GetWindowPixelDensity(window);
  return SDL_GetWindowDisplayScale(window) / std::max(1.0f, d > 0 ? d : 1.0f);
}

void App::applyPrefs() {
  applyUiScale(uiScaleNow());
  R.maxUndoSteps = prefs.undoSteps;
  R.undoBudgetBytes = prefs.undoGB > 0 ? VkDeviceSize(double(prefs.undoGB) * (1ull << 30)) : R.undoBudgetAuto;
}

// Pen barrel buttons (actions chosen in Preferences > Pen & input).
void App::penButton(int action, float x, float y, bool down) {
  if (action == 1) {  // hold to pan
    if (down && !ImGui::GetIO().WantCaptureMouse) {
      drag = Drag::Pan;
      dragFromPen = true;
      dragX = x * density;
      dragY = y * density;
    } else if (!down && drag == Drag::Pan && dragFromPen) {
      drag = Drag::None;
    }
  } else if (action == 2 && down && !ImGui::GetIO().WantCaptureMouse) {  // pick colour
    for (int k = 0; k < 3; ++k) pickPrev[k] = color[k];
    R.requestPick(int(x * density), int(y * density));
  } else if (action == 3 && down) {
    eraserToggle = !eraserToggle;
  }
}

// Preferences window, laid out like Claude Code's settings: sections on the left, settings on
// the right as rows of name + explanation + control.
void App::drawPrefs() {
  if (!showPrefs) return;
  float sc = ImGui::GetStyle().FontScaleDpi;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(780 * sc, 520 * sc), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Preferences", &showPrefs, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
  static const char* pages[] = {"General", "Interface", "Pen & input", "Canvas", "Performance", "Shortcuts", "About"};
  ImGui::BeginChild("nav", ImVec2(170 * sc, 0), ImGuiChildFlags_Borders);
  for (int i = 0; i < 7; ++i)
    if (ImGui::Selectable(pages[i], prefsPage == i, 0, ImVec2(0, ImGui::GetFrameHeight()))) prefsPage = i;
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
  ImGui::TextUnformatted(pages[prefsPage]);
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::Spacing();
  bool changed = false;
  auto row = [&](const char* title, const char* desc, const std::function<bool()>& control) {
    ImGui::PushID(title);
    float w = ImGui::GetContentRegionAvail().x;
    float cw = std::min(230 * sc, w * 0.42f);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - cw - 20 * sc);
    ImGui::TextUnformatted(title);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", desc);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float h = ImGui::GetItemRectSize().y;
    ImGui::SameLine(w - cw);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (h - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::SetNextItemWidth(cw);
    changed |= control();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PopID();
  };
  auto toggle = [](bool* v) { return [v] { return ImGui::Checkbox("##t", v); }; };
  switch (prefsPage) {
    case 0:
      row("Start maximised", "Open the window maximised on start-up.", toggle(&prefs.startMaximized));
      row("Show Performance panel at start", "Useful while testing: FPS, GPU time, latency and memory are visible right away.",
          toggle(&prefs.perfAtStart));
      row("Ask before discarding changes", "When quitting, creating or opening a document with unsaved changes.",
          toggle(&prefs.confirmUnsaved));
      row("Automatic backups", "Saves a copy of the picture in the background (no dialogs, no waiting) whenever it changed. "
                               "After a crash, the next start offers to restore it. File > Restore from backup lists all versions.",
          toggle(&prefs.backupOn));
      row("Backup every", "Minutes between automatic backups.",
          [&] { return ImGui::SliderInt("##bm", &prefs.backupMinutes, 1, 60, "%d min"); });
      row("Backup versions kept", "Older backups of the same picture are deleted.",
          [&] { return ImGui::SliderInt("##bk", &prefs.backupKeep, 1, 200, "%d"); });
      break;
    case 1: {
      row("Interface size", "Automatic follows the Windows display scaling of the monitor the window is on (e.g. 150 % on a "
                            "2K laptop, 200 % on a 4K screen) and adapts when the window moves to another monitor.",
          [&] {
            const char* items[] = {"Automatic", "75 %", "100 %", "125 %", "150 %", "175 %", "200 %", "250 %"};
            const int vals[] = {0, 75, 100, 125, 150, 175, 200, 250};
            int cur = 0;
            for (int i = 0; i < 8; ++i) if (vals[i] == prefs.uiScalePct) cur = i;
            if (ImGui::Combo("##s", &cur, items, 8)) { prefs.uiScalePct = vals[cur]; return true; }
            return false;
          });
      row("Reset panel layout", "Put all panels back to the default arrangement.", [&] {
        if (ImGui::Button("Reset layout", ImVec2(-FLT_MIN, 0))) { resetLayout = true; showBrush = showColor = showLayers = showNav = showToolGroup = true; }
        return false;
      });
      break;
    }
    case 2: {
      row("Smooth strokes", "Draw a smooth curve through the pen samples (no stabiliser: the line still goes through every "
                            "sample; adds one sample of delay, about 4 ms).", toggle(&prefs.smoothStrokes));
      row("Mouse pixel correction", "Mouse positions come in whole screen pixels; zoomed out that makes steps in the line. "
                                    "This corrects for it (pens already report sub-pixel positions).", toggle(&prefs.mousePixelFix));
      const char* acts[] = {"Nothing", "Hold to pan", "Pick colour", "Toggle eraser"};
      row("Pen lower button", "Action of the barrel button closer to the tip. In the Wacom settings, leave the button on its "
                              "default function so the app receives it.", [&] { return ImGui::Combo("##l", &prefs.barrelLower, acts, 4); });
      row("Pen upper button", "Action of the barrel button further from the tip.",
          [&] { return ImGui::Combo("##u", &prefs.barrelUpper, acts, 4); });
      break;
    }
    case 3: {
      row("Mouse-wheel zoom step", "How much one wheel notch zooms.",
          [&] { return ImGui::SliderFloat("##w", &prefs.wheelZoom, 1.05f, 2.0f, "x %.2f per notch"); });
      const char* names[] = {"Immediate (lowest latency, may tear)", "Mailbox (low latency)", "FIFO (vsync)", "FIFO relaxed"};
      row("Display mode", "How finished frames reach the screen. Mailbox is the best default for drawing.", [&] {
        bool c = false;
        if (ImGui::BeginCombo("##p", int(R.presentMode) < 4 ? names[R.presentMode] : "?")) {
          for (VkPresentModeKHR m : R.presentModes)
            if (int(m) < 3 && ImGui::Selectable(names[m], m == R.presentMode)) { R.setPresentMode(m); c = true; }
          ImGui::EndCombo();
        }
        return c;
      });
      break;
    }
    case 4: {
      row("Frame rate limit", "Maximum frames per second drawn on screen. Pen input is still read at full speed; a limit "
                              "saves GPU power and heat. Pick your monitor's refresh rate or higher.",
          [&] {
            const int vals[] = {30, 60, 90, 120, 144, 165, 240, 360, 0};
            const char* items[] = {"30 fps", "60 fps", "90 fps", "120 fps", "144 fps", "165 fps", "240 fps", "360 fps", "Unlimited"};
            int cur = 3;
            for (int i = 0; i < 9; ++i) if (vals[i] == prefs.fpsLimit) cur = i;
            if (ImGui::Combo("##fps", &cur, items, 9)) { prefs.fpsLimit = vals[cur]; return true; }
            return false;
          });
      row("Undo steps", "How many steps back you can go. More steps use more system memory.",
          [&] { return ImGui::SliderInt("##us", &prefs.undoSteps, 5, 500); });
      char auto_[64];
      snprintf(auto_, sizeof auto_, "Automatic (%s)", fmtBytes(double(R.undoBudgetAuto)).c_str());
      row("Undo memory limit", "Oldest steps are dropped when the undo history would use more system memory than this.", [&] {
        return ImGui::SliderFloat("##ug", &prefs.undoGB, 0.0f, 64.0f, prefs.undoGB <= 0 ? auto_ : "%.1f GB");
      });
      row("GPU", R.driverInfo.c_str(), [&] { ImGui::TextWrapped("%s", R.deviceName.c_str()); return false; });
      break;
    }
    case 5:
      row("Hold a tool key to use it temporarily",
          "Tap a tool key to switch tools. Hold it instead, use the tool and let go: you are back on the tool you had "
          "(e.g. hold E, erase, release E and keep drawing).",
          toggle(&prefs.springTools));
      row("Hold time", "A tool key held at least this long - or used on the canvas while held - counts as temporary.",
          [&] { return ImGui::SliderInt("##hm", &prefs.holdMs, 100, 800, "%d ms"); });
      drawShortcutsPage();
      break;
    default:
      ImGui::Text("Saraswati %s", SARASWATI_VERSION);
      ImGui::TextDisabled("A lag-free painting engine (Vulkan, SDL3, Dear ImGui).");
      ImGui::Spacing();
      ImGui::TextDisabled("Icons: Lucide (ISC licence). Font: Roboto (Apache 2.0).");
      ImGui::TextDisabled("Settings file: %s", settingsPath.empty() ? "(not saved in test runs)" : settingsPath.c_str());
      break;
  }
  ImGui::EndChild();
  if (changed) applyPrefs();
  ImGui::End();
}

void App::updateTitle() {
  std::string name = documentPath.empty() ? std::string("Untitled")
                                          : fs::path(reinterpret_cast<const char8_t*>(documentPath.c_str())).filename().string();
  std::string t = name + (modified() ? " *" : "") + "  -  Saraswati " SARASWATI_VERSION;
  if (t != lastTitle) { SDL_SetWindowTitle(window, t.c_str()); lastTitle = t; }
}

void App::drawStatusBar() {
  static const char* kToolNames[] = {"Brush", "Eyedropper", "Fill", "Gradient", "Line", "Rectangle", "Ellipse",
                                     "Rectangle select", "Ellipse select", "Lasso", "Magic wand", "Transform", "Hand", "Text"};
  ImGuiWindowFlags f = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
  if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), f)) {
    if (ImGui::BeginMenuBar()) {
      const char* toolName = kToolNames[std::clamp(int(tool), 0, 13)];
      if (tool == ToolId::Brush) ImGui::Text("%s%s", brushes[tipIndex].name.c_str(), eraserToggle ? " (eraser)" : "");
      else ImGui::TextUnformatted(toolName);
      ImGui::Separator();
      double dx, dy;
      screenToDoc(lastX, lastY, dx, dy);
      if (dx >= 0 && dy >= 0 && dx < R.docW && dy < R.docH) ImGui::Text("%5d, %5d px", int(dx), int(dy));
      else ImGui::TextDisabled("    -,     - px");
      ImGui::Separator();
      ImGui::Text("%.1f %%", view.zoom * 100);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wheel / + - zoom, Ctrl+0 fit, Ctrl+1 100 %%");
      ImGui::Text("%.0f deg%s", std::fmod(view.rotation * 180 / kPi + 36000, 360.0), view.flipX ? "  flipped" : "");
      ImGui::Separator();
      ImGui::Text("%u x %u", R.docW, R.docH);
      if (selActive) { ImGui::Separator(); ImGui::Text("Selection %d x %d", selX1 - selX0, selY1 - selY0); }
      if (xf.active) { ImGui::Separator(); ImGui::TextUnformatted("Transforming - Enter applies, Esc cancels"); }
      ImGui::Separator();
      if (saving) ImGui::TextUnformatted(saveVerifying ? "Checking the saved file..." : "Saving...");
      else if (flooding) ImGui::TextUnformatted("Working...");
      else if (SDL_GetTicksNS() < toastUntil) ImGui::TextUnformatted(toast.c_str());
      else if (R.cpuEmulation) ImGui::TextDisabled("CPU emulation");
      if (backupStage) { ImGui::Separator(); ImGui::TextDisabled("Backing up..."); }
      else if (!lastBackupText.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", lastBackupText.c_str()); }
      ImGui::EndMenuBar();
    }
  }
  ImGui::End();
}

void App::drawUI() {
  updateTitle();
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("New...", sk(Act::New))) requestAction(PA_New);
      drawFileDialogs();
      ImGui::Separator();
      if (ImGui::MenuItem("Quit")) requestAction(PA_Quit);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      bool canEdit = !R.stroking() && !xf.active;
      if (ImGui::MenuItem("Undo", sk(Act::Undo), false, R.canUndo() && canEdit)) R.undo();
      if (ImGui::MenuItem("Redo", sk(Act::Redo), false, R.canRedo() && canEdit)) R.redo();
      ImGui::Separator();
      bool ready = R.hasDocument() && !R.busy() && !xf.active && !clipping;
      if (ImGui::MenuItem("Cut", sk(Act::Cut), false, ready)) copySelection(true);
      if (ImGui::MenuItem("Copy", sk(Act::Copy), false, ready)) copySelection(false);
      if (ImGui::MenuItem("Paste", sk(Act::Paste), false, ready)) pasteClipboard();
      ImGui::Separator();
      if (ImGui::MenuItem("Clear", sk(Act::ClearSel), false, ready)) fillSelection(true);
      if (ImGui::MenuItem("Clear outside the selection", sk(Act::ClearOutside), false, ready && selActive)) clearOutsideSelection();
      if (ImGui::MenuItem("Fill", sk(Act::FillSel), false, ready)) fillSelection(false);
      ImGui::Separator();
      if (ImGui::BeginMenu("Tonal correction", ready)) {
        if (ImGui::MenuItem("Brightness / Contrast...")) openAdjust(1);
        if (ImGui::MenuItem("Hue / Saturation / Luminosity...")) openAdjust(2);
        if (ImGui::MenuItem("Posterize...")) openAdjust(4);
        if (ImGui::MenuItem("Threshold (black and white)...")) openAdjust(5);
        if (ImGui::MenuItem("Invert colours")) openAdjust(3);
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Convert brightness to opacity", nullptr, false, ready)) openAdjust(6);
      if (ImGui::MenuItem("Change colour to drawing colour", nullptr, false, ready)) openAdjust(7);
      ImGui::Separator();
      if (ImGui::BeginMenu("Transform", R.hasDocument() && !R.busy())) {
        if (ImGui::MenuItem("Free transform", sk(Act::ToolTransform))) setTool(ToolId::Transform);
        ImGui::Separator();
        if (ImGui::MenuItem("Flip horizontally")) transformLayer(0);
        if (ImGui::MenuItem("Flip vertically")) transformLayer(1);
        if (ImGui::MenuItem("Rotate 90 deg clockwise")) transformLayer(2);
        if (ImGui::MenuItem("Rotate 90 deg counter-clockwise")) transformLayer(3);
        if (ImGui::MenuItem("Rotate 180 deg")) transformLayer(4);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Preferences...", sk(Act::Prefs))) showPrefs = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Fit to window", sk(Act::ZoomFit))) fitView();
      if (ImGui::MenuItem("100 %", sk(Act::Zoom100))) view.zoom = 1;
      if (ImGui::MenuItem("Zoom in", sk(Act::ZoomIn))) zoomAt(canvasX + canvasW / 2, canvasY + canvasH / 2, 1.25);
      if (ImGui::MenuItem("Zoom out", sk(Act::ZoomOut))) zoomAt(canvasX + canvasW / 2, canvasY + canvasH / 2, 0.8);
      if (ImGui::MenuItem("Reset rotation", sk(Act::ResetRotation))) rotateView(-view.rotation);
      if (ImGui::MenuItem("Rotate left 15 deg", sk(Act::RotateLeft))) rotateView(-3.14159265358979323846 / 12);
      if (ImGui::MenuItem("Rotate right 15 deg", sk(Act::RotateRight))) rotateView(3.14159265358979323846 / 12);
      if (ImGui::MenuItem("Flip view horizontally", sk(Act::FlipView), view.flipX)) flipView();
      if (ImGui::MenuItem("Flip view vertically", sk(Act::FlipViewV))) flipViewV();
      ImGui::Separator();
      ImGui::MenuItem("Hide panels", sk(Act::HidePanels), &hideUI);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Selection")) {
      bool ok = R.hasDocument() && !R.busy() && !xf.active;
      if (ImGui::MenuItem("Select all", sk(Act::SelectAll), false, ok)) selectAll();
      if (ImGui::MenuItem("Deselect", sk(Act::Deselect), false, ok && selActive)) deselect();
      if (ImGui::MenuItem("Invert selection", sk(Act::InvertSel), false, ok)) invertSelection();
      ImGui::Separator();
      if (ImGui::MenuItem("Grow selection...", nullptr, false, ok && selActive)) { selGrowShrink = 1; growPopup = true; }
      if (ImGui::MenuItem("Shrink selection...", nullptr, false, ok && selActive)) { selGrowShrink = -1; growPopup = true; }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("Navigator", nullptr, &showNav);
      ImGui::MenuItem("Tool Group", nullptr, &showToolGroup);
      ImGui::MenuItem("Colour", nullptr, &showColor);
      ImGui::MenuItem("Tool Settings", nullptr, &showBrush);
      ImGui::MenuItem("Layers", nullptr, &showLayers);
      ImGui::MenuItem("Layer Properties", nullptr, &showLayerProps);
      ImGui::MenuItem("Performance", nullptr, &showPerf);
      ImGui::Separator();
      if (ImGui::MenuItem("Reset layout")) { resetLayout = true; showBrush = showColor = showLayers = showNav = showToolGroup = true; hideUI = false; }
      ImGui::EndMenu();
    }
    drawWindowButtons();
    ImGui::EndMainMenuBar();
  }
  drawStatusBar();
  // Dockspace: panels snap to the edges, resize, and combine into tab stacks; the central
  // node is see-through and is the canvas.
  ImGuiID dockId = ImGui::GetID("MainDock2");
  if (resetLayout || !ImGui::DockBuilderGetNode(dockId)) { buildDefaultLayout(dockId); resetLayout = false; }
  ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
  if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(dockId)) {
    float s = ImGui::GetIO().DisplayFramebufferScale.x;
    canvasX = c->Pos.x * s; canvasY = c->Pos.y * s; canvasW = c->Size.x * s; canvasH = c->Size.y * s;
  }
  if (!hideUI) {
    drawToolbar();
    if (showNav) drawNavigator(); else R.navWanted = false;
    if (showToolGroup) drawToolGroup();
    if (showColor) drawColorPanel();
    if (showBrush) drawBrushPanel();
    if (showLayerProps) drawLayerProperties();
    if (showLayers) drawLayerPanel();
    if (showPerf) drawPerfPanel();
  } else {
    R.navWanted = false;
    R.updateThumbnails(1);
  }
  drawNewDocDialog();
  drawUnsavedDialog();
  drawPrefs();
  if (!R.lastError.empty()) { error(R.lastError); R.lastError.clear(); }
  if (errorOpen) { ImGui::OpenPopup("Message"); errorOpen = false; }
  if (ImGui::BeginPopupModal("Message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(500);
    ImGui::TextUnformatted(errorMsg.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("OK", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  drawToolOverlay();
  drawSelectionBar();
  drawBubbleButtons();
  drawRestoreDialog();
  drawAdjustDialog();
  if (growPopup) { ImGui::OpenPopup("Grow / shrink selection"); growPopup = false; }
  if (ImGui::BeginPopupModal("Grow / shrink selection", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(selGrowShrink > 0 ? "Grow the selection by" : "Shrink the selection by");
    ImGui::SliderInt("##gpx", &selGrowPx, 1, 200, "%d px", ImGuiSliderFlags_Logarithmic);
    if (ImGui::Button("OK", ImVec2(100, 0))) { growSelection(selGrowShrink * selGrowPx); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  drawSizePreview();
  {
    float rgba[4];
    if (R.takePick(rgba) && rgba[3] > 0.01f)
      for (int k = 0; k < 3; ++k) color[k] = std::clamp(rgba[k] / rgba[3], 0.0f, 1.0f);
  }
  // brush outline at the cursor (or at the resize anchor while Ctrl+Alt+dragging the size)
  bool brushCursor = (tool == ToolId::Brush && !altDown) || tool == ToolId::Line || sizeDrag ||
                     ((tool == ToolId::Rect || tool == ToolId::Ellipse) && !shapeFilled);
  if (brushCursor && (sizeDrag || !ImGui::GetIO().WantCaptureMouse) && R.hasDocument() && drag == Drag::None) {
    float s = ImGui::GetIO().DisplayFramebufferScale.x;
    float r = float(brushes[tipIndex].size * 0.5 * view.zoom) / s;
    ImVec2 c = sizeDrag ? ImVec2(sizeDragX / s, sizeDragY / s) : ImVec2(lastX / s, lastY / s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(canvasX / s, canvasY / s), ImVec2((canvasX + canvasW) / s, (canvasY + canvasH) / s), true);
    dl->AddCircle(c, std::max(r, 1.5f) + 1, IM_COL32(255, 255, 255, 160), 0, 1.0f);
    dl->AddCircle(c, std::max(r, 1.5f), IM_COL32(0, 0, 0, 200), 0, 1.0f);
    if (sizeDrag) {
      char t[32];
      snprintf(t, sizeof t, "%.1f px", brushes[tipIndex].size);
      dl->AddText(ImVec2(c.x + 6, c.y + 6), IM_COL32(255, 255, 255, 255), t);
    }
    dl->PopClipRect();
  }
}

// ---------------------------------------------------------------------------
// Benchmark

void App::startBenchmark(uint32_t w, uint32_t h, float brush, Tip tip, int layers) {
  if (!newDocument(w, h, true)) return;
  for (int i = 1; i < layers; ++i) {
    std::string err;
    int li = R.addLayer(int(R.layers.size()), err, false);
    if (li < 0) { error("Benchmark: " + err); break; }
  }
  active = int(R.layers.size()) / 2;  // layers above and below the active one exercise both caches
  tipIndex = legacyTip(int(tip));
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
  SDL_Log("benchmark: %ux%u, brush %.0f px, %s, %d layers", w, h, brush, brushes[tipIndex].name.c_str(), bench.layers);
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
           bench.docW, bench.docH, bench.layers, R.maskR16 ? "R16" : "R32F", brushes[legacyTip(int(bench.tip))].name.c_str(), bench.brush,
           brushes[legacyTip(int(bench.tip))].spacing * 100, bench.samples.size(), bench.times.empty() ? 0 : bench.times.back(),
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
      tipIndex = legacyTip(tip);
      brushes[tipIndex].size = size;
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
    tipIndex = legacyTip(2);
    brushes[tipIndex].size = 120;
    color[0] = 1.0f; color[1] = 0.5f; color[2] = 0.0f;
    strokeBegin({300, 850, 1.0f}, false);
    for (int i = 1; i <= 60; ++i) strokeAdd({300 + 1000.0 * i / 60, 850 - 100 * std::sin(i / 60.0 * kPi), 1.0f});
    flushDabs(0);
  });
}

// --splinetest: an arc sampled like a zoomed-out 30k canvas (every sample snapped to a 20 px grid,
// i.e. one screen pixel at 5 % zoom), drawn without (left) and with (right) spline interpolation.
void App::buildSplineTest() {
  demo.push_back([this] { newDocument(2400, 1600, true); });
  for (int pass = 0; pass < 2; ++pass)
    demo.push_back([this, pass] {
      view.zoom = pass ? 0.05 : 1.0;  // pass 1 behaves like 5 % zoom (half-pixel = 10 doc px)
      tipIndex = brushIndex("Hard round");
      brushes[tipIndex].size = 50;
      color[0] = color[1] = color[2] = 0.15f;
      double ox = pass ? 1250 : 150;
      auto snap = [](double v) { return std::round(v / 20.0) * 20.0; };
      for (int k = 0; k <= 900; ++k) {  // dense like a 240 Hz pen, each sample snapped to the pixel grid
        double t = k / 900.0, a = 3.3 * t;
        PenSample p{snap(ox + 500 - 450 * std::cos(a) + 150 * t), snap(250 + 1100 * t + 80 * std::sin(a * 2)), 1.0f};
        if (k == 0) strokeBegin(p, false, pass == 1);
        else strokeAdd(p);
      }
      strokeEnd();
      fitView();
    });
}

// --brushtest: one stroke with every brush of the library (visual check of the brush engine)
void App::buildBrushTest() {
  demo.push_back([this] { newDocument(1800, 1300, true); });
  int n = int(brushes.size());
  int perCol = (n + 1) / 2;
  for (int i = 0; i < n; ++i) {
    demo.push_back([this, i, perCol] {
      tipIndex = i;
      BrushSettings& b = brushes[i];
      float sz = std::min(b.size, 44.0f);
      if (b.particles) sz = 60;
      float keep = b.size;
      b.size = sz;
      double x0 = (i / perCol) * 900 + 60, y = 45 + (i % perCol) * (1300.0 - 60) / perCol;
      color[0] = 0.08f; color[1] = 0.1f; color[2] = 0.16f;
      if (b.eraser) {  // show erasers on a grey band
        eraserToggle = false;
        tipIndex = legacyTip(0);
        float hs = brushes[tipIndex].size;
        brushes[tipIndex].size = 40;
        color[0] = color[1] = color[2] = 0.6f;
        strokeBegin({x0, y, 1.0f}, false);
        strokeAdd({x0 + 780, y, 1.0f});
        strokeEnd();
        brushes[tipIndex].size = hs;
        tipIndex = i;
        pendingBrushStroke = true;
        b.size = keep;
        return;
      }
      strokeBegin({x0, y, 0.1f}, false);
      for (int k = 1; k <= 120; ++k) {
        double t = k / 120.0;
        float pr = float(t < 0.5 ? 0.1 + 1.8 * t : 1.0 - 1.4 * (t - 0.5));
        strokeAdd({x0 + 780 * t, y + 16 * std::sin(t * 12), pr});
      }
      strokeEnd();
      b.size = keep;
    });
    demo.push_back([this, i, perCol] {  // erasers: second pass over the grey band
      if (!pendingBrushStroke) return;
      pendingBrushStroke = false;
      tipIndex = i;
      BrushSettings& b = brushes[i];
      float keep = b.size;
      b.size = 34;
      double x0 = (i / perCol) * 900 + 60, y = 45 + (i % perCol) * (1300.0 - 60) / perCol;
      strokeBegin({x0 + 60, y, 1.0f}, false);
      strokeAdd({x0 + 720, y, 1.0f});
      strokeEnd();
      b.size = keep;
    });
  }
}

void App::tickDemo() {
  if (demoDone || demo.empty()) return;
  // wait for the GPU to finish the previous step (the last step leaves a stroke open on purpose)
  if (demoStep > 0 && ((R.busy() && !hoverPreviewOn && !textPreviewOn) || flooding || clipping || hoverDirty || !fonts.ready() || textEdit.dirty || backupStage != 0) && !(demoStep == demo.size())) return;
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
  if (flooding) floodJob.wait();
  backupShutdown();
  if (clipping) clipJob.wait();
  R.finishAsyncRead(clipRead);
  if (floodRead == floodCache) floodRead.reset();  // same read-back: release it once
  R.finishAsyncRead(floodRead);
  R.finishAsyncRead(floodCache);
  if (xf.active) R.destroyFloating(xf.fl);
  R.destroyFloating(iconAtlas);
  R.destroyFloating(previewAtlas);
  saveSettings();
}

// Settings: brushes, colours, current tool and last folder, as "key value" lines.
void App::loadSettings() {
  if (settingsPath.empty()) return;
  FILE* f = fopen(settingsPath.c_str(), "r");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    char key[64] = {};
    int n = 0;
    if (sscanf(line, "%63s %n", key, &n) != 1) continue;
    const char* v = line + n;
    std::string k = key;
    if (k == "tool") { int t = atoi(v); if (t >= 0 && t < int(ToolId::Count) && ToolId(t) != ToolId::Transform) tool = ToolId(t); }
    else if (k == "color") sscanf(v, "%f %f %f", &color[0], &color[1], &color[2]);
    else if (k == "bg") sscanf(v, "%f %f %f", &bgColor[0], &bgColor[1], &bgColor[2]);
    else if (k == "folder") { lastFolder = v; while (!lastFolder.empty() && (lastFolder.back() == '\n' || lastFolder.back() == '\r')) lastFolder.pop_back(); }
    else if (k == "fill") sscanf(v, "%f %f", &fillTol, &wandTol);
    else if (k == "perf") showPerf = atoi(v) != 0;
    else if (k == "prefs") {
      int a[9] = {1, 1, 1, 0, 1, 1, 1, 2, 50};
      float wz = 1.2f, ug = 0;
      if (sscanf(v, "%d %d %d %d %d %d %d %d %d %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &wz, &ug) >= 9) {
        prefs.startMaximized = a[0]; prefs.perfAtStart = a[1]; prefs.confirmUnsaved = a[2]; prefs.uiScalePct = a[3];
        prefs.smoothStrokes = a[4]; prefs.mousePixelFix = a[5]; prefs.barrelLower = a[6]; prefs.barrelUpper = a[7];
        prefs.undoSteps = std::clamp(a[8], 5, 500); prefs.wheelZoom = std::clamp(wz, 1.05f, 2.0f); prefs.undoGB = std::max(0.0f, ug);
      }
    }
    else if (k == "brush2") {
      // fields ... | group | name   (built-in brushes are matched by name, others are custom)
      BrushSettings b;
      int follow = 0, build = 0, ps = 1, po = 0, er = 0, n2 = 0;
      if (sscanf(v, "%f %f %f %f %f %f %f %d %f %f %f %f %f %d %d %f %d %f %d %f %d %n", &b.size, &b.opacity, &b.flow,
                 &b.spacing, &b.hardness, &b.roundness, &b.angle, &follow, &b.texStrength, &b.texScale, &b.scatter,
                 &b.sizeJitter, &b.flowJitter, &build, &b.particles, &b.particleSize, &ps, &b.minSize, &po, &b.gamma, &er,
                 &n2) >= 21) {
        b.followStroke = follow; b.buildUp = build; b.pressureSize = ps; b.pressureOpacity = po; b.eraser = er;
        std::string rest = v + n2;
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
        size_t bar = rest.find('|', 1);
        if (rest.size() > 1 && rest[0] == '|' && bar != std::string::npos) {
          b.group = rest.substr(1, bar - 1);
          b.name = rest.substr(bar + 1);
          int found = -1;
          for (size_t i = 0; i < brushes.size(); ++i) if (brushes[i].name == b.name) found = int(i);
          if (found >= 0) { b.builtIn = brushes[found].builtIn; brushes[found] = b; }
          else { b.builtIn = false; brushes.push_back(b); }
        }
      }
    }
    else if (k == "fps") prefs.fpsLimit = std::clamp(atoi(v), 0, 1000);
    else if (k == "backup") {
      int on = 1, mins = 5, keep = 20;
      if (sscanf(v, "%d %d %d", &on, &mins, &keep) == 3) {
        prefs.backupOn = on != 0; prefs.backupMinutes = std::clamp(mins, 1, 60); prefs.backupKeep = std::clamp(keep, 1, 200);
      }
    }
    else if (k == "hold") {
      int sp = 1, ms = 250;
      if (sscanf(v, "%d %d", &sp, &ms) == 2) { prefs.springTools = sp != 0; prefs.holdMs = std::clamp(ms, 100, 800); }
    }
    else if (k == "key") {  // key <action id> <key> <mods> <key2> <mods2>
      char id[64];
      unsigned k1 = 0, k2 = 0;
      int m1 = 0, m2 = 0;
      if (sscanf(v, "%63s %u %d %u %d", id, &k1, &m1, &k2, &m2) == 5)
        for (int a = 0; a < kActCount; ++a)
          if (std::strcmp(actionInfo(Act(a)).id, id) == 0) {
            keys[a][0] = KeyCombo{SDL_Keycode(k1), m1};
            keys[a][1] = KeyCombo{SDL_Keycode(k2), m2};
          }
    }
    else if (k == "brushsel") {
      std::string name = v;
      while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
      for (size_t i = 0; i < brushes.size(); ++i) if (brushes[i].name == name) tipIndex = int(i);
    }
  }
  fclose(f);
}

void App::saveSettings() {
  if (settingsPath.empty()) return;
  FILE* f = fopen(settingsPath.c_str(), "w");
  if (!f) return;
  fprintf(f, "tool %d\ncolor %f %f %f\nbg %f %f %f\nfill %f %f\nperf %d\nbrushsel %s\n",
          int(tool == ToolId::Transform ? ToolId::Brush : tool), color[0], color[1], color[2], bgColor[0], bgColor[1],
          bgColor[2], fillTol, wandTol, showPerf ? 1 : 0, brushes[tipIndex].name.c_str());
  fprintf(f, "prefs %d %d %d %d %d %d %d %d %d %f %f\n", prefs.startMaximized, prefs.perfAtStart, prefs.confirmUnsaved,
          prefs.uiScalePct, prefs.smoothStrokes, prefs.mousePixelFix, prefs.barrelLower, prefs.barrelUpper, prefs.undoSteps,
          prefs.wheelZoom, prefs.undoGB);
  for (const BrushSettings& b : brushes)
    fprintf(f, "brush2 %f %f %f %f %f %f %f %d %f %f %f %f %f %d %d %f %d %f %d %f %d |%s|%s\n", b.size, b.opacity, b.flow,
            b.spacing, b.hardness, b.roundness, b.angle, b.followStroke ? 1 : 0, b.texStrength, b.texScale, b.scatter,
            b.sizeJitter, b.flowJitter, b.buildUp ? 1 : 0, b.particles, b.particleSize, b.pressureSize ? 1 : 0, b.minSize,
            b.pressureOpacity ? 1 : 0, b.gamma, b.eraser ? 1 : 0, b.group.c_str(), b.name.c_str());
  fprintf(f, "hold %d %d\n", prefs.springTools ? 1 : 0, prefs.holdMs);
  fprintf(f, "backup %d %d %d\n", prefs.backupOn ? 1 : 0, prefs.backupMinutes, prefs.backupKeep);
  fprintf(f, "fps %d\n", prefs.fpsLimit);
  for (int a = 0; a < kActCount; ++a)  // only changed shortcuts are stored, so new defaults reach old settings files
    if (!(keys[a][0] == actionInfo(Act(a)).def[0] && keys[a][1] == actionInfo(Act(a)).def[1]))
      fprintf(f, "key %s %u %d %u %d\n", actionInfo(Act(a)).id, unsigned(keys[a][0].key), keys[a][0].mods,
              unsigned(keys[a][1].key), keys[a][1].mods);
  if (!lastFolder.empty()) fprintf(f, "folder %s\n", lastFolder.c_str());
  fclose(f);
}

void App::showDialog(int kind) {
  struct Ctx { App* app; int kind; };
  auto* ctx = new Ctx{this, kind};
  auto cb = [](void* ud, const char* const* list, int) {
    Ctx* c = static_cast<Ctx*>(ud);
    if (list && !list[0] && c->kind == DlgSave) {  // save dialog cancelled: drop a pending quit/new/open
      std::lock_guard<std::mutex> lock(c->app->dialogMutex);
      c->app->dialogResults.push_back({0, ""});
    }
    if (list && list[0]) {
      std::lock_guard<std::mutex> lock(c->app->dialogMutex);
      c->app->dialogResults.push_back({c->kind, list[0]});
    } else if (!list) {
      std::lock_guard<std::mutex> lock(c->app->dialogMutex);
      c->app->dialogResults.push_back({-1, SDL_GetError()});
    }
    delete c;
  };
  std::string folder = lastFolder.empty() ? (fs::path(userData) / "documents").string() : lastFolder;
  if (kind == DlgSave) {
    std::string def = documentPath.empty() ? (fs::path(folder) / "Untitled.psd").string() : documentPath;
    SDL_ShowSaveFileDialog(cb, ctx, window, kSaveFilters, 1, def.c_str());
  } else if (kind == DlgExport) {
    static const SDL_DialogFileFilter f[] = {{"PNG image", "png"}, {"JPEG image", "jpg;jpeg"}};
    SDL_ShowSaveFileDialog(cb, ctx, window, f, 2, (fs::path(folder) / "Untitled.png").string().c_str());
  } else if (kind == DlgOpen) {
    std::string def = folder;
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
    if (kind == 0) { continueAfterSave = false; pendingAction = PA_None; continue; }
    if (kind == DlgOpen) openFile(path);
    else if (kind == DlgImport) importAsLayer(path);
    else if (kind == DlgSave) {
      std::string p = path;
      if (!isPsdPath(p)) p += ".psd";
      saveFile(p);
    } else if (kind == DlgExport) {
      std::string p = path, e = fs::path(reinterpret_cast<const char8_t*>(p.c_str())).extension().string();
      for (auto& c : e) c = char(std::tolower((unsigned char)c));
      if (e != ".png" && e != ".jpg" && e != ".jpeg") p += ".png";
      exportFlat(p);
    } else {
      error("File dialog failed: " + path);
      continueAfterSave = false;
      pendingAction = PA_None;
    }
  }
  std::lock_guard<std::mutex> lock(saveMutex);
  if (saveDone) {
    saveDone = false;
    if (saveThread.joinable()) saveThread.join();
    if (!saveMessage.empty()) {
      documentPath = pathBeforeSave;  // the document is still where it was
      error(saveMessage);
      continueAfterSave = false;
      pendingAction = PA_None;
    } else if (saveOk) {
      savedRevision = revisionAtSave;
      toast = "Saved " + fs::path(reinterpret_cast<const char8_t*>(documentPath.c_str())).filename().string();
      toastUntil = SDL_GetTicksNS() + 3000000000ull;
      if (continueAfterSave) performAction();
    }
  }
}

void App::drawFileDialogs() {
  bool busy = R.stroking() || saving;
  if (ImGui::MenuItem("Open...", sk(Act::Open), false, !busy)) requestAction(PA_OpenDialog);
  if (ImGui::MenuItem("Restore from backup...", nullptr, false, !busy && !userData.empty())) showRestore = true;
  if (ImGui::MenuItem("Import image as layer...", sk(Act::Import), false, !busy && R.hasDocument())) showDialog(DlgImport);
  ImGui::Separator();
  if (ImGui::MenuItem("Save", sk(Act::Save), false, !busy && R.hasDocument())) {
    if (documentPath.empty()) showDialog(DlgSave); else saveFile(documentPath);
  }
  if (ImGui::MenuItem("Save as PSD...", sk(Act::SaveAs), false, !busy && R.hasDocument())) showDialog(DlgSave);
  if (ImGui::MenuItem("Export PNG / JPEG...", sk(Act::Export), false, !busy && R.hasDocument())) showDialog(DlgExport);
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
        idx = R.addLayer(int(i), err, false);
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
    savedRevision = R.revision;
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
    savedRevision = R.revision;
  }
  lastFolder = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).parent_path().string();
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
  if (!R.hasDocument()) return;
  if (saving) { pendingSavePath = path; return; }  // saved again as soon as the running save is done
  // everything that is not in the layers yet goes in first (nothing is silently left out)
  if (textEdit.active) commitText();
  if (xf.active) applyTransform();
  if (hoverPreviewOn || previewing) cancelPreviews();
  if (engine.active()) strokeEnd();
  if (R.stroking()) { pendingSavePath = path; return; }  // the stroke lands in the layer next frame: save then
  auto doc = std::make_shared<DocFile>();
  auto merged = std::make_shared<ImageRGBA>();
  doc->w = R.docW;
  doc->h = R.docH;
  std::string err;
  try {
    // GPU readback of only the painted area of each layer; encoding, checking and disk I/O run on a worker thread
    R.waitIdle();
    for (size_t i = 0; i < R.layers.size(); ++i) {
      DocLayer L;
      const Layer& src = R.layers[i];
      L.name = src.name;
      L.visible = src.visible;
      L.opacity = src.opacity;
      L.mode = src.mode;
      int x0 = std::max(src.bx0, 0), y0 = std::max(src.by0, 0);
      int x1 = std::min(src.bx1, int(R.docW)), y1 = std::min(src.by1, int(R.docH));
      if (x0 < x1 && y0 < y1) {
        L.x = x0; L.y = y0;
        L.w = uint32_t(x1 - x0); L.h = uint32_t(y1 - y0);
        if (!R.readLayerRegion(int(i), x0, y0, L.w, L.h, L.rgba, err)) { error("Save failed: " + err); return; }
      }
      doc->layers.push_back(std::move(L));
    }
    merged->w = R.docW;
    merged->h = R.docH;
    if (!R.readMergedPixels(merged->rgba, err)) { error("Save failed: " + err); return; }
  } catch (const std::bad_alloc&) {
    error("Not enough memory to save this document right now.\nClose other programs and try again - nothing was written.");
    return;
  }
  if (saveThread.joinable()) saveThread.join();
  saving = true;
  saveVerifying = false;
  saveProgress = 0;
  pathBeforeSave = documentPath;
  documentPath = path;
  revisionAtSave = R.revision;
  lastFolder = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).parent_path().string();
  saveThread = std::thread([this, doc, merged, path] {
    std::string e;
    bool ok = false;
    std::string tmp = path + ".saving";
    try {
      for (auto& L : doc->layers) unpremultiply(L.rgba);
      unpremultiply(merged->rgba);
      float prog = 0;
      ok = writePsdFile(tmp, *doc, *merged, e, &prog);
      if (ok) {
        // read the file back and compare every layer before it replaces the old one
        std::vector<PsdCheck> checks = psdChecks(*doc);
        uint32_t w = doc->w, h = doc->h;
        doc->layers.clear();
        doc->layers.shrink_to_fit();
        merged->rgba.clear();
        merged->rgba.shrink_to_fit();
        saveVerifying = true;
        std::string verr;
        if (!verifyPsd(tmp, w, h, checks, verr)) {
          ok = false;
          e = "Saving was stopped - " + verr + ".\nThe previous file was not touched.";
        } else {
          ok = commitFileReplace(tmp, path, e);
        }
      }
    } catch (const std::bad_alloc&) {
      ok = false;
      e = "Not enough memory to finish saving. The previous file was not touched.";
    } catch (const std::exception& ex) {
      ok = false;
      e = std::string("Unexpected error while saving: ") + ex.what() + ". The previous file was not touched.";
    }
    if (!ok) {
      std::error_code ec;
      if (e.find("was kept as") == std::string::npos) fs::remove(fs::path(reinterpret_cast<const char8_t*>(tmp.c_str())), ec);
    }
    std::lock_guard<std::mutex> lock(saveMutex);
    saveMessage = ok ? std::string() : "Save failed: " + e;
    saveOk = ok;
    if (ok) SDL_Log("saved %s (verified)", path.c_str());
    else SDL_Log("save failed: %s", e.c_str());
    saveDone = true;
    saving = false;
  });
}

void App::exportFlat(const std::string& path) {
  if (!R.hasDocument() || R.stroking() || saving) return;
  R.waitIdle();
  auto merged = std::make_shared<ImageRGBA>();
  merged->w = R.docW;
  merged->h = R.docH;
  std::string err;
  if (!R.readMergedPixels(merged->rgba, err)) { error("Export failed: " + err); return; }
  if (saveThread.joinable()) saveThread.join();
  saving = true;
  revisionAtSave = savedRevision;  // exporting does not count as saving the document
  saveThread = std::thread([this, merged, path] {
    unpremultiply(merged->rgba);
    std::string e;
    bool ok = exportImage(path, *merged, e);
    std::lock_guard<std::mutex> lock(saveMutex);
    saveMessage = ok ? std::string() : "Export failed: " + e;
    saveOk = false;
    if (ok) { toast = "Exported " + fs::path(reinterpret_cast<const char8_t*>(path.c_str())).filename().string(); toastUntil = SDL_GetTicksNS() + 3000000000ull; }
    saveDone = true;
    saving = false;
  });
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
    fonts.startScan("");
  } else {
    std::error_code ec;
    fs::create_directories(fs::path(userData) / "settings", ec);
    iniPath = (fs::path(userData) / "settings" / "layout.ini").string();
    io.IniFilename = iniPath.c_str();
    settingsPath = (fs::path(userData) / "settings" / "settings.txt").string();
    fonts.startScan((fs::path(userData) / "settings" / "fontcache.txt").string());
    backupStartup();
    loadSettings();
    applyPrefs();
    if (prefs.perfAtStart) showPerf = true;
    if (!prefs.startMaximized && !opt.windowSet) SDL_RestoreWindow(window);
  }
  R.onResize();
  loadIcons();
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
  else if (opt.toolTest) { opt.demo = true; buildToolTest(); }
  else if (opt.brushTest) { opt.demo = true; buildBrushTest(); }
  else if (opt.splineTest) { opt.demo = true; buildSplineTest(); }
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
    bool animating = bench.running || saving || saveDone || !pendingSavePath.empty() || clipping || backupStage != 0 ||
                     flooding || hoverDirty || textEdit.active || (!opt.save.empty() && !savedForTest) || (!demo.empty() && !demoDone) || framesToRender > 0 || R.busy() ||
                     (wantShot && !screenshotTaken);
    SDL_Event e;
    if (!animating) {
      // idle: sleep until input (0 % CPU). With the Performance panel open, refresh it twice a second.
      bool perfVisible = showPerf && !hideUI;
      if (!SDL_WaitEventTimeout(&e, perfVisible ? 500 : 250)) {
        if (!perfVisible) continue;
      } else {
        handleEvent(e);
      }
    }
    while (SDL_PollEvent(&e)) handleEvent(e);
    if (!running) break;
    processDialogResults();
    pollFlood();
    pollClipboard();
    if (textEdit.active) {
      if (textEdit.dirty || !(textEdit.shownStyle == textStyle) || !(textEdit.shownBubble == bubbleStyle) || textEdit.shownColor[0] != color[0] ||
          textEdit.shownColor[1] != color[1] || textEdit.shownColor[2] != color[2])
        updateTextPreview();
      if (!ImGui::GetIO().WantTextInput && !SDL_TextInputActive(window)) SDL_StartTextInput(window);
    }
    if (previewing && previewDirty) updateShapePreview();
    if (hoverDirty && !flooding && hoverX >= 0) {
      hoverDirty = false;
      startFlood(hoverX + 0.5, hoverY + 0.5, tool == ToolId::Fill, true);
    }
    // the previewed fill follows colour / opacity changes
    if (hoverPreviewOn && tool == ToolId::Fill &&
        (hoverColor[0] != color[0] || hoverColor[1] != color[1] || hoverColor[2] != color[2] ||
         hoverColor[3] != brushes[tipIndex].opacity))
      showHoverPreview();
    // the picture changed under a hover preview (e.g. undo): recompute it
    if (hoverPreviewOn && (hoverRevision != R.revision || hoverDoc != R.docSerial) && !R.stroking()) {
      hoverPreviewOn = false;
      hoverDirty = true;
    }
    tickBenchmark();
    tickToolRestore();
    tickBackup();
    if (!pendingSavePath.empty() && !saving && !R.stroking() && !engine.active()) {
      std::string p = std::move(pendingSavePath);
      pendingSavePath.clear();
      saveFile(p);
    }
    tickDemo();
    if (!opt.save.empty() && !savedForTest && (!opt.demo || demoDone) && engine.active() && (!wantShot || screenshotTaken))
      strokeEnd();  // the demo leaves its last stroke open; finish it before saving
    if (!opt.save.empty() && !savedForTest && (!opt.demo || demoDone) && (!R.busy() || hoverPreviewOn || textPreviewOn) && !bench.running &&
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
    // frame-rate limit: keep taking input while waiting for the next frame slot (never delays input,
    // only the redraw; benchmark and test runs are not limited)
    if (prefs.fpsLimit > 0 && !bench.running && !opt.demo && lastPresentNs) {
      uint64_t minNs = 1000000000ull / uint64_t(prefs.fpsLimit);
      while (SDL_GetTicksNS() - lastPresentNs < minNs) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) handleEvent(ev);
        uint64_t left = minNs - std::min(minNs, SDL_GetTicksNS() - lastPresentNs);
        if (left) SDL_DelayPrecise(std::min<uint64_t>(left, 1000000ull));
      }
    }
    lastPresentNs = SDL_GetTicksNS();
    bool shotNow = false;
    if (wantShot && !screenshotTaken) {
      bool ready = opt.demo ? (demoDone && framesRendered > 2)
                 : opt.benchmark ? (!bench.running && !bench.report.empty())
                 : framesRendered >= opt.frames;
      if (ready) { p.screenshotPath = opt.screenshot; shotNow = true; }
    }
    uint64_t inputNs = pendingInputNs;
    uint64_t tUi = SDL_GetTicksNS();
    bool rendered = R.renderFrame(p);
    uint64_t tEnd = SDL_GetTicksNS();
    // Frame-spike log (real GPUs only): anything over 50 ms of CPU time gets a breakdown line.
    if (rendered && !R.cpuEmulation && framesRendered > 30 && !bench.running) {
      double total = (tEnd - t) * 1e-6;
      if (total > 50.0) {
        const FrameCpuStats& c = R.lastCpu;
        char line[512];
        std::time_t tt = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%H:%M:%S", std::localtime(&tt));
        snprintf(line, sizeof line,
                 "%s  frame %.1f ms: events+UI %.1f, render %.1f (fence %.1f, acquire %.1f, record %.1f [undo copy %.1f, "
                 "thumbnail %.1f], submit %.1f, present %.1f), dabs %u, undo steps %zu\n",
                 when, total, (tUi - t) * 1e-6, (tEnd - tUi) * 1e-6, c.fenceMs, c.acquireMs, c.recordMs, c.undoMs, c.thumbMs,
                 c.submitMs, c.presentMs, R.lastFrameDabs, R.undoSteps());
        std::error_code ec;
        fs::create_directories(fs::path(userData) / "logs", ec);
        if (FILE* f = fopen((fs::path(userData) / "logs" / "frame-spikes.txt").string().c_str(), "a")) {
          fputs(line, f);
          fclose(f);
        }
      }
    }
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
    if (opt.exitAfter && (!wantShot || screenshotTaken) && !saving && !saveDone && pendingSavePath.empty() &&
        (opt.save.empty() || savedForTest)) {
      if ((opt.benchmark && !bench.running && !bench.report.empty()) || (opt.demo && demoDone) || !opt.save.empty())
        running = false;
    }
  }
  R.waitIdle();
  return 0;
}
