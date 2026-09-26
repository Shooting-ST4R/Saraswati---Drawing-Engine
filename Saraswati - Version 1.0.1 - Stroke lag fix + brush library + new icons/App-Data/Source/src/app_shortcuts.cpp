// Keyboard shortcuts: lookup, actions, spring-loaded (hold) tools and the Preferences page.
#include "app.h"
#include <algorithm>
#include <cstring>

static const ToolId kToolOfAct[] = {ToolId::Brush,  ToolId::Brush,      ToolId::Eyedropper, ToolId::Fill,    ToolId::Gradient,
                                    ToolId::Line,   ToolId::Rect,       ToolId::Ellipse,    ToolId::SelRect, ToolId::SelEllipse,
                                    ToolId::Lasso,  ToolId::Wand,       ToolId::Transform,  ToolId::Hand};

void App::resetShortcuts() {
  for (int a = 0; a < kActCount; ++a)
    for (int s = 0; s < 2; ++s) keys[a][s] = actionInfo(Act(a)).def[s];
}

int App::findAction(const KeyCombo& c) const {
  if (c.empty()) return -1;
  for (int a = 0; a < kActCount; ++a)
    if (keys[a][0] == c || keys[a][1] == c) return a;
  return -1;
}

std::string App::shortcutLabel(Act a) const {
  const KeyCombo* k = keys[int(a)];
  std::string s = comboLabel(k[0]);
  if (!k[1].empty()) s += (s.empty() ? "" : ", ") + comboLabel(k[1]);
  return s;
}

const char* App::sk(Act a) {
  std::string& s = skBuf[int(a)];
  s = comboLabel(keys[int(a)][0]);
  return s.empty() ? nullptr : s.c_str();
}

bool App::toolActActive(Act a) const {
  ToolId t = kToolOfAct[int(a)];
  return tool == t && (t != ToolId::Brush || (eraserToggle || brushes[tipIndex].eraser) == (a == Act::ToolEraser));
}

void App::switchToolState(ToolId t, bool eraser) {
  auto valid = [&](int i) { return i >= 0 && i < int(brushes.size()); };
  if (tool == ToolId::Brush) (eraserToggle ? eraseTip : paintTip) = tipIndex;  // remember the brush of each mode
  if (t == ToolId::Brush) {
    if (eraser && !valid(eraseTip))
      for (int i = 0; i < int(brushes.size()); ++i)
        if (brushes[i].eraser) { eraseTip = i; break; }
    int want = eraser ? eraseTip : paintTip;
    if (valid(want)) tipIndex = want;
    eraserToggle = eraser;
  } else if (t != tool) {
    eraserToggle = eraser;  // another tool starts in paint mode (its settings can still erase)
  }
  setTool(t);
}

void App::selectToolAct(Act a) {
  ToolId t = kToolOfAct[int(a)];
  switchToolState(t, a == Act::ToolEraser);
}

void App::releaseToolHold(bool forceRestore) {
  if (!toolHold.active) return;
  toolHold.active = false;
  uint64_t held = SDL_GetTicksNS() - toolHold.downNs;
  bool temporary = forceRestore || toolHold.used || held >= uint64_t(prefs.holdMs) * 1000000ull;
  if (!prefs.springTools || !temporary) return;  // a tap: the new tool stays
  if (tool == toolHold.prevTool && eraserToggle == toolHold.prevEraser) return;
  restoreTool = toolHold.prevTool;
  restoreEraser = toolHold.prevEraser;
  restorePending = true;
  tickToolRestore();
}

// go back to the previous tool once the stroke / drag made with the temporary tool has finished
void App::tickToolRestore() {
  if (!restorePending || engine.active() || toolDrag) return;
  restorePending = false;
  switchToolState(restoreTool, restoreEraser);
}

void App::releaseAllKeys() {
  spaceDown = rotateDown = false;
  panKeyHeld = rotateKeyHeld = 0;
  shiftDown = ctrlDown = altDown = false;
  if (toolHold.active) releaseToolHold(false);
}

void App::runAction(Act a, SDL_Keycode key, bool repeat) {
  bool busyStroke = engine.active() || toolDrag;
  auto center = [&](double f) { zoomAt(canvasX + canvasW / 2, canvasY + canvasH / 2, f); };
  if (isToolAct(a)) {
    if (repeat || busyStroke) return;
    if (toolHold.active) {
      // another tool key while holding one: releasing it still returns to the original tool
      toolHold.key = key;
      toolHold.downNs = SDL_GetTicksNS();
      toolHold.used = false;
    } else if (prefs.springTools && !ctrlDown) {
      toolHold.active = true;
      toolHold.used = false;
      toolHold.key = key;
      toolHold.prevTool = tool;
      toolHold.prevEraser = eraserToggle;
      toolHold.downNs = SDL_GetTicksNS();
    }
    restorePending = false;
    selectToolAct(a);
    return;
  }
  if (a >= Act::Brush1 && a <= Act::Brush9) {
    if (repeat || busyStroke) return;
    int i = int(a) - int(Act::Brush1);
    if (i < int(brushes.size())) {
      tipIndex = i;
      eraserToggle = false;
      setTool(ToolId::Brush);
    }
    return;
  }
  bool hasDoc = R.hasDocument();
  switch (a) {
    case Act::PanHold: spaceDown = true; panKeyHeld = key; break;
    case Act::RotateHold: rotateDown = true; rotateKeyHeld = key; break;
    case Act::Undo:
      if (busyStroke) break;
      if (hoverPreviewOn) cancelPreviews();
      if (xf.active) cancelTransform();  // undo while transforming = cancel the transform
      else R.undo();
      break;
    case Act::Redo:
      if (busyStroke || xf.active) break;
      if (hoverPreviewOn) cancelPreviews();
      R.redo();
      break;
    case Act::New: if (!repeat) requestAction(PA_New); break;
    case Act::Open: if (!repeat && !saving) requestAction(PA_OpenDialog); break;
    case Act::Save:
      if (!repeat && !saving && hasDoc) {
        if (documentPath.empty()) showDialog(DlgSave); else saveFile(documentPath);
      }
      break;
    case Act::SaveAs: if (!repeat && !saving && hasDoc) showDialog(DlgSave); break;
    case Act::Import: if (!repeat && !saving && hasDoc) showDialog(DlgImport); break;
    case Act::Export: if (!repeat && !saving && hasDoc) showDialog(DlgExport); break;
    case Act::Prefs: if (!repeat) showPrefs = !showPrefs; break;
    case Act::SelectAll: case Act::Deselect: case Act::InvertSel: case Act::ClearSel: case Act::FillSel:
      if (repeat || busyStroke) break;
      if (hoverPreviewOn) cancelPreviews();
      if (a == Act::SelectAll) selectAll();
      else if (a == Act::Deselect) deselect();
      else if (a == Act::InvertSel) invertSelection();
      else if (!xf.active) fillSelection(a == Act::ClearSel);
      break;
    case Act::Cut: if (!repeat && !busyStroke) copySelection(true); break;
    case Act::Copy: if (!repeat && !busyStroke) copySelection(false); break;
    case Act::Paste: if (!repeat && !busyStroke) pasteClipboard(); break;
    case Act::ClearOutside: if (!repeat && !busyStroke) clearOutsideSelection(); break;
    case Act::SwapColors: if (!repeat) for (int c = 0; c < 3; ++c) std::swap(color[c], bgColor[c]); break;
    case Act::BrushSmaller: brushes[tipIndex].size = std::max(1.0f, brushes[tipIndex].size / 1.15f); break;
    case Act::BrushBigger: brushes[tipIndex].size = std::min(5000.0f, brushes[tipIndex].size * 1.15f); break;
    case Act::ZoomIn: center(1.25); break;
    case Act::ZoomOut: center(0.8); break;
    case Act::ZoomFit: fitView(); break;
    case Act::Zoom100: view.zoom = 1; break;
    case Act::ResetRotation: rotateView(-view.rotation); break;
    case Act::HidePanels: if (!repeat) hideUI = !hideUI; break;
    case Act::FlipView: if (!repeat) flipView(); break;
    case Act::RotateLeft: rotateView(-3.14159265358979323846 / 12); break;
    case Act::RotateRight: rotateView(3.14159265358979323846 / 12); break;
    default: break;
  }
}

// Preferences: the next key pressed becomes the binding being edited.
bool App::captureKey(const SDL_KeyboardEvent& k) {
  if (isModifierKey(k.key)) return true;  // wait for the key that goes with Ctrl / Shift / Alt
  KeyCombo c{k.key, modsFromSdl(k.mod)};
  if (k.key == SDLK_ESCAPE && c.mods == 0) {  // Esc cancels (it stays the fixed cancel key)
    captureAct = -1;
    captureMsg.clear();
    return true;
  }
  captureMsg.clear();
  for (int a = 0; a < kActCount; ++a)
    for (int s = 0; s < 2; ++s)
      if ((a != captureAct || s != captureSlot) && keys[a][s] == c) {
        keys[a][s] = KeyCombo{};
        captureMsg = comboLabel(c) + " was used by \"" + actionInfo(Act(a)).name + "\" - removed there.";
      }
  keys[captureAct][captureSlot] = c;
  captureAct = -1;
  return true;
}

void App::drawShortcutsPage() {
  float sc = ImGui::GetStyle().FontScaleDpi;
  static char filter[64] = "";
  ImGui::SetNextItemWidth(260 * sc);
  ImGui::InputTextWithHint("##filter", "Search actions or keys", filter, sizeof filter);
  ImGui::SameLine();
  if (ImGui::Button("Reset all to defaults")) { resetShortcuts(); captureMsg.clear(); captureAct = -1; }
  if (!captureMsg.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1));
    ImGui::TextWrapped("%s", captureMsg.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::TextDisabled("Click a key to change it, then press the new combination (Esc cancels).");
  auto lower = [](std::string s) { for (char& ch : s) ch = char(std::tolower((unsigned char)ch)); return s; };
  std::string f = lower(filter);
  const char* lastGroup = nullptr;
  bool tableOpen = false;
  auto endTable = [&] { if (tableOpen) ImGui::EndTable(); tableOpen = false; };
  for (int a = 0; a < kActCount; ++a) {
    const ActionInfo& info = actionInfo(Act(a));
    if (!f.empty() && lower(info.name).find(f) == std::string::npos && lower(shortcutLabel(Act(a))).find(f) == std::string::npos)
      continue;
    if (!lastGroup || std::strcmp(lastGroup, info.group) != 0) {
      endTable();
      ImGui::Spacing();
      ImGui::SeparatorText(info.group);
      lastGroup = info.group;
      tableOpen = ImGui::BeginTable(info.group, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
      if (tableOpen) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Alternative", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      }
    }
    if (!tableOpen) continue;
    ImGui::PushID(a);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(info.name);
    if (isToolAct(Act(a)) && prefs.springTools) {
      ImGui::SameLine();
      ImGui::TextDisabled("tap / hold");
    }
    for (int s = 0; s < 2; ++s) {
      ImGui::TableNextColumn();
      ImGui::PushID(s);
      bool capturing = captureAct == a && captureSlot == s;
      std::string label = capturing ? "Press a key..." : keys[a][s].empty() ? "-" : comboLabel(keys[a][s]);
      float xw = ImGui::GetFrameHeight();
      float w = ImGui::GetContentRegionAvail().x - (keys[a][s].empty() || capturing ? 0 : xw + ImGui::GetStyle().ItemSpacing.x);
      if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(label.c_str(), ImVec2(std::max(w, 40 * sc), 0))) {
        captureAct = capturing ? -1 : a;
        captureSlot = s;
      }
      if (capturing) ImGui::PopStyleColor();
      if (!keys[a][s].empty() && !capturing) {
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(xw, 0))) keys[a][s] = KeyCombo{};
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this key");
      }
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  endTable();
  ImGui::Spacing();
  ImGui::SeparatorText("Fixed keys");
  ImGui::TextDisabled(
      "Enter  apply the transform\n"
      "Esc  cancel transform / preview, or deselect\n"
      "Alt (brush)  pick a colour while held\n"
      "Shift / Alt (selection tools)  add to / subtract from the selection\n"
      "Shift (shapes)  square / circle\n"
      "Ctrl+Alt+drag  change the brush size");
}
