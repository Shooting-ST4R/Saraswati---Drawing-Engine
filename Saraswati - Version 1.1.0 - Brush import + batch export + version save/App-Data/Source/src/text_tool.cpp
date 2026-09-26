// Text tool: click for point text, drag for a text frame (area text). Typing, caret, selection
// and every formatting change are previewed live on the canvas; the text is committed as a new
// layer and stays editable (click it again with the Text tool) while the document is open.
#include "app.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static bool textInside(const TextLayout& l, double x, double y, double margin) {
  return x >= l.boxX0 - margin && x <= l.boxX1 + margin && y >= l.boxY0 - margin && y <= l.boxY1 + margin;
}

std::shared_ptr<LoadedFont> App::textFont() {
  FontFace f = FontLibrary::builtIn();
  if (const FontFace* ff = fonts.face(textStyle.family, textStyle.style)) f = *ff;
  std::string key = f.path + "#" + std::to_string(f.index);
  if (key != textFontKey || !textFontCache) {
    textFontCache = fonts.load(f);
    if (!textFontCache) textFontCache = fonts.load(FontLibrary::builtIn());
    textFontKey = key;
  }
  return textFontCache;
}

int App::textHit(double x, double y) const {
  const auto& cs = textEdit.lay.carets;
  if (cs.empty()) return 0;
  int bestLine = -1;
  double bestDy = 1e30;
  for (const auto& c : cs) {
    double dy = y < c.top ? c.top - y : y > c.bottom ? y - c.bottom : 0;
    if (dy < bestDy) { bestDy = dy; bestLine = c.line; }
  }
  int best = 0;
  double bestDx = 1e30;
  for (size_t i = 0; i < cs.size(); ++i) {
    if (cs[i].line != bestLine) continue;
    double dx = std::abs(cs[i].x - x);
    if (dx < bestDx) { bestDx = dx; best = int(i); }
  }
  return best;
}

void App::textRelayout() {
  auto f = textFont();
  layoutText(f.get(), textStyle, textEdit.box, textEdit.lay, true);
}

void App::updateTextPreview() {
  if (!textEdit.active) return;
  textEdit.dirty = false;
  textEdit.shownStyle = textStyle;
  textEdit.shownBubble = bubbleStyle;
  for (int k = 0; k < 3; ++k) textEdit.shownColor[k] = color[k];
  textRelayout();
  int li = R.indexOf(textEdit.layerId);
  if (li < 0) return;
  if (textPreviewOn) { R.abortStroke(); textPreviewOn = false; }
  updateBubblePreview();
  if (!textEdit.lay.w) return;
  StrokeStyle st;
  st.color[0] = color[0]; st.color[1] = color[1]; st.color[2] = color[2];
  st.opacity = 1;
  st.overlay = true;  // text is not clipped by the selection
  R.beginStroke(li, st);
  std::string err;
  R.previewCoverage(textEdit.lay.x0, textEdit.lay.y0, textEdit.lay.w, textEdit.lay.h, textEdit.lay.cov.data(), err);
  textPreviewOn = true;
}

// The bubble goes straight into its own layer (under the text layer) while editing, so it is seen
// in the right layer order; Done replaces it by one undoable stamp.
void App::renderTextBubble(BubbleImage& img) {
  const TextLayout& l = textEdit.lay;
  double y1 = std::max(l.boxY1, l.boxY0 + textStyle.size * 0.6);
  BubbleGeom g = bubbleGeometry(bubbleStyle, l.boxX0, l.boxY0, l.boxX1, y1);
  std::vector<BubbleTail> tails = textEdit.tails;
  if (textEdit.tailDrawing && textEdit.curTail.size() >= 2) tails.push_back(textEdit.curTail);
  renderBubble(bubbleStyle, g, tails, img);
}

void App::clearBubblePreview() {
  auto& e = textEdit;
  int bi = R.indexOf(e.bubbleLayerId);
  if (bi < 0 || !e.shownW) return;
  std::vector<uint8_t> zero(size_t(e.shownW) * e.shownH * 4, 0);
  std::string err;
  R.uploadLayerPixels(bi, e.shownX, e.shownY, e.shownW, e.shownH, zero.data(), err);
  e.shownW = e.shownH = 0;
}

bool App::ensureBubbleLayer() {
  auto& e = textEdit;
  if (R.indexOf(e.bubbleLayerId) >= 0) return true;
  int ti = R.indexOf(e.layerId);
  if (ti < 0) return false;
  std::string err;
  int bi = R.addLayer(ti, err);  // directly under the text layer
  if (bi < 0) { error(err); bubbleStyle.enabled = false; return false; }
  ++e.editUndos;
  Layer& bl = R.layers[size_t(bi)];
  bl.name = "Bubble";
  bl.parentId = e.layerId;
  e.bubbleLayerId = bl.id;
  active = R.indexOf(e.layerId);
  return true;
}

void App::updateBubblePreview() {
  auto& e = textEdit;
  if (!bubbleStyle.enabled) { clearBubblePreview(); return; }
  if (!ensureBubbleLayer()) return;
  int bi = R.indexOf(e.bubbleLayerId);
  BubbleImage img;
  renderTextBubble(img);
  // one upload over old + new area: the bubble where it is, transparent where it was
  int x0 = img.x0, y0 = img.y0, x1 = img.x0 + int(img.w), y1 = img.y0 + int(img.h);
  if (e.shownW) {
    x0 = std::min(x0, e.shownX); y0 = std::min(y0, e.shownY);
    x1 = std::max(x1, e.shownX + int(e.shownW)); y1 = std::max(y1, e.shownY + int(e.shownH));
  }
  if (x0 >= x1 || y0 >= y1) return;
  uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
  std::vector<uint8_t> px(size_t(w) * h * 4, 0);
  for (uint32_t y = 0; y < img.h; ++y) {
    uint8_t* d = &px[(size_t(y + uint32_t(img.y0 - y0)) * w + uint32_t(img.x0 - x0)) * 4];
    const uint8_t* s = &img.rgba[size_t(y) * img.w * 4];
    for (uint32_t x = 0; x < img.w; ++x, d += 4, s += 4) {  // premultiply for the layer
      d[3] = s[3];
      for (int k = 0; k < 3; ++k) d[k] = uint8_t((s[k] * s[3] + 127) / 255);
    }
  }
  std::string err;
  R.uploadLayerPixels(bi, x0, y0, w, h, px.data(), err);
  R.markCachesDirty();
  e.shownX = img.x0; e.shownY = img.y0; e.shownW = img.w; e.shownH = img.h;
}

void App::beginTextInput() {
  SDL_StartTextInput(window);
  textEdit.blinkNs = SDL_GetTicksNS();
}

void App::commitText() {
  if (!textEdit.active) return;
  auto& e = textEdit;
  if (e.box.text.empty() && !e.reedit) { cancelText(); return; }  // nothing typed: leave no layers behind
  e.active = false;
  e.tailMode = e.tailDrawing = false;
  SDL_StopTextInput(window);
  if (textPreviewOn) { R.abortStroke(); textPreviewOn = false; }
  textRelayout();
  std::string err;
  int idx = R.indexOf(e.layerId);
  if (idx < 0) return;
  // bubble: preview pixels out, final bubble in as one undo step
  BubbleImage img;
  bool bubble = bubbleStyle.enabled && !e.box.text.empty();
  if (bubble) renderTextBubble(img);
  clearBubblePreview();
  int bi = R.indexOf(e.bubbleLayerId);
  if (bubble && bi >= 0 && img.w) {
    Floating fl;
    if (R.createFloating(img.w, img.h, img.rgba.data(), fl, err)) {
      double inv[9] = {1, 0, double(-img.x0), 0, 1, double(-img.y0), 0, 0, 1};
      R.stamp(bi, fl, inv, img.x0, img.y0, img.x0 + int(img.w), img.y0 + int(img.h), err);
      R.destroyFloating(fl);
    }
  } else if (!bubble && bi >= 0 && !e.reedit) {
    R.deleteLayer(bi);  // bubble switched off again: no empty sublayer
    e.bubbleLayerId = 0;
    idx = R.indexOf(e.layerId);
  }
  StrokeStyle st;
  st.color[0] = color[0]; st.color[1] = color[1]; st.color[2] = color[2];
  st.opacity = 1;
  st.ignoreSelection = true;
  const TextLayout& l = e.lay;
  if (l.w && !R.paintCoverage(idx, l.x0, l.y0, l.w, l.h, l.cov.data(), st, err) && !err.empty()) error(err);
  if (!e.reedit || R.layers[size_t(idx)].name.rfind("Text", 0) == 0) {
    std::string first = u32ToUtf8(e.box.text.substr(0, e.box.text.find(U'\n')));
    if (first.size() > 24) first = first.substr(0, 24) + "...";
    R.layers[size_t(idx)].name = "Text: " + first;
  }
  TextObject o;
  o.box = e.box;
  o.style = textStyle;
  o.bubble = bubbleStyle;
  o.tails = e.tails;
  o.bubbleLayerId = e.bubbleLayerId;
  for (int k = 0; k < 3; ++k) o.color[k] = color[k];
  o.docSerial = R.docSerial;
  textObjects[e.layerId] = o;
  active = idx;
}

void App::cancelText() {
  if (!textEdit.active) return;
  auto& e = textEdit;
  e.active = false;
  e.tailMode = e.tailDrawing = false;
  SDL_StopTextInput(window);
  if (textPreviewOn) { R.abortStroke(); textPreviewOn = false; }
  clearBubblePreview();
  // new layers / lifted pixels of this edit: undo them (they are the newest undo steps)
  for (int i = 0; i < e.editUndos; ++i) R.undo();
  if (e.reedit) {
    textStyle = e.savedStyle;
    bubbleStyle = e.savedBubble;
  }
  active = std::clamp(active, 0, int(R.layers.size()) - 1);
}

bool App::textDown(double dx, double dy) {
  toolDrag = true;
  t0x = t1x = dx;
  t0y = t1y = dy;
  textCreating = textSelecting = false;
  if (textEdit.active) {
    if (textEdit.tailMode) {  // drawing a bubble tail
      textEdit.tailDrawing = true;
      textEdit.curTail = {{dx, dy}};
      return true;
    }
    double margin = 6 / view.zoom;
    if (textInside(textEdit.lay, dx, dy, margin)) {
      int c = textHit(dx, dy);
      textEdit.caret = c;
      if (!shiftDown) textEdit.anchor = c;
      textSelecting = true;
      textEdit.blinkNs = SDL_GetTicksNS();
      return true;
    }
    commitText();
  }
  // click on text made earlier (its layer or its bubble is active): edit it again
  if (R.hasDocument() && active >= 0 && active < int(R.layers.size())) {
    uint32_t id = R.layers[size_t(active)].id;
    if (R.layers[size_t(active)].parentId && textObjects.count(R.layers[size_t(active)].parentId)) id = R.layers[size_t(active)].parentId;
    auto it = textObjects.find(id);
    int ti = R.indexOf(id);
    if (it != textObjects.end() && it->second.docSerial == R.docSerial && ti >= 0) {
      TextStyle keepT = textStyle;
      BubbleStyle keepB = bubbleStyle;
      textStyle = it->second.style;
      auto f = textFont();
      TextLayout old;
      layoutText(f.get(), textStyle, it->second.box, old, true);
      bool hit = textInside(old, dx, dy, 6 / view.zoom);
      if (!hit && it->second.bubble.enabled) {
        double y1 = std::max(old.boxY1, old.boxY0 + textStyle.size * 0.6);
        BubbleGeom g = bubbleGeometry(it->second.bubble, old.boxX0, old.boxY0, old.boxX1, y1);
        hit = bubbleShapeDistance(it->second.bubble, g, dx, dy) < 0;
      }
      if (hit) {
        textEdit = TextEditState{};
        textEdit.reedit = true;
        textEdit.savedStyle = keepT;
        textEdit.savedBubble = keepB;
        // lift the old text and bubble off their layers (undo steps; Cancel undoes them)
        StrokeStyle er;
        er.eraser = true;
        er.opacity = 1;
        er.ignoreSelection = true;
        std::string err;
        if (old.w && R.paintCoverage(ti, old.x0, old.y0, old.w, old.h, old.cov.data(), er, err)) ++textEdit.editUndos;
        int bi = R.indexOf(it->second.bubbleLayerId);
        if (bi >= 0) {
          const Layer& bl = R.layers[size_t(bi)];
          if (bl.bx0 < bl.bx1) {
            std::vector<uint8_t> all(size_t(bl.bx1 - bl.bx0) * (bl.by1 - bl.by0), 255);
            if (R.paintCoverage(bi, bl.bx0, bl.by0, uint32_t(bl.bx1 - bl.bx0), uint32_t(bl.by1 - bl.by0), all.data(), er, err))
              ++textEdit.editUndos;
          }
          textEdit.bubbleLayerId = it->second.bubbleLayerId;
        }
        for (int k = 0; k < 3; ++k) color[k] = it->second.color[k];
        bubbleStyle = it->second.bubble;
        textEdit.tails = it->second.tails;
        textEdit.active = true;
        textEdit.layerId = id;
        textEdit.box = it->second.box;
        textEdit.lay = old;
        textEdit.caret = textEdit.anchor = textHit(dx, dy);
        textEdit.dirty = true;
        textSelecting = true;
        active = R.indexOf(id);
        beginTextInput();
        return true;
      }
      textStyle = keepT;
    }
  }
  textCreating = true;
  return true;
}

void App::textMove(double dx, double dy) {
  if (textEdit.tailDrawing) {
    auto& t = textEdit.curTail;
    if (std::hypot(dx - t.back().x, dy - t.back().y) * view.zoom >= 2) { t.push_back({dx, dy}); textEdit.dirty = true; }
    return;
  }
  if (textSelecting && textEdit.active) {
    textEdit.caret = textHit(dx, dy);
    textEdit.blinkNs = SDL_GetTicksNS();
  }
}

void App::textUp(double dx, double dy) {
  toolDrag = false;
  if (textEdit.tailDrawing) {
    textEdit.tailDrawing = false;
    textEdit.curTail.push_back({dx, dy});
    if (textEdit.curTail.size() >= 2) textEdit.tails.push_back(textEdit.curTail);
    textEdit.curTail.clear();
    textEdit.tailMode = false;
    textEdit.dirty = true;
    return;
  }
  if (textSelecting) { textSelecting = false; return; }
  if (!textCreating || !R.hasDocument()) return;
  textCreating = false;
  textEdit = TextEditState{};
  double w = std::abs(dx - t0x), h = std::abs(dy - t0y);
  if (w * view.zoom > 8 && h * view.zoom > 4) {  // dragged: a text frame, lines wrap at its width
    textEdit.box.x = std::min(dx, t0x);
    textEdit.box.y = std::min(dy, t0y);
    textEdit.box.width = w;
  } else {  // clicked: point text, the click is on the first baseline
    auto f = textFont();
    TextLayout probe;
    TextBox b;
    b.text = U"X";
    layoutText(f.get(), textStyle, b, probe, false);
    double ascent = probe.carets.empty() ? textStyle.size * 0.8 : -probe.carets[0].top;
    textEdit.box.x = t0x;
    textEdit.box.y = t0y - ascent;
  }
  // the text layer exists from the start, so the text (and its bubble) show in the right order
  std::string err;
  int idx = R.addLayer(active + 1, err);
  if (idx < 0) { error(err); return; }
  textEdit.editUndos = 1;
  R.layers[size_t(idx)].name = "Text";
  textEdit.layerId = R.layers[size_t(idx)].id;
  active = idx;
  textEdit.active = true;
  textEdit.dirty = true;
  beginTextInput();
}

void App::textInsert(const std::u32string& s) {
  auto& e = textEdit;
  int a = std::min(e.caret, e.anchor), b = std::max(e.caret, e.anchor);
  e.history.push_back({e.box.text, e.caret});
  if (e.history.size() > 200) e.history.erase(e.history.begin());
  e.box.text.replace(size_t(a), size_t(b - a), s);
  e.caret = e.anchor = a + int(s.size());
  e.dirty = true;
  e.blinkNs = SDL_GetTicksNS();
}

bool App::textKey(const SDL_KeyboardEvent& k) {
  auto& e = textEdit;
  bool ctrl = ctrlDown, shift = shiftDown;
  int n = int(e.box.text.size());
  auto moveTo = [&](int c) {
    e.caret = std::clamp(c, 0, n);
    if (!shift) e.anchor = e.caret;
    e.blinkNs = SDL_GetTicksNS();
  };
  bool hasSel = e.caret != e.anchor;
  switch (k.key) {
    case SDLK_ESCAPE: commitText(); return true;
    case SDLK_RETURN: case SDLK_KP_ENTER:
      if (ctrl || k.key == SDLK_KP_ENTER) commitText();
      else textInsert(U"\n");
      return true;
    case SDLK_TAB: textInsert(U"\t"); return true;
    case SDLK_BACKSPACE:
      if (!hasSel && e.caret > 0) e.anchor = e.caret - 1;
      if (e.caret != e.anchor) textInsert(U"");
      return true;
    case SDLK_DELETE:
      if (!hasSel && e.caret < n) e.anchor = e.caret + 1;
      if (e.caret != e.anchor) textInsert(U"");
      return true;
    case SDLK_LEFT: moveTo(hasSel && !shift ? std::min(e.caret, e.anchor) : e.caret - 1); return true;
    case SDLK_RIGHT: moveTo(hasSel && !shift ? std::max(e.caret, e.anchor) : e.caret + 1); return true;
    case SDLK_UP: case SDLK_DOWN: case SDLK_HOME: case SDLK_END: {
      const auto& cs = e.lay.carets;
      if (cs.empty() || e.caret >= int(cs.size())) return true;
      int line = cs[size_t(e.caret)].line;
      if (k.key == SDLK_HOME || k.key == SDLK_END) {
        int best = e.caret;
        for (int i = 0; i < int(cs.size()); ++i)
          if (cs[size_t(i)].line == line) { if (k.key == SDLK_HOME) { best = i; break; } best = i; }
        moveTo(best);
        return true;
      }
      int target = line + (k.key == SDLK_UP ? -1 : 1);
      double x = cs[size_t(e.caret)].x, bestDx = 1e30;
      int best = k.key == SDLK_UP ? 0 : n;
      for (int i = 0; i < int(cs.size()); ++i)
        if (cs[size_t(i)].line == target && std::abs(cs[size_t(i)].x - x) < bestDx) { bestDx = std::abs(cs[size_t(i)].x - x); best = i; }
      moveTo(best);
      return true;
    }
    default: break;
  }
  if (ctrl) {
    int a = std::min(e.caret, e.anchor), b = std::max(e.caret, e.anchor);
    switch (k.key) {
      case SDLK_A: e.anchor = 0; e.caret = n; return true;
      case SDLK_C: case SDLK_X:
        if (a < b) SDL_SetClipboardText(u32ToUtf8(e.box.text.substr(size_t(a), size_t(b - a))).c_str());
        if (k.key == SDLK_X && a < b) textInsert(U"");
        return true;
      case SDLK_V:
        if (char* t = SDL_GetClipboardText()) {
          std::u32string s = utf8ToU32(t);
          SDL_free(t);
          s.erase(std::remove(s.begin(), s.end(), U'\r'), s.end());
          if (!s.empty()) textInsert(s);
        }
        return true;
      case SDLK_Z:
        if (!e.history.empty()) {
          e.box.text = e.history.back().first;
          e.caret = e.anchor = std::min<int>(e.history.back().second, int(e.box.text.size()));
          e.history.pop_back();
          e.dirty = true;
        }
        return true;
      case SDLK_EQUALS: case SDLK_MINUS: case SDLK_KP_PLUS: case SDLK_KP_MINUS: case SDLK_0: case SDLK_1:
        return false;  // view zoom keeps working while typing
      default:
        commitText();  // e.g. Ctrl+S: finish the text first
        return false;
    }
  }
  if (altDown) return false;
  return !isModifierKey(k.key);  // plain keys are typing (SDL_EVENT_TEXT_INPUT), not shortcuts
}

// frame, selection and blinking caret over the canvas
void App::drawTextOverlay(ImDrawList* dl) {
  float fb = ImGui::GetIO().DisplayFramebufferScale.x;
  auto S = [&](double x, double y) {
    float sx, sy;
    docToScreen(x, y, sx, sy);
    return ImVec2(sx / fb, sy / fb);
  };
  ImU32 blue = IM_COL32(60, 150, 255, 255);
  if (textCreating && toolDrag) {
    double x0 = std::min(t0x, t1x), x1 = std::max(t0x, t1x), y0 = std::min(t0y, t1y), y1 = std::max(t0y, t1y);
    dl->AddQuad(S(x0, y0), S(x1, y0), S(x1, y1), S(x0, y1), blue, 1.0f);
  }
  if (!textEdit.active) return;
  if (textEdit.tailDrawing && textEdit.curTail.size() >= 2) {
    std::vector<ImVec2> pts;
    for (auto& p : textEdit.curTail) pts.push_back(S(p.x, p.y));
    dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(60, 150, 255, 220), 2.0f, 0);
  }
  const TextLayout& l = textEdit.lay;
  double y1 = std::max(l.boxY1, l.boxY0 + textStyle.size * 0.5);
  dl->AddQuad(S(l.boxX0, l.boxY0), S(l.boxX1, l.boxY0), S(l.boxX1, y1), S(l.boxX0, y1), IM_COL32(60, 150, 255, 170), 1.0f);
  const auto& cs = l.carets;
  if (cs.empty()) return;
  int a = std::clamp(std::min(textEdit.caret, textEdit.anchor), 0, int(cs.size()) - 1);
  int b = std::clamp(std::max(textEdit.caret, textEdit.anchor), 0, int(cs.size()) - 1);
  for (int i = a; i < b; ++i) {  // selection: one quad per character
    const auto& c0 = cs[size_t(i)];
    const auto& c1 = cs[size_t(i + 1)];
    double x1 = c1.line == c0.line ? c1.x : c0.x + textStyle.size * 0.25;
    dl->AddQuadFilled(S(c0.x, c0.top), S(x1, c0.top), S(x1, c0.bottom), S(c0.x, c0.bottom), IM_COL32(60, 150, 255, 90));
  }
  uint64_t t = (SDL_GetTicksNS() - textEdit.blinkNs) / 1000000;
  if ((t / 530) % 2 == 0) {
    const auto& c = cs[size_t(std::clamp(textEdit.caret, 0, int(cs.size()) - 1))];
    dl->AddLine(S(c.x, c.top), S(c.x, c.bottom), IM_COL32(0, 0, 0, 255), 3.0f);
    dl->AddLine(S(c.x, c.top), S(c.x, c.bottom), IM_COL32(255, 255, 255, 255), 1.4f);
    ImVec2 p = S(c.x, c.bottom);  // IME candidate window next to the caret
    SDL_Rect r{int(p.x * fb), int(p.y * fb), 1, 1};
    SDL_SetTextInputArea(window, &r, 0);
  }
}

// ---------------------------------------------------------------------------
// Tool Settings: Character + Paragraph (the basic options of InDesign's panels)

void App::drawTextSettings() {
  float fh = ImGui::GetFrameHeight();
  float w = ImGui::GetContentRegionAvail().x;
  auto fullWidth = [] { ImGui::SetNextItemWidth(-FLT_MIN); };
  ImGui::SeparatorText("Character");
  if (!fonts.ready()) ImGui::TextDisabled("Looking for installed fonts...");
  // font family with search
  std::string famLabel = textStyle.family.empty() ? FontLibrary::builtIn().family : textStyle.family;
  fullWidth();
  if (ImGui::BeginCombo("##family", famLabel.c_str(), ImGuiComboFlags_HeightLargest)) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##fsearch", "Search fonts", fontSearch, sizeof fontSearch);
    std::string q = fontSearch;
    for (char& c : q) c = char(std::tolower((unsigned char)c));
    std::vector<const FontFamily*> list;
    if (fonts.ready())
      for (const FontFamily& f : fonts.families()) {
        std::string n = f.name;
        for (char& c : n) c = char(std::tolower((unsigned char)c));
        if (q.empty() || n.find(q) != std::string::npos) list.push_back(&f);
      }
    ImGui::TextDisabled("%zu font families", list.size());
    ImGui::BeginChild("##fl", ImVec2(0, fh * 12));
    ImGuiListClipper clip;
    clip.Begin(int(list.size()));
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const FontFamily& f = *list[size_t(i)];
        ImGui::PushID(i);
        if (ImGui::Selectable(f.name.c_str(), f.name == famLabel)) {
          textStyle.family = f.name;
          if (const FontFace* ff = fonts.face(f.name, textStyle.style)) textStyle.style = ff->style;
          ImGui::CloseCurrentPopup();
        }
        if (f.faces.size() > 1) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%zu)", f.faces.size());
        }
        ImGui::PopID();
      }
    ImGui::EndChild();
    ImGui::EndCombo();
  }
  // style
  const FontFamily* fam = fonts.family(famLabel);
  fullWidth();
  if (ImGui::BeginCombo("##style", textStyle.style.c_str())) {
    if (fam)
      for (const FontFace& f : fam->faces)
        if (ImGui::Selectable(f.style.c_str(), f.style == textStyle.style)) textStyle.style = f.style;
    ImGui::EndCombo();
  }
  float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
  auto pair = [&](const char* id1, float* v1, float a1, float b1, const char* f1, const char* tip1, const char* id2, float* v2,
                  float a2, float b2, const char* f2, const char* tip2, ImGuiSliderFlags fl1 = 0, ImGuiSliderFlags fl2 = 0) {
    ImGui::SetNextItemWidth(half);
    ImGui::DragFloat(id1, v1, std::max(0.05f, (b1 - a1) / 1000), a1, b1, f1, fl1 | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    ImGui::DragFloat(id2, v2, std::max(0.05f, (b2 - a2) / 1000), a2, b2, f2, fl2 | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip2);
  };
  pair("##size", &textStyle.size, 1, 5000, "Size %.1f px", "Font size", "##lead", &textStyle.leading, 0, 10000,
       textStyle.leading <= 0 ? "Leading Auto" : "Leading %.1f px", "Leading (distance between baselines; 0 = Auto, 120 %)",
       ImGuiSliderFlags_Logarithmic, ImGuiSliderFlags_Logarithmic);
  {
    ImGui::SetNextItemWidth(half);
    const char* kern[] = {"Kerning: None", "Kerning: Metrics"};
    ImGui::Combo("##kern", &textStyle.kerning, kern, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    ImGui::DragFloat("##track", &textStyle.tracking, 1, -500, 2000, "Tracking %.0f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tracking (1/1000 em between all characters)");
  }
  pair("##vs", &textStyle.vScale, 10, 400, "V scale %.0f %%", "Vertical scale", "##hs", &textStyle.hScale, 10, 400,
       "H scale %.0f %%", "Horizontal scale");
  pair("##bs", &textStyle.baselineShift, -2000, 2000, "Baseline %.1f px", "Baseline shift (up)", "##sk", &textStyle.skew, -60, 60,
       "Skew %.0f deg", "Skew (false italic)");
  float bs = fh * 1.15f;
  if (iconButton("##caps", "case-upper", bs, textStyle.allCaps, "All caps")) textStyle.allCaps = !textStyle.allCaps;
  ImGui::SameLine();
  if (iconButton("##ul", "underline", bs, textStyle.underline, "Underline")) textStyle.underline = !textStyle.underline;
  ImGui::SameLine();
  if (iconButton("##st", "strikethrough", bs, textStyle.strike, "Strikethrough")) textStyle.strike = !textStyle.strike;
  ImGui::SameLine();
  ImGui::ColorButton("##tc", ImVec4(color[0], color[1], color[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(bs, bs));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Text colour = the drawing colour (Colour panel)");
  ImGui::SeparatorText("Paragraph");
  const char* alignIcons[] = {"text-align-start", "text-align-center", "text-align-end", "text-align-justify"};
  const char* alignTips[] = {"Align left", "Align centre", "Align right", "Justify (last line left)"};
  for (int i = 0; i < 4; ++i) {
    ImGui::PushID(i);
    if (i) ImGui::SameLine();
    if (iconButton("##al", alignIcons[i], bs, textStyle.align == i, alignTips[i])) textStyle.align = i;
    ImGui::PopID();
  }
  pair("##il", &textStyle.indentLeft, -5000, 5000, "Left indent %.0f", "Left indent (px)", "##ir", &textStyle.indentRight, -5000, 5000,
       "Right indent %.0f", "Right indent (px, text frames)");
  ImGui::SetNextItemWidth(half);
  ImGui::DragFloat("##if", &textStyle.indentFirst, 1, -5000, 5000, "First line %.0f", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("First line left indent (px)");
  pair("##sb", &textStyle.spaceBefore, 0, 5000, "Space before %.0f", "Space before each paragraph (px)", "##sa", &textStyle.spaceAfter,
       0, 5000, "Space after %.0f", "Space after each paragraph (px)");
  drawBubbleSettings();
  ImGui::Spacing();
  if (textEdit.active) {
    if (ImGui::Button("Done", ImVec2(half, 0))) commitText();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Finish the text (Esc)");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(half, 0))) cancelText();
  } else {
    ImGui::TextDisabled("Click for a line of text, drag for a text frame.\nClick existing text on the active layer to edit it.");
  }
}

// ---------------------------------------------------------------------------
// Speech bubble settings (Tool Settings, under the text options)

void App::drawBubbleSettings() {
  auto& b = bubbleStyle;
  ImGui::SeparatorText("Speech bubble");
  ImGui::Checkbox("Bubble around the text", &b.enabled);
  if (!b.enabled) return;
  float w = ImGui::GetContentRegionAvail().x;
  float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
  const char* shapes[] = {"Ellipse", "Rounded rectangle", "Rectangle", "Cloud (thought)", "Burst (shout)"};
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::Combo("##bshape", &b.shape, shapes, 5);
  auto drag = [&](const char* id, float* v, float mn, float mx, const char* fmt, const char* tip, float wd) {
    ImGui::SetNextItemWidth(wd);
    ImGui::DragFloat(id, v, std::max(0.05f, (mx - mn) / 500), mn, mx, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  drag("##bpx", &b.padX, 0, 2000, "Distance X %.0f", "Space between the text and the bubble, left / right", half);
  ImGui::SameLine();
  drag("##bpy", &b.padY, 0, 2000, "Distance Y %.0f", "Space between the text and the bubble, top / bottom", half);
  if (b.shape == 1) drag("##bcr", &b.corner, 0, 2000, "Corner radius %.0f", nullptr, -FLT_MIN);
  if (b.shape >= 3) {
    ImGui::SetNextItemWidth(half);
    ImGui::DragInt("##bn", &b.bumps, 0.2f, 3, 80, b.shape == 3 ? "Bumps %d" : "Spikes %d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    float am = b.amount * 100;
    ImGui::SetNextItemWidth(half);
    if (ImGui::DragFloat("##ba", &am, 0.3f, 2, 60, "Depth %.0f %%", ImGuiSliderFlags_AlwaysClamp)) b.amount = am / 100;
  }
  ImGui::ColorEdit3("##bfill", b.fill, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill colour");
  ImGui::SameLine();
  float fo = b.fillOpacity * 100;
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::SliderFloat("##bfo", &fo, 0, 100, "Fill opacity %.0f %%")) b.fillOpacity = fo / 100;
  ImGui::ColorEdit3("##bbc", b.border, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Border colour");
  ImGui::SameLine();
  float bw2 = ImGui::GetContentRegionAvail().x;
  float bh = (bw2 - ImGui::GetStyle().ItemSpacing.x) / 2;
  drag("##bbw", &b.borderWidth, 0, 200, "Border %.1f px", "Border thickness", bh);
  ImGui::SameLine();
  float bo = b.borderOpacity * 100;
  ImGui::SetNextItemWidth(bh);
  if (ImGui::SliderFloat("##bbo", &bo, 0, 100, "Opacity %.0f %%")) b.borderOpacity = bo / 100;
  ImGui::SeparatorText("Tails");
  drag("##btw", &b.tailWidth, 2, 2000, "Tail width %.0f", "Width of a tail where it leaves the bubble", half);
  ImGui::SameLine();
  const char* modes[] = {"Freehand", "Straight"};
  ImGui::SetNextItemWidth(half);
  ImGui::Combo("##btm", &b.tailMode, modes, 2);
  if (textEdit.active) {
    if (ImGui::Button(textEdit.tailMode ? "Drag on the canvas..." : "Draw a tail", ImVec2(half, 0))) textEdit.tailMode = !textEdit.tailMode;
    ImGui::SameLine();
    ImGui::BeginDisabled(textEdit.tails.empty());
    if (ImGui::Button("Remove tails", ImVec2(half, 0))) { textEdit.tails.clear(); textEdit.dirty = true; }
    ImGui::EndDisabled();
  }
}

// small buttons next to the bubble: add a tail / remove tails
void App::drawBubbleButtons() {
  if (!textEdit.active || !bubbleStyle.enabled || hideUI || textEdit.tailDrawing) return;
  const TextLayout& l = textEdit.lay;
  double y1 = std::max(l.boxY1, l.boxY0 + textStyle.size * 0.6);
  BubbleGeom g = bubbleGeometry(bubbleStyle, l.boxX0, l.boxY0, l.boxX1, y1);
  float fb = ImGui::GetIO().DisplayFramebufferScale.x;
  float maxX = -1e9f, minY = 1e9f;
  for (int i = 0; i < 4; ++i) {
    float sx, sy;
    docToScreen(g.cx + (i & 1 ? g.hx : -g.hx), g.cy + (i & 2 ? g.hy : -g.hy), sx, sy);
    maxX = std::max(maxX, sx); minY = std::min(minY, sy);
  }
  float sc = ImGui::GetStyle().FontScaleDpi;
  float bs = 26 * sc;
  float x = maxX / fb + 6 * sc, y = minY / fb;
  float cx0 = canvasX / fb, cy0 = canvasY / fb, cx1 = (canvasX + canvasW) / fb, cy1 = (canvasY + canvasH) / fb;
  x = std::clamp(x, cx0 + 4, std::max(cx0 + 4, cx1 - bs - 16 * sc));
  y = std::clamp(y, cy0 + 4, std::max(cy0 + 4, cy1 - 2 * bs - 20 * sc));
  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::SetNextWindowBgAlpha(0.94f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * sc, 4 * sc));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * sc);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##bubblebar", nullptr, fl)) {
    if (iconButton("##tail", "message-circle", bs, textEdit.tailMode,
                   "Draw a tail: drag from the bubble towards the speaker\n(freehand follows your stroke; see Tool Settings)"))
      textEdit.tailMode = !textEdit.tailMode;
    if (iconButton("##notail", "x", bs, false, "Remove the tails", !textEdit.tails.empty())) { textEdit.tails.clear(); textEdit.dirty = true; }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}
