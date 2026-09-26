// Saraswati-only layer data that PSD has no place for: layer effects (tone, layer colour), the
// sublayer link (speech bubble under its text) and editable text / speech bubbles. Stored as
// "key values..." lines in a private PSD block and in backups; unknown keys are ignored, so the
// format can grow.
#include "app.h"
#include <cstdio>
#include <sstream>

namespace {
std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}
std::string unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) { o += s[i + 1] == 'n' ? '\n' : s[i + 1]; ++i; }
    else o += s[i];
  }
  return o;
}
}  // namespace

std::string App::layerMeta(int index) const {
  const Layer& l = R.layers[size_t(index)];
  std::ostringstream o;
  o.precision(9);
  if (l.tone.on || !(l.tone == ToneFx{})) {
    const ToneFx& t = l.tone;
    o << "tone " << t.on << ' ' << t.shape << ' ' << t.density << ' ' << t.reflectOpacity << ' ' << t.posterize << ' ' << t.levels << ' '
      << t.frequency << ' ' << t.angle << ' ' << t.noiseSize << ' ' << t.noiseFactor << ' ' << t.offX << ' ' << t.offY << ' '
      << t.express << '\n';
  }
  if (l.lcolor.on || !(l.lcolor == LayerColorFx{})) {
    const LayerColorFx& c = l.lcolor;
    o << "lcolor " << c.on << ' ' << c.main[0] << ' ' << c.main[1] << ' ' << c.main[2] << ' ' << c.sub[0] << ' ' << c.sub[1] << ' '
      << c.sub[2] << ' ' << c.useSub << '\n';
  }
  if (isSublayer(l) && index + 1 < int(R.layers.size()) && R.layers[size_t(index + 1)].id == l.parentId) o << "attached 1\n";
  auto it = textObjects.find(l.id);
  if (it != textObjects.end() && it->second.docSerial == R.docSerial) {
    const TextObject& t = it->second;
    const TextStyle& s = t.style;
    o << "font " << esc(s.family) << '\n' << "fontstyle " << esc(s.style) << '\n';
    o << "text " << s.size << ' ' << s.leading << ' ' << s.kerning << ' ' << s.tracking << ' ' << s.hScale << ' ' << s.vScale << ' '
      << s.baselineShift << ' ' << s.skew << ' ' << s.allCaps << ' ' << s.underline << ' ' << s.strike << ' ' << s.align << ' '
      << s.indentLeft << ' ' << s.indentRight << ' ' << s.indentFirst << ' ' << s.spaceBefore << ' ' << s.spaceAfter << '\n';
    o << "box " << t.box.x << ' ' << t.box.y << ' ' << t.box.width << '\n';
    o << "content " << esc(u32ToUtf8(t.box.text)) << '\n';
    o << "tcolor " << t.color[0] << ' ' << t.color[1] << ' ' << t.color[2] << '\n';
    const BubbleStyle& b = t.bubble;
    o << "bubble " << b.enabled << ' ' << b.shape << ' ' << b.padX << ' ' << b.padY << ' ' << b.corner << ' ' << b.bumps << ' '
      << b.amount << ' ' << b.fill[0] << ' ' << b.fill[1] << ' ' << b.fill[2] << ' ' << b.fillOpacity << ' ' << b.border[0] << ' '
      << b.border[1] << ' ' << b.border[2] << ' ' << b.borderWidth << ' ' << b.borderOpacity << ' ' << b.tailWidth << ' '
      << b.tailMode << '\n';
    for (const BubbleTail& tail : t.tails) {
      o << "tail";
      for (const BubblePt& p : tail) o << ' ' << p.x << ' ' << p.y;
      o << '\n';
    }
  }
  return o.str();
}

// Applies the data to layer `index` (all layers of the document already exist).
void App::applyLayerMeta(int index, const std::string& meta) {
  if (meta.empty() || index < 0 || index >= int(R.layers.size())) return;
  Layer& l = R.layers[size_t(index)];
  std::istringstream in(meta);
  std::string line;
  TextObject t;
  bool hasText = false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string k;
    ls >> k;
    std::string rest;
    if (k == "font" || k == "fontstyle" || k == "content") {
      rest = line.size() > k.size() + 1 ? line.substr(k.size() + 1) : std::string();
      rest = unesc(rest);
    }
    if (k == "tone") {
      ToneFx& f = l.tone;
      ls >> f.on >> f.shape >> f.density >> f.reflectOpacity >> f.posterize >> f.levels >> f.frequency >> f.angle >> f.noiseSize >>
          f.noiseFactor >> f.offX >> f.offY >> f.express;
      f.shape = std::clamp(f.shape, 0, kToneShapes - 1);
    } else if (k == "lcolor") {
      LayerColorFx& c = l.lcolor;
      ls >> c.on >> c.main[0] >> c.main[1] >> c.main[2] >> c.sub[0] >> c.sub[1] >> c.sub[2] >> c.useSub;
    } else if (k == "attached") {
      if (index + 1 < int(R.layers.size())) l.parentId = R.layers[size_t(index + 1)].id;
    } else if (k == "font") {
      t.style.family = rest; hasText = true;
    } else if (k == "fontstyle") {
      t.style.style = rest;
    } else if (k == "text") {
      TextStyle& s = t.style;
      ls >> s.size >> s.leading >> s.kerning >> s.tracking >> s.hScale >> s.vScale >> s.baselineShift >> s.skew >> s.allCaps >>
          s.underline >> s.strike >> s.align >> s.indentLeft >> s.indentRight >> s.indentFirst >> s.spaceBefore >> s.spaceAfter;
      hasText = true;
    } else if (k == "box") {
      ls >> t.box.x >> t.box.y >> t.box.width;
    } else if (k == "content") {
      t.box.text = utf8ToU32(rest);
    } else if (k == "tcolor") {
      ls >> t.color[0] >> t.color[1] >> t.color[2];
    } else if (k == "bubble") {
      BubbleStyle& b = t.bubble;
      ls >> b.enabled >> b.shape >> b.padX >> b.padY >> b.corner >> b.bumps >> b.amount >> b.fill[0] >> b.fill[1] >> b.fill[2] >>
          b.fillOpacity >> b.border[0] >> b.border[1] >> b.border[2] >> b.borderWidth >> b.borderOpacity >> b.tailWidth >> b.tailMode;
    } else if (k == "tail") {
      BubbleTail tail;
      BubblePt p;
      while (ls >> p.x >> p.y) tail.push_back(p);
      if (tail.size() >= 2) t.tails.push_back(std::move(tail));
    }
  }
  if (hasText) {
    t.docSerial = R.docSerial;
    // the bubble is the sublayer right below
    if (index > 0 && R.layers[size_t(index - 1)].parentId == l.id) t.bubbleLayerId = R.layers[size_t(index - 1)].id;
    textObjects[l.id] = std::move(t);
  }
}
