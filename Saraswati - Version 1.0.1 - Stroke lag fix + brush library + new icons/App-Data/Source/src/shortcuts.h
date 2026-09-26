// Keyboard shortcuts: every action with up to two rebindable key combinations.
// Tool keys are "spring-loaded": tap to switch tool, hold to use it temporarily and go back on release.
#pragma once
#include <SDL3/SDL.h>
#include <string>

enum class Act : int {
  // tools (tap = switch, hold = temporary)
  ToolBrush, ToolEraser, ToolEyedropper, ToolFill, ToolGradient, ToolLine, ToolRect, ToolEllipse, ToolSelRect,
  ToolSelEllipse, ToolLasso, ToolWand, ToolTransform, ToolHand,
  // held while dragging on the canvas
  PanHold, RotateHold,
  // commands
  Undo, Redo, New, Open, Save, SaveAs, Import, Export, Prefs,
  SelectAll, Deselect, InvertSel, ClearSel, FillSel,
  SwapColors, BrushSmaller, BrushBigger,
  ZoomIn, ZoomOut, ZoomFit, Zoom100, ResetRotation, HidePanels, FlipView, RotateLeft, RotateRight,
  Brush1, Brush2, Brush3, Brush4, Brush5, Brush6, Brush7, Brush8, Brush9,
  Count
};
constexpr int kActCount = int(Act::Count);
constexpr bool isToolAct(Act a) { return int(a) <= int(Act::ToolHand); }

enum : int { KM_CTRL = 1, KM_SHIFT = 2, KM_ALT = 4 };

struct KeyCombo {
  SDL_Keycode key = 0;  // 0 = unassigned
  int mods = 0;         // KM_* bits
  bool operator==(const KeyCombo& o) const { return key == o.key && mods == o.mods; }
  bool empty() const { return key == 0; }
};

struct ActionInfo {
  const char* id;     // stable name for the settings file
  const char* name;   // shown in Preferences and tooltips
  const char* group;
  KeyCombo def[2];
};

const ActionInfo& actionInfo(Act a);
int modsFromSdl(SDL_Keymod m);
bool isModifierKey(SDL_Keycode k);
std::string comboLabel(const KeyCombo& c);
