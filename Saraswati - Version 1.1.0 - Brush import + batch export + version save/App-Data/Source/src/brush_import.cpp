// Brush import: Photoshop .abr (v1, v2, v6-v10) and Clip Studio Paint .sut.
// Tips, patterns and settings are mapped onto the Saraswati brush engine; whatever cannot be
// reproduced is listed in the brush's notes (shown after the import), never silently dropped.
#include "brush_import.h"
#include "stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------------------
// Big-endian reader with bounds checks
struct BE {
  const uint8_t* d;
  size_t n, p = 0;
  bool bad = false;
  BE(const uint8_t* data, size_t size) : d(data), n(size) {}
  bool need(size_t k) { if (p + k > n) { bad = true; return false; } return true; }
  uint8_t u8() { return need(1) ? d[p++] : 0; }
  uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(d[p] << 8 | d[p + 1]); p += 2; return v; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v = uint32_t(d[p]) << 24 | uint32_t(d[p + 1]) << 16 | uint32_t(d[p + 2]) << 8 | d[p + 3]; p += 4; return v; }
  int32_t i32() { return int32_t(u32()); }
  double f64() { uint64_t h = u32(), l = u32(); uint64_t b = h << 32 | l; double v; memcpy(&v, &b, 8); return v; }
  void skip(size_t k) { if (need(k)) p += k; }
  std::string raw(size_t k) { if (!need(k)) return {}; std::string s(reinterpret_cast<const char*>(d + p), k); p += k; return s; }
};

std::string utf16be(BE& r, uint32_t count) {
  std::string out;
  for (uint32_t i = 0; i < count && !r.bad; ++i) {
    uint32_t c = r.u16();
    if (c == 0) continue;
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 63)); }
    else { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 63)); out += char(0x80 | (c & 63)); }
  }
  return out;
}

// PackBits rows (PSD / ABR RLE)
bool unpackRows(BE& r, uint32_t w, uint32_t h, bool longCounts, std::vector<uint8_t>& out) {
  std::vector<uint32_t> counts(h);
  for (uint32_t y = 0; y < h; ++y) counts[y] = longCounts ? r.u32() : r.u16();
  out.assign(size_t(w) * h, 0);
  for (uint32_t y = 0; y < h && !r.bad; ++y) {
    size_t end = r.p + counts[y];
    size_t x = 0;
    while (r.p < end && !r.bad) {
      int8_t c = int8_t(r.u8());
      if (c >= 0) {
        for (int k = 0; k <= c && r.p < end; ++k) { uint8_t v = r.u8(); if (x < w) out[size_t(y) * w + x] = v; ++x; }
      } else if (c != -128) {
        uint8_t v = r.u8();
        for (int k = 0; k < 1 - c; ++k) { if (x < w) out[size_t(y) * w + x] = v; ++x; }
      }
    }
    r.p = end;
  }
  return !r.bad;
}

// ---------------------------------------------------------------------------
// Photoshop action descriptor (the settings of v6+ brushes)
struct DVal {
  std::string type;              // Objc, VlLs, doub, UntF, TEXT, enum, long, bool, ...
  double num = 0;
  bool b = false;
  std::string str, unit;         // text / enum value / unit
  std::vector<std::pair<std::string, DVal>> obj;
  std::vector<DVal> list;
  const DVal* get(const std::string& k) const {
    for (auto& e : obj) if (e.first == k) return &e.second;
    return nullptr;
  }
  double numOr(const std::string& k, double def) const { const DVal* v = get(k); return v && (v->type == "doub" || v->type == "UntF" || v->type == "long") ? v->num : def; }
  bool boolOr(const std::string& k, bool def) const { const DVal* v = get(k); return v && v->type == "bool" ? v->b : def; }
  std::string strOr(const std::string& k) const { const DVal* v = get(k); return v ? v->str : std::string(); }
};

std::string dKey(BE& r) {
  uint32_t n = r.u32();
  if (n == 0) n = 4;
  if (n > 4096) { r.bad = true; return {}; }
  return r.raw(n);
}

bool dObject(BE& r, DVal& out, int depth);
bool dItem(BE& r, const std::string& t, DVal& v, int depth) {
  v.type = t;
  if (depth > 64) { r.bad = true; return false; }
  if (t == "Objc" || t == "GlbO") return dObject(r, v, depth + 1);
  if (t == "VlLs") {
    uint32_t n = r.u32();
    if (n > 1000000) { r.bad = true; return false; }
    for (uint32_t i = 0; i < n && !r.bad; ++i) {
      std::string it = r.raw(4);
      DVal e;
      dItem(r, it, e, depth + 1);
      v.list.push_back(std::move(e));
    }
    return !r.bad;
  }
  if (t == "doub") { v.num = r.f64(); return true; }
  if (t == "UntF") { v.unit = r.raw(4); v.num = r.f64(); return true; }
  if (t == "TEXT") { v.str = utf16be(r, r.u32()); return true; }
  if (t == "enum") { dKey(r); v.str = dKey(r); return true; }
  if (t == "long") { v.num = r.i32(); return true; }
  if (t == "comp") { v.num = double(int64_t(uint64_t(r.u32()) << 32 | r.u32())); return true; }
  if (t == "bool") { v.b = r.u8() != 0; return true; }
  if (t == "type" || t == "GlbC") { utf16be(r, r.u32()); v.str = dKey(r); return true; }
  if (t == "tdta" || t == "alis" || t == "Pth ") { r.skip(r.u32()); return true; }
  if (t == "obj ") {  // reference: skip its items
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && !r.bad; ++i) {
      std::string k = r.raw(4);
      if (k == "prop") { utf16be(r, r.u32()); dKey(r); dKey(r); }
      else if (k == "Clss") { utf16be(r, r.u32()); dKey(r); }
      else if (k == "Enmr") { utf16be(r, r.u32()); dKey(r); dKey(r); dKey(r); }
      else if (k == "rele") { utf16be(r, r.u32()); dKey(r); r.u32(); }
      else if (k == "Idnt" || k == "indx") r.u32();
      else if (k == "name") { utf16be(r, r.u32()); dKey(r); utf16be(r, r.u32()); }
      else { r.bad = true; }
    }
    return !r.bad;
  }
  r.bad = true;  // unknown type: stop (the rest cannot be parsed reliably)
  return false;
}

bool dObject(BE& r, DVal& out, int depth) {
  utf16be(r, r.u32());  // display name
  out.str = dKey(r);    // class id
  uint32_t n = r.u32();
  if (n > 100000) { r.bad = true; return false; }
  for (uint32_t i = 0; i < n && !r.bad; ++i) {
    std::string k = dKey(r);
    std::string t = r.raw(4);
    DVal v;
    dItem(r, t, v, depth);
    out.obj.push_back({k, std::move(v)});
  }
  return !r.bad;
}

// ---------------------------------------------------------------------------
// ABR sections

struct Gray { uint32_t w = 0, h = 0; std::vector<uint8_t> a; };

void parseSamples(const uint8_t* d, size_t n, int sub, std::map<std::string, Gray>& out, std::vector<std::string>& order) {
  BE r(d, n);
  while (r.p + 4 <= n && !r.bad) {
    uint32_t size = r.u32();
    size_t start = r.p, end = start + size;
    while ((end - start) % 4) ++end;  // padded to 4
    if (end > n) break;
    std::string uuid = r.raw(37);  // pascal string: '$' + 36-char id
    if (uuid.size() == 37) uuid = uuid.substr(1);
    r.skip(sub == 1 ? 10 : 264);
    int32_t top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    uint16_t depth = r.u16();
    uint8_t comp = r.u8();
    int32_t w = right - left, h = bottom - top;
    if (w > 0 && h > 0 && w <= 30000 && h <= 30000 && (depth == 8 || depth == 16) && !r.bad) {
      Gray g;
      g.w = uint32_t(w);
      g.h = uint32_t(h);
      if (depth == 8) {
        if (comp == 0) { std::string s = r.raw(size_t(w) * h); g.a.assign(s.begin(), s.end()); }
        else unpackRows(r, g.w, g.h, false, g.a);
      } else if (comp == 0) {  // 16-bit: keep the high bytes
        g.a.resize(size_t(w) * h);
        for (size_t i = 0; i < g.a.size() && !r.bad; ++i) g.a[i] = uint8_t(r.u16() >> 8);
      }
      if (g.a.size() == size_t(w) * h) { out[uuid] = std::move(g); order.push_back(uuid); }
    }
    r.bad = false;
    r.p = end;
  }
}

void parsePatterns(const uint8_t* d, size_t n, std::map<std::string, Gray>& out, std::map<std::string, std::string>& names) {
  BE r(d, n);
  while (r.p + 4 <= n) {
    uint32_t len = r.u32();
    size_t start = r.p, end = start + len;
    while ((end - start) % 4) ++end;
    if (end > n || len < 16) break;
    r.u32();  // version
    uint32_t mode = r.u32();
    uint16_t ph = r.u16(), pw = r.u16();
    std::string name = utf16be(r, r.u32());
    uint8_t idLen = r.u8();
    std::string id = r.raw(idLen);
    if (mode == 2) r.skip(256 * 3);  // indexed: colour table (shown as gray)
    r.u32();  // VMA version
    r.u32();  // VMA length
    r.u32(); r.u32(); r.u32(); r.u32();  // rect
    uint32_t channels = r.u32();
    std::vector<std::vector<uint8_t>> planes;
    for (uint32_t c = 0; c < channels + 2 && !r.bad && r.p < end; ++c) {
      uint32_t written = r.u32();
      if (!written) continue;
      uint32_t clen = r.u32();
      size_t cend = r.p + clen;
      r.u32();  // pixel depth
      int32_t t = r.i32(), l = r.i32(), b = r.i32(), rr = r.i32();
      r.u16();  // depth
      uint8_t comp = r.u8();
      uint32_t w = uint32_t(std::max(0, rr - l)), h = uint32_t(std::max(0, b - t));
      std::vector<uint8_t> plane;
      if (w && h && w <= 30000 && h <= 30000) {
        if (comp == 0) { std::string s = r.raw(size_t(w) * h); plane.assign(s.begin(), s.end()); }
        else unpackRows(r, w, h, false, plane);
      }
      if (plane.size() == size_t(pw) * ph) planes.push_back(std::move(plane));
      r.p = std::min(cend, n);
    }
    if (!planes.empty()) {
      Gray g;
      g.w = pw;
      g.h = ph;
      g.a.resize(size_t(pw) * ph);
      bool rgb = mode == 3 && planes.size() >= 3;
      for (size_t i = 0; i < g.a.size(); ++i)
        g.a[i] = rgb ? uint8_t((planes[0][i] * 77 + planes[1][i] * 150 + planes[2][i] * 29) >> 8) : planes[0][i];
      out[id] = std::move(g);
      names[id] = name;
    }
    r.bad = false;
    r.p = end;
  }
}

std::string baseName(const std::string& path) {
  auto u = fs::u8path(path).stem().u8string();
  return std::string(reinterpret_cast<const char*>(u.c_str()), u.size());
}

// PS "control" (bVTy): 0 off, 1 fade, 2 pen pressure, 3 pen tilt, 4 stylus wheel, 5 rotation,
// 6 initial direction, 7 direction
void dynamics(const DVal* v, int& control, float& jitter) {
  control = 0;
  jitter = 0;
  if (!v) return;
  control = int(v->numOr("bVTy", 0));
  if (const DVal* j = v->get("jitter")) jitter = float(j->num / 100.0);
}
}  // namespace

// ---------------------------------------------------------------------------

bool importAbr(const std::string& path, BrushImport& out, std::string& err) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.size() < 4) { err = "Empty or unreadable file"; return false; }
  BE r(data.data(), data.size());
  uint16_t ver = r.u16();
  std::string group = baseName(path);
  auto addTip = [&](const std::string& name, Gray g, bool invert) {
    ImportedImage im;
    im.name = name;
    im.w = g.w;
    im.h = g.h;
    im.alpha = std::move(g.a);
    if (invert) for (auto& a : im.alpha) a = uint8_t(255 - a);
    out.images.push_back(std::move(im));
    return out.images.size() - 1;
  };
  if (ver == 1 || ver == 2) {  // old brushes: a list of computed / sampled tips
    uint16_t count = r.u16();
    for (uint16_t i = 0; i < count && !r.bad; ++i) {
      uint16_t type = r.u16();
      uint32_t size = r.u32();
      size_t end = r.p + size;
      BrushSettings b;
      b.group = group;
      b.name = group + " " + std::to_string(i + 1);
      b.builtIn = false;
      if (type == 1) {
        r.u32();
        b.spacing = std::max(0.01f, r.u16() / 100.0f);
        b.size = std::max(1.0f, float(r.u16()));
        b.roundness = std::clamp(r.u16() / 100.0f, 0.05f, 1.0f);
        b.angle = float(int16_t(r.u16()));
        b.hardness = std::clamp(r.u16() / 100.0f, 0.0f, 1.0f);
        out.brushes.push_back(b);
      } else if (type == 2) {
        r.u32();
        b.spacing = std::max(0.01f, r.u16() / 100.0f);
        if (ver == 2) { std::string nm = utf16be(r, r.u32()); if (!nm.empty()) b.name = nm; }
        r.u8();  // antialias
        r.skip(8);  // short bounds
        int32_t top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
        uint16_t depth = r.u16();
        uint8_t comp = r.u8();
        Gray g;
        g.w = uint32_t(std::max(0, right - left));
        g.h = uint32_t(std::max(0, bottom - top));
        if (g.w && g.h && depth == 8 && g.w <= 30000 && g.h <= 30000) {
          if (comp == 0) { std::string s = r.raw(size_t(g.w) * g.h); g.a.assign(s.begin(), s.end()); }
          else unpackRows(r, g.w, g.h, false, g.a);
          if (g.a.size() == size_t(g.w) * g.h) {
            b.size = float(std::max(g.w, g.h));
            size_t idx = addTip(b.name, std::move(g), false);
            out.tipRefs.push_back({out.brushes.size(), {idx}, SIZE_MAX, SIZE_MAX});
            out.brushes.push_back(b);
          }
        }
      }
      r.bad = false;
      r.p = std::min(end, data.size());
    }
    if (out.brushes.empty()) { err = "No brushes found in the file"; return false; }
    return true;
  }
  if (ver < 6 || ver > 10) { err = "Unsupported .abr version " + std::to_string(ver); return false; }
  int sub = r.u16();
  std::map<std::string, Gray> samples, patterns;
  std::map<std::string, std::string> patternNames;
  std::vector<std::string> sampleOrder;
  DVal desc;
  bool haveDesc = false;
  while (r.p + 12 <= data.size()) {
    std::string sig = r.raw(4), key = r.raw(4);
    uint32_t len = r.u32();
    if (sig != "8BIM" || r.p + len > data.size()) break;
    const uint8_t* sec = data.data() + r.p;
    if (key == "samp") parseSamples(sec, len, sub, samples, sampleOrder);
    else if (key == "patt") parsePatterns(sec, len, patterns, patternNames);
    else if (key == "desc") {
      BE d(sec, len);
      d.u32();  // descriptor version (16)
      haveDesc = dObject(d, desc, 0) || !desc.obj.empty();
    }
    r.p += len;
    while (r.p % 2) ++r.p;
  }
  std::map<std::string, size_t> tipIndex, patIndex;
  auto tipFor = [&](const std::string& uuid, bool flipX, bool flipY) -> size_t {
    std::string key = uuid + (flipX ? "x" : "") + (flipY ? "y" : "");
    auto it = tipIndex.find(key);
    if (it != tipIndex.end()) return it->second;
    auto s = samples.find(uuid);
    if (s == samples.end()) return SIZE_MAX;
    Gray g = s->second;
    if (flipX || flipY) {  // the preset's fixed flip is baked into the tip
      Gray m = g;
      for (uint32_t y = 0; y < g.h; ++y)
        for (uint32_t x = 0; x < g.w; ++x)
          m.a[size_t(y) * g.w + x] = g.a[size_t(flipY ? g.h - 1 - y : y) * g.w + (flipX ? g.w - 1 - x : x)];
      g = std::move(m);
    }
    size_t idx = addTip(group + "-" + uuid.substr(0, 8), std::move(g), false);
    tipIndex[key] = idx;
    return idx;
  };
  auto patFor = [&](const std::string& id) -> size_t {
    auto it = patIndex.find(id);
    if (it != patIndex.end()) return it->second;
    auto p = patterns.find(id);
    if (p == patterns.end()) return SIZE_MAX;
    std::string nm = patternNames[id].empty() ? "pattern" : patternNames[id];
    size_t idx = addTip(group + "-" + nm, p->second, false);
    patIndex[id] = idx;
    return idx;
  };
  const DVal* list = haveDesc ? desc.get("Brsh") : nullptr;
  if (list && list->type == "VlLs") {
    int n = 0;
    for (const DVal& pr : list->list) {
      ++n;
      const DVal* tip = pr.get("Brsh");
      if (!tip) continue;
      BrushSettings b;
      b.builtIn = false;
      b.group = group;
      b.name = pr.strOr("Nm  ");
      if (b.name.empty()) b.name = group + " " + std::to_string(n);
      std::vector<std::string> notes;
      b.size = float(std::clamp(tip->numOr("Dmtr", 20), 1.0, 5000.0));
      b.spacing = float(std::clamp(tip->numOr("Spcn", 25) / 100.0, 0.01, 10.0));
      b.angle = float(tip->numOr("Angl", 0));
      b.roundness = float(std::clamp(tip->numOr("Rndn", 100) / 100.0, 0.05, 1.0));
      TipRef ref{out.brushes.size(), {}, SIZE_MAX, SIZE_MAX};
      if (tip->str == "computedBrush") {
        b.hardness = float(std::clamp(tip->numOr("Hrdn", 100) / 100.0, 0.0, 1.0));
      } else {
        size_t t = tipFor(tip->strOr("sampledData"), tip->boolOr("flipX", false), tip->boolOr("flipY", false));
        if (t == SIZE_MAX) notes.push_back("tip image missing in the file (round tip used)");
        else ref.tips.push_back(t);
      }
      // shape dynamics
      if (pr.boolOr("useTipDynamics", false)) {
        int c; float j;
        dynamics(pr.get("szVr"), c, j);
        b.pressureSize = c == 2;
        b.sizeJitter = j;
        b.minSize = float(std::clamp(pr.numOr("minimumDiameter", 20) / 100.0, 0.0, 1.0));
        if (c != 0 && c != 2) notes.push_back("size control (fade / tilt / wheel) used as pen pressure");
        if (c != 0 && c != 2) b.pressureSize = true;
        dynamics(pr.get("angleDynamics"), c, j);
        b.angleJitter = j;
        if (c == 6 || c == 7) b.followStroke = true;
        else if (c != 0) notes.push_back("angle control (tilt / rotation / wheel) not reproduced");
        dynamics(pr.get("roundnessDynamics"), c, j);
        b.roundJitter = j * float(1.0 - std::clamp(pr.numOr("minimumRoundness", 25) / 100.0, 0.0, 1.0));
        b.flipXJitter = pr.boolOr("flipX", false);
        b.flipYJitter = pr.boolOr("flipY", false);
      } else {
        b.pressureSize = false;
      }
      if (pr.boolOr("useScatter", false)) {
        int c; float j;
        dynamics(pr.get("scatterDynamics"), c, j);
        b.scatter = j;
        b.bothAxes = pr.boolOr("bothAxes", false);
        b.count = int(std::clamp(pr.numOr("Cnt ", 1), 1.0, 16.0));
        if (pr.get("countDynamics")) { int c2; float j2; dynamics(pr.get("countDynamics"), c2, j2); if (j2 > 0) notes.push_back("count jitter approximated"); }
      }
      if (pr.boolOr("useTexture", false)) {
        const DVal* tx = pr.get("Txtr");
        size_t pt = tx ? patFor(tx->strOr("Idnt")) : SIZE_MAX;
        if (pt == SIZE_MAX) notes.push_back("texture pattern not stored in the file (needs the Photoshop pattern set)");
        else {
          ref.texture = pt;
          b.patScale = float(std::clamp(pr.numOr("textureScale", 100) / 100.0, 0.05, 20.0));
          b.texStrength = float(std::clamp(pr.numOr("textureDepth", 100) / 100.0, 0.0, 1.0));
          b.texInvert = pr.boolOr("InvT", false);
          b.texBrightness = float(std::clamp(pr.numOr("textureBrightness", 0) / 150.0, -1.0, 1.0));
          b.texContrast = float(std::clamp(pr.numOr("textureContrast", 0) / 50.0, -1.0, 1.0));
          std::string mode = pr.get("textureBlendMode") ? pr.get("textureBlendMode")->str : "";
          if (mode == "Sbtr") b.texMode = 1;
          else if (mode == "Drkn") b.texMode = 2;
          else if (mode == "Hght" || mode == "linearHeight") b.texMode = 3;
          else if (!mode.empty() && mode != "Mltp") { b.texMode = 0; notes.push_back("texture mode '" + mode + "' used as multiply"); }
        }
      }
      if (const DVal* dual = pr.get("dualBrush"); dual && dual->boolOr("useDualBrush", false)) {
        const DVal* dt = dual->get("Brsh");
        size_t t = dt && dt->str == "sampledBrush" ? tipFor(dt->strOr("sampledData"), false, false) : SIZE_MAX;
        if (t == SIZE_MAX) notes.push_back("dual brush not reproduced (computed or missing tip)");
        else {
          ref.dual = t;
          b.dualScale = float(std::clamp((dt ? dt->numOr("Dmtr", b.size) : b.size) / std::max(1.0f, b.size), 0.05, 20.0));
          notes.push_back("dual brush approximated (the second tip is multiplied per dab)");
        }
      }
      if (pr.boolOr("usePaintDynamics", false)) {
        int c; float j;
        dynamics(pr.get("opVr"), c, j);
        b.pressureOpacity = c == 2;
        b.flowJitter = j;
        dynamics(pr.get("prVr"), c, j);
        b.pressureFlow = c == 2;
        b.flowJitter = std::max(b.flowJitter, j);
      }
      if (pr.boolOr("useColorDynamics", false)) notes.push_back("colour dynamics not supported yet");
      if (pr.boolOr("Wtdg", false)) notes.push_back("wet edges not supported yet");
      if (pr.boolOr("Nose", false)) notes.push_back("noise not supported yet");
      if (const DVal* g = pr.get("brushGroup"); g && g->boolOr("useBrushGroup", false)) notes.push_back("brush group not supported");
      b.buildUp = pr.boolOr("Rpt ", false);
      if (b.buildUp) b.flow = std::min(b.flow, 0.3f);
      for (size_t k = 0; k < notes.size(); ++k) b.notes += (k ? "; " : "") + notes[k];
      out.tipRefs.push_back(ref);
      out.brushes.push_back(b);
    }
  } else {
    // no settings: one brush per sampled tip
    for (const std::string& id : sampleOrder) {
      BrushSettings b;
      b.builtIn = false;
      b.group = group;
      b.name = group + " " + std::to_string(out.brushes.size() + 1);
      const Gray& g = samples[id];
      b.size = float(std::max(g.w, g.h));
      out.tipRefs.push_back({out.brushes.size(), {tipFor(id, false, false)}, SIZE_MAX, SIZE_MAX});
      b.notes = "brush settings missing in the file (tip only)";
      out.brushes.push_back(b);
    }
  }
  if (out.brushes.empty()) { err = "No brushes found in the file"; return false; }
  return true;
}

// ---------------------------------------------------------------------------
// Minimal read-only SQLite reader: walks table b-trees and decodes records (enough for .sut).

namespace {
struct SqliteFile {
  std::vector<uint8_t> d;
  uint32_t pageSize = 0, usable = 0;
  const uint8_t* page(uint32_t no) const {
    if (no == 0 || size_t(no) * pageSize > d.size()) return nullptr;
    return d.data() + size_t(no - 1) * pageSize;
  }
  static uint64_t varint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0;
    for (int i = 0; i < 9 && p < end; ++i) {
      uint8_t b = *p++;
      if (i == 8) return v << 8 | b;
      v = v << 7 | (b & 0x7f);
      if (!(b & 0x80)) break;
    }
    return v;
  }
  // payload of a cell: local part + overflow chain
  bool payload(const uint8_t* cell, const uint8_t* end, uint64_t size, std::vector<uint8_t>& out) const {
    uint64_t X = usable - 35;
    uint64_t local = size;
    if (size > X) {
      uint64_t M = ((usable - 12) * 32 / 255) - 23;
      uint64_t K = M + ((size - M) % (usable - 4));
      local = K <= X ? K : M;
    }
    if (cell + local > end) return false;
    out.assign(cell, cell + local);
    if (local < size) {
      if (cell + local + 4 > end) return false;
      uint32_t next = uint32_t(cell[local]) << 24 | uint32_t(cell[local + 1]) << 16 | uint32_t(cell[local + 2]) << 8 | cell[local + 3];
      int guard = 0;
      while (next && out.size() < size && guard++ < 1000000) {
        const uint8_t* pg = page(next);
        if (!pg) return false;
        next = uint32_t(pg[0]) << 24 | uint32_t(pg[1]) << 16 | uint32_t(pg[2]) << 8 | pg[3];
        size_t take = size_t(std::min<uint64_t>(usable - 4, size - out.size()));
        out.insert(out.end(), pg + 4, pg + 4 + take);
      }
    }
    return out.size() == size;
  }
  // visits every row (rowid, record bytes) of the table b-tree rooted at `root`
  void walk(uint32_t root, const std::function<void(int64_t, const std::vector<uint8_t>&)>& fn, int depth = 0) const {
    const uint8_t* pg = page(root);
    if (!pg || depth > 40) return;
    size_t hdr = root == 1 ? 100 : 0;
    const uint8_t* h = pg + hdr;
    uint8_t type = h[0];
    uint16_t cells = uint16_t(h[3] << 8 | h[4]);
    const uint8_t* ptrs = h + (type == 0x05 ? 12 : 8);
    const uint8_t* end = pg + pageSize;
    for (uint16_t i = 0; i < cells; ++i) {
      uint16_t off = uint16_t(ptrs[i * 2] << 8 | ptrs[i * 2 + 1]);
      if (off >= pageSize) continue;
      const uint8_t* c = pg + off;
      if (type == 0x05) {  // interior: left child first
        uint32_t child = uint32_t(c[0]) << 24 | uint32_t(c[1]) << 16 | uint32_t(c[2]) << 8 | c[3];
        walk(child, fn, depth + 1);
      } else if (type == 0x0D) {
        uint64_t size = varint(c, end);
        int64_t rowid = int64_t(varint(c, end));
        std::vector<uint8_t> rec;
        if (payload(c, end, size, rec)) fn(rowid, rec);
      }
    }
    if (type == 0x05) walk(uint32_t(h[8]) << 24 | uint32_t(h[9]) << 16 | uint32_t(h[10]) << 8 | h[11], fn, depth + 1);
  }
};

struct SqlValue {
  int kind = 0;  // 0 null, 1 int, 2 float, 3 text, 4 blob
  int64_t i = 0;
  double f = 0;
  std::string s;  // text or blob bytes
  double num() const { return kind == 1 ? double(i) : kind == 2 ? f : 0; }
};

std::vector<SqlValue> decodeRecord(const std::vector<uint8_t>& rec, int64_t rowid) {
  std::vector<SqlValue> out;
  const uint8_t* p = rec.data();
  const uint8_t* end = p + rec.size();
  uint64_t hsize = SqliteFile::varint(p, end);
  const uint8_t* hend = rec.data() + std::min<uint64_t>(hsize, rec.size());
  std::vector<uint64_t> types;
  while (p < hend) types.push_back(SqliteFile::varint(p, hend));
  const uint8_t* b = hend;
  for (uint64_t t : types) {
    SqlValue v;
    auto be = [&](int n) { int64_t x = (n && b < end && (*b & 0x80)) ? -1 : 0; for (int k = 0; k < n && b < end; ++k) x = x << 8 | *b++; return x; };
    if (t == 0) v.kind = 0;
    else if (t <= 6) { static const int sz[7] = {0, 1, 2, 3, 4, 6, 8}; v.kind = 1; v.i = be(sz[t]); }
    else if (t == 7) { v.kind = 2; uint64_t x = uint64_t(be(8)); memcpy(&v.f, &x, 8); }
    else if (t == 8 || t == 9) { v.kind = 1; v.i = int64_t(t - 8); }
    else if (t >= 12) {
      size_t n = size_t((t - (t % 2 ? 13 : 12)) / 2);
      if (b + n > end) n = size_t(end - b);
      v.kind = t % 2 ? 3 : 4;
      v.s.assign(reinterpret_cast<const char*>(b), n);
      b += n;
    }
    out.push_back(std::move(v));
  }
  (void)rowid;
  return out;
}

// column names from "CREATE TABLE name (a INTEGER PRIMARY KEY, b, ...)"
std::vector<std::string> columnsOf(const std::string& sql) {
  std::vector<std::string> cols;
  size_t a = sql.find('('), b = sql.rfind(')');
  if (a == std::string::npos || b == std::string::npos || b <= a) return cols;
  std::string body = sql.substr(a + 1, b - a - 1);
  int depth = 0;
  std::string cur;
  auto flush = [&] {
    size_t s = cur.find_first_not_of(" \t\r\n");
    if (s != std::string::npos) {
      std::string t = cur.substr(s);
      std::string name;
      if (!t.empty() && (t[0] == '"' || t[0] == '`' || t[0] == '[')) {
        char close = t[0] == '[' ? ']' : t[0];
        size_t e = t.find(close, 1);
        name = t.substr(1, e == std::string::npos ? std::string::npos : e - 1);
      } else {
        name = t.substr(0, t.find_first_of(" \t\r\n"));
      }
      std::string up = name;
      for (char& c : up) c = char(std::toupper((unsigned char)c));
      if (up != "PRIMARY" && up != "UNIQUE" && up != "CHECK" && up != "FOREIGN" && up != "CONSTRAINT") cols.push_back(name);
    }
    cur.clear();
  };
  for (char c : body) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == ',' && depth == 0) flush();
    else cur += c;
  }
  flush();
  return cols;
}

struct Table {
  std::vector<std::string> cols;
  std::vector<std::vector<SqlValue>> rows;
  int col(const std::string& n) const {
    for (size_t i = 0; i < cols.size(); ++i) if (cols[i] == n) return int(i);
    return -1;
  }
};

bool readTable(const SqliteFile& db, const std::string& name, Table& t) {
  uint32_t root = 0;
  std::string sql;
  db.walk(1, [&](int64_t, const std::vector<uint8_t>& rec) {
    auto v = decodeRecord(rec, 0);
    if (v.size() >= 5 && v[0].s == "table" && v[1].s == name) { root = uint32_t(v[3].i); sql = v[4].s; }
  });
  if (!root) return false;
  t.cols = columnsOf(sql);
  int pk = -1;  // INTEGER PRIMARY KEY is stored as the rowid
  {
    size_t a = sql.find('(');
    std::string body = sql.substr(a == std::string::npos ? 0 : a);
    std::string up = body;
    for (char& c : up) c = char(std::toupper((unsigned char)c));
    size_t k = up.find("INTEGER PRIMARY KEY");
    if (k != std::string::npos) {
      std::string before = body.substr(0, k);
      size_t comma = before.find_last_of(",(");
      std::string nm = before.substr(comma + 1);
      nm.erase(0, nm.find_first_not_of(" \t\r\n\"`["));
      nm = nm.substr(0, nm.find_first_of(" \t\r\n\"`]"));
      for (size_t i = 0; i < t.cols.size(); ++i) if (t.cols[i] == nm) pk = int(i);
    }
  }
  db.walk(root, [&](int64_t rowid, const std::vector<uint8_t>& rec) {
    auto v = decodeRecord(rec, rowid);
    v.resize(t.cols.size());
    if (pk >= 0 && v[size_t(pk)].kind == 0) { v[size_t(pk)].kind = 1; v[size_t(pk)].i = rowid; }
    t.rows.push_back(std::move(v));
  });
  return true;
}

// tar: file name -> contents
std::map<std::string, std::string> untar(const std::string& blob) {
  std::map<std::string, std::string> files;
  size_t p = 0;
  std::string longName;
  while (p + 512 <= blob.size()) {
    const char* h = blob.data() + p;
    if (h[0] == 0) break;
    std::string name(h, strnlen(h, 100));
    size_t size = size_t(std::strtoull(std::string(h + 124, 12).c_str(), nullptr, 8));
    char type = h[156];
    p += 512;
    if (p + size > blob.size()) break;
    std::string body = blob.substr(p, size);
    if (type == 'L') longName = body.c_str();
    else if (type == '0' || type == 0) {
      files[longName.empty() ? name : longName] = body;
      longName.clear();
    }
    p += (size + 511) / 512 * 512;
  }
  return files;
}

// The tip image of a CSP material: the PNG inside the layer file (rebuilt from its checked
// chunks when the container interleaves other data), else the material's thumbnail.
bool materialImage(const std::string& blob, Gray& g, std::string& source) {
  auto files = untar(blob);
  auto decode = [&](const std::string& png) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png.data()), int(png.size()), &w, &h, &n, 4);
    if (!px) return false;
    g.w = uint32_t(w);
    g.h = uint32_t(h);
    g.a.resize(size_t(w) * h);
    // CSP tips: black (or any colour) on transparent / white -> paint amount
    for (size_t i = 0; i < g.a.size(); ++i) {
      const unsigned char* q = px + i * 4;
      float lum = (q[0] * 0.299f + q[1] * 0.587f + q[2] * 0.114f) / 255.0f;
      g.a[i] = uint8_t(std::clamp(q[3] / 255.0f * (1.0f - lum), 0.0f, 1.0f) * 255 + 0.5f);
    }
    stbi_image_free(px);
    return true;
  };
  for (auto& [name, body] : files) {
    if (name.find("material.layer") == std::string::npos) continue;
    size_t s = body.rfind("\x89PNG\r\n\x1a\n");
    size_t e = body.rfind("IEND");
    if (s != std::string::npos && e != std::string::npos && e > s && decode(body.substr(s, e + 8 - s))) { source = "layer image"; return true; }
    if (s == std::string::npos) continue;
    // rebuild from chunks with a valid length (skip foreign bytes between chunks)
    std::string png = body.substr(s, 8);
    size_t p = s + 8;
    int guard = 0;
    while (p + 12 <= body.size() && guard++ < 100000) {
      uint32_t len = uint32_t((unsigned char)body[p]) << 24 | uint32_t((unsigned char)body[p + 1]) << 16 |
                     uint32_t((unsigned char)body[p + 2]) << 8 | (unsigned char)body[p + 3];
      std::string type = body.substr(p + 4, 4);
      bool known = type == "IHDR" || type == "IDAT" || type == "IEND" || type == "PLTE" || type == "tRNS" || type == "pHYs" ||
                   type == "tEXt" || type == "gAMA" || type == "sRGB" || type == "iCCP";
      if (known && p + 12 + len <= body.size()) {
        png += body.substr(p, 12 + len);
        if (type == "IEND") break;
        p += 12 + len;
      } else {
        size_t nx = body.find("IDAT", p + 1), ne = body.find("IEND", p + 1);
        size_t next = std::min(nx, ne);
        if (next == std::string::npos || next < 4) break;
        p = next - 4;
      }
    }
    if (decode(png)) { source = "layer image (rebuilt)"; return true; }
  }
  for (auto& [name, body] : files)
    if (name.find("thumbnail") != std::string::npos && name.find(".png") != std::string::npos && decode(body)) {
      source = "thumbnail";
      return true;
    }
  return false;
}

// "…:Paint004:afb14569c2-…:data:material.layer" style references inside a pattern blob
std::vector<std::string> materialRefs(const std::string& blob, std::vector<std::string>* names = nullptr) {
  std::vector<std::string> refs;
  std::string cur;
  auto flush = [&] {
    if (cur.find(":data:material.layer") != std::string::npos) refs.push_back(cur);
    else if (names && !cur.empty() && cur.find(':') == std::string::npos && cur.size() > 1) names->push_back(cur);
    cur.clear();
  };
  for (size_t i = 16; i + 1 < blob.size(); i += 2) {
    uint16_t c = uint16_t((unsigned char)blob[i] | (unsigned char)blob[i + 1] << 8);
    if (c >= 32 && c < 127) cur += char(c);
    else flush();
  }
  flush();
  return refs;
}

std::string uuidOf(const std::string& ref) {  // the token before ":data:"
  size_t e = ref.find(":data:");
  size_t s = ref.rfind(':', e == std::string::npos ? std::string::npos : e - 1);
  return (s == std::string::npos || e == std::string::npos) ? ref : ref.substr(s + 1, e - s - 1);
}

// CSP effector blob: flags (0x10 pen pressure, 0x80 random), minimum %, amount %
struct Effector { bool pressure = false, random = false; float minimum = 0, amount = 0; };
Effector effector(const SqlValue& v) {
  Effector e;
  if (v.kind != 4 || v.s.size() < 20) return e;
  auto u = [&](size_t i) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(v.s.data()) + i * 4;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  uint32_t flags = u(2);
  e.pressure = (flags & 0x10) != 0;
  e.random = (flags & 0x80) != 0;
  e.minimum = std::clamp(int32_t(u(3)) / 100.0f, 0.0f, 1.0f);
  e.amount = std::clamp(int32_t(u(4)) / 100.0f, 0.0f, 1.0f);
  return e;
}
}  // namespace

bool importSut(const std::string& path, BrushImport& out, std::string& err) {
  SqliteFile db;
  {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    db.d.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  if (db.d.size() < 100 || memcmp(db.d.data(), "SQLite format 3", 15) != 0) { err = "Not a Clip Studio sub tool (.sut) file"; return false; }
  db.pageSize = uint32_t(db.d[16] << 8 | db.d[17]);
  if (db.pageSize == 1) db.pageSize = 65536;
  db.usable = db.pageSize - db.d[20];
  if (db.pageSize < 512) { err = "Damaged file"; return false; }
  Table node, var, mat;
  if (!readTable(db, "Variant", var) || var.rows.empty()) { err = "No brush settings in the file"; return false; }
  readTable(db, "Node", node);
  readTable(db, "MaterialFile", mat);
  std::string group = baseName(path);
  // materials: decode once
  std::vector<size_t> matImage(mat.rows.size(), SIZE_MAX);
  std::vector<std::string> matKey(mat.rows.size());
  int cData = mat.col("FileData"), cOrig = mat.col("OriginalPath"), cCat = mat.col("CatalogPath"), cUuid = mat.col("MaterialUuid");
  for (size_t i = 0; i < mat.rows.size(); ++i) {
    auto& row = mat.rows[i];
    for (int c : {cOrig, cCat, cUuid}) if (c >= 0) matKey[i] += row[size_t(c)].s + "|";
  }
  std::vector<std::string> sources;
  auto materialFor = [&](const std::string& ref, size_t fallback) -> size_t {
    std::string uuid = uuidOf(ref);
    size_t row = SIZE_MAX;
    for (size_t i = 0; i < mat.rows.size(); ++i)
      if (!uuid.empty() && matKey[i].find(uuid) != std::string::npos) row = i;
    if (row == SIZE_MAX) row = fallback < mat.rows.size() ? fallback : SIZE_MAX;
    if (row == SIZE_MAX || cData < 0) return SIZE_MAX;
    if (matImage[row] != SIZE_MAX) return matImage[row];
    Gray g;
    std::string src;
    if (!materialImage(mat.rows[row][size_t(cData)].s, g, src)) return SIZE_MAX;
    sources.push_back(src);
    ImportedImage im;
    im.name = group + "-" + (uuid.size() > 10 ? uuid.substr(0, 10) : uuid);
    im.w = g.w;
    im.h = g.h;
    im.alpha = std::move(g.a);
    out.images.push_back(std::move(im));
    matImage[row] = out.images.size() - 1;
    return matImage[row];
  };
  // which variant is the brush's current one (Node.NodeVariantID), else every variant
  std::set<int64_t> wanted;
  std::map<int64_t, std::string> nameOf;
  int nv = node.col("NodeVariantID"), nn = node.col("NodeName");
  for (auto& row : node.rows)
    if (nv >= 0 && row[size_t(nv)].kind == 1) { wanted.insert(row[size_t(nv)].i); if (nn >= 0) nameOf[row[size_t(nv)].i] = row[size_t(nn)].s; }
  int cVid = var.col("VariantID");
  size_t matOrder = 0;
  for (auto& row : var.rows) {
    int64_t vid = cVid >= 0 ? row[size_t(cVid)].i : 0;
    if (!wanted.empty() && !wanted.count(vid)) continue;
    auto V = [&](const char* c) -> const SqlValue& {
      static const SqlValue none;
      int i = var.col(c);
      return i >= 0 ? row[size_t(i)] : none;
    };
    auto num = [&](const char* c, double d) { const SqlValue& v = V(c); return v.kind == 1 || v.kind == 2 ? v.num() : d; };
    BrushSettings b;
    b.builtIn = false;
    b.group = group;
    b.name = nameOf.count(vid) ? nameOf[vid] : group;
    std::vector<std::string> notes;
    b.size = float(std::clamp(num("BrushSize", 20), 1.0, 5000.0));
    if (num("BrushSizeUnit", 0) != 0) notes.push_back("size unit (mm / pt) read as pixels");
    b.opacity = float(std::clamp(num("Opacity", 100) / 100.0, 0.0, 1.0));
    b.flow = float(std::clamp(num("BrushFlow", 100) / 100.0, 0.01, 1.0));
    b.hardness = float(std::clamp(num("BrushHardness", 100) / 100.0, 0.0, 1.0));
    b.spacing = num("BrushAutoIntervalType", 0) == 2 ? 0.1f : float(std::clamp(num("BrushInterval", 10) / 100.0, 0.01, 10.0));
    b.roundness = float(std::clamp(num("BrushThickness", 100) / 100.0, 0.05, 1.0));
    b.angle = float(num("BrushRotation", 0));
    int rotEff = int(num("BrushRotationEffector", 0));
    if (rotEff == 3) b.followStroke = true;
    else if (rotEff == 4 || rotEff == 5) b.angleJitter = float(std::clamp(num("BrushRotationRandomScale", 100) / 100.0, 0.0, 1.0));
    Effector es = effector(V("BrushSizeEffector")), eo = effector(V("BrushOpacityEffector")), ef = effector(V("BrushFlowEffector"));
    b.pressureSize = es.pressure;
    b.minSize = es.pressure ? es.minimum : 0.2f;
    if (es.random) b.sizeJitter = 0.5f * es.amount;
    b.pressureOpacity = eo.pressure;
    b.pressureFlow = ef.pressure;
    if (eo.random || ef.random) b.flowJitter = 0.5f;
    TipRef ref{out.brushes.size(), {}, SIZE_MAX, SIZE_MAX};
    if (num("BrushUsePatternImage", 0) != 0) {
      std::vector<std::string> refs = materialRefs(V("BrushPatternImageArray").s);
      for (const std::string& r : refs) {
        size_t t = materialFor(r, matOrder++);
        if (t != SIZE_MAX) ref.tips.push_back(t);
      }
      if (ref.tips.empty()) notes.push_back("tip image could not be read (round tip used)");
      b.tipOrder = num("BrushPatternOrderType", 0) != 0 ? 1 : 0;
      b.flipXJitter = num("BrushPatternReverseHorizontal", 0) != 0;
      b.flipYJitter = num("BrushPatternReverseVertical", 0) != 0;
    }
    if (V("TextureImage").kind == 4 && num("TextureDensity", 0) > 0) {
      std::vector<std::string> refs = materialRefs(V("TextureImage").s);
      size_t t = refs.empty() ? SIZE_MAX : materialFor(refs[0], matOrder++);
      if (t == SIZE_MAX) notes.push_back("texture could not be read");
      else {
        ref.texture = t;
        b.texStrength = float(std::clamp(num("TextureDensity", 100) / 100.0, 0.0, 1.0));
        b.patScale = float(std::clamp(num("TextureScale2", 100) / 100.0, 0.05, 20.0));
        b.texInvert = num("TextureReverseDensity", 0) != 0;
        b.texBrightness = float(std::clamp(num("TextureBrightness", 0) / 100.0, -1.0, 1.0));
        b.texContrast = float(std::clamp(num("TextureContrast", 0) / 100.0, -1.0, 1.0));
        int mode = int(num("TextureCompositeMode", 0));
        b.texMode = mode == 2 ? 1 : mode == 3 ? 2 : 0;
        if (num("TextureRotate", 0) != 0) notes.push_back("texture rotation not supported yet");
      }
    }
    if (num("BrushUseSpray", 0) != 0) {
      b.particles = int(std::clamp(num("BrushSprayDensity", 5), 1.0, 64.0));
      b.particleSize = float(std::clamp(num("BrushSpraySize", 5), 0.5, 500.0));
      b.size = std::max(b.size, b.particleSize * 2);
      if (num("BrushSprayBias", 0) != 0) notes.push_back("spray bias not supported");
    }
    if (num("BrushUseIn", 0) != 0) b.taperIn = float(std::clamp(num("BrushInLength", 0), 0.0, 20000.0));
    if (num("BrushUseOut", 0) != 0) notes.push_back("stroke end taper not supported yet");
    if (num("UseDualBrush", 0) != 0) {
      std::vector<std::string> refs = materialRefs(V("DualPatternImageArray").s);
      size_t t = refs.empty() ? SIZE_MAX : materialFor(refs[0], matOrder++);
      if (t != SIZE_MAX) {
        ref.dual = t;
        b.dualScale = float(std::clamp(num("DualSize", b.size) / std::max(1.0f, b.size), 0.05, 20.0));
        notes.push_back("dual brush approximated (the second tip is multiplied per dab)");
      } else {
        notes.push_back("dual brush not reproduced");
      }
    }
    if (num("BrushUseWaterColor", 0) != 0 || num("BrushMixColor", 0) > 0 && num("BrushUseWaterColor", 0) != 0) notes.push_back("colour mixing (watercolour) not supported yet");
    if (num("BrushBlur", 0) > 0 && num("BrushUseWaterColor", 0) != 0) notes.push_back("blur not supported yet");
    if (num("BrushChangePatternColor", 0) != 0 || num("BrushChangeStrokeColor", 0) != 0) notes.push_back("colour jitter not supported yet");
    if (num("BrushUseWaterEdge", 0) != 0) notes.push_back("watercolour edge not supported yet");
    if (num("BrushRibbon", 0) != 0) notes.push_back("ribbon mode not supported yet");
    for (size_t k = 0; k < notes.size(); ++k) b.notes += (k ? "; " : "") + notes[k];
    out.tipRefs.push_back(ref);
    out.brushes.push_back(b);
  }
  if (!sources.empty()) {
    bool thumb = std::find(sources.begin(), sources.end(), "thumbnail") != sources.end();
    if (thumb) out.fileNotes.push_back("Some tip images were taken from the material previews (the full images are stored in a format Saraswati cannot read yet).");
  }
  if (out.brushes.empty()) { err = "No brush found in the file"; return false; }
  return true;
}
