#include "shortcuts.h"

namespace {
constexpr int C = KM_CTRL, S = KM_SHIFT, A = KM_ALT;
const ActionInfo kActions[kActCount] = {
    {"tool.brush", "Brush", "Tools", {{SDLK_B, 0}, {}}},
    {"tool.eraser", "Eraser", "Tools", {{SDLK_E, 0}, {}}},
    {"tool.eyedropper", "Eyedropper", "Tools", {{SDLK_I, 0}, {}}},
    {"tool.fill", "Fill", "Tools", {{SDLK_G, 0}, {}}},
    {"tool.gradient", "Gradient", "Tools", {{SDLK_G, S}, {}}},
    {"tool.line", "Line", "Tools", {{SDLK_U, 0}, {}}},
    {"tool.rect", "Rectangle", "Tools", {{SDLK_U, S}, {}}},
    {"tool.ellipse", "Ellipse", "Tools", {{SDLK_U, A}, {}}},
    {"tool.selrect", "Rectangle select", "Tools", {{SDLK_M, 0}, {}}},
    {"tool.selellipse", "Ellipse select", "Tools", {{SDLK_M, S}, {}}},
    {"tool.lasso", "Lasso select", "Tools", {{SDLK_L, 0}, {}}},
    {"tool.wand", "Magic wand", "Tools", {{SDLK_W, 0}, {}}},
    {"tool.transform", "Move / Transform", "Tools", {{SDLK_V, 0}, {SDLK_T, C}}},
    {"tool.hand", "Hand", "Tools", {{SDLK_H, 0}, {}}},
    {"view.panhold", "Pan view (hold and drag)", "View", {{SDLK_SPACE, 0}, {}}},
    {"view.rotatehold", "Rotate view (hold and drag)", "View", {{SDLK_SPACE, S}, {}}},
    {"edit.undo", "Undo", "Edit", {{SDLK_Z, C}, {}}},
    {"edit.redo", "Redo", "Edit", {{SDLK_Y, C}, {SDLK_Z, C | S}}},
    {"file.new", "New document", "File", {{SDLK_N, C}, {}}},
    {"file.open", "Open", "File", {{SDLK_O, C}, {}}},
    {"file.save", "Save", "File", {{SDLK_S, C}, {}}},
    {"file.saveas", "Save as", "File", {{SDLK_S, C | S}, {}}},
    {"file.import", "Import image as layer", "File", {{SDLK_I, C}, {}}},
    {"file.export", "Export PNG / JPEG", "File", {{SDLK_E, C}, {}}},
    {"app.prefs", "Preferences", "File", {{SDLK_K, C}, {}}},
    {"sel.all", "Select all", "Selection", {{SDLK_A, C}, {}}},
    {"sel.none", "Deselect", "Selection", {{SDLK_D, C}, {}}},
    {"sel.invert", "Invert selection", "Selection", {{SDLK_I, C | S}, {}}},
    {"sel.clear", "Clear (delete pixels)", "Selection", {{SDLK_DELETE, 0}, {}}},
    {"sel.fill", "Fill with colour", "Selection", {{SDLK_BACKSPACE, A}, {}}},
    {"edit.cut", "Cut", "Edit", {{SDLK_X, C}, {}}},
    {"edit.copy", "Copy", "Edit", {{SDLK_C, C}, {}}},
    {"edit.paste", "Paste", "Edit", {{SDLK_V, C}, {}}},
    {"sel.clearoutside", "Clear outside the selection", "Selection", {{}, {}}},
    {"color.swap", "Swap colours", "Brush & colour", {{SDLK_X, 0}, {}}},
    {"brush.smaller", "Brush smaller", "Brush & colour", {{SDLK_LEFTBRACKET, 0}, {}}},
    {"brush.bigger", "Brush bigger", "Brush & colour", {{SDLK_RIGHTBRACKET, 0}, {}}},
    {"view.zoomin", "Zoom in", "View", {{SDLK_EQUALS, 0}, {SDLK_KP_PLUS, 0}}},
    {"view.zoomout", "Zoom out", "View", {{SDLK_MINUS, 0}, {SDLK_KP_MINUS, 0}}},
    {"view.fit", "Fit to window", "View", {{SDLK_0, C}, {}}},
    {"view.100", "100 %", "View", {{SDLK_1, C}, {}}},
    {"view.resetrot", "Reset rotation", "View", {{SDLK_R, 0}, {}}},
    {"view.hideui", "Hide panels", "View", {{SDLK_TAB, 0}, {}}},
    {"view.flip", "Flip view horizontally", "View", {{}, {}}},
    {"view.rotleft", "Rotate view left 15 deg", "View", {{}, {}}},
    {"view.rotright", "Rotate view right 15 deg", "View", {{}, {}}},
    {"brush.slot1", "Brush slot 1", "Brush & colour", {{SDLK_1, 0}, {}}},
    {"brush.slot2", "Brush slot 2", "Brush & colour", {{SDLK_2, 0}, {}}},
    {"brush.slot3", "Brush slot 3", "Brush & colour", {{SDLK_3, 0}, {}}},
    {"brush.slot4", "Brush slot 4", "Brush & colour", {{SDLK_4, 0}, {}}},
    {"brush.slot5", "Brush slot 5", "Brush & colour", {{SDLK_5, 0}, {}}},
    {"brush.slot6", "Brush slot 6", "Brush & colour", {{SDLK_6, 0}, {}}},
    {"brush.slot7", "Brush slot 7", "Brush & colour", {{SDLK_7, 0}, {}}},
    {"brush.slot8", "Brush slot 8", "Brush & colour", {{SDLK_8, 0}, {}}},
    {"brush.slot9", "Brush slot 9", "Brush & colour", {{SDLK_9, 0}, {}}},
};
}  // namespace

const ActionInfo& actionInfo(Act a) { return kActions[int(a)]; }

int modsFromSdl(SDL_Keymod m) {
  return ((m & SDL_KMOD_CTRL) ? KM_CTRL : 0) | ((m & SDL_KMOD_SHIFT) ? KM_SHIFT : 0) | ((m & SDL_KMOD_ALT) ? KM_ALT : 0);
}

bool isModifierKey(SDL_Keycode k) {
  return k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LALT || k == SDLK_RALT ||
         k == SDLK_LGUI || k == SDLK_RGUI || k == SDLK_MODE;
}

std::string comboLabel(const KeyCombo& c) {
  if (c.empty()) return "";
  std::string s;
  if (c.mods & KM_CTRL) s += "Ctrl+";
  if (c.mods & KM_SHIFT) s += "Shift+";
  if (c.mods & KM_ALT) s += "Alt+";
  const char* n = SDL_GetKeyName(c.key);
  s += (n && *n) ? n : "?";
  return s;
}
