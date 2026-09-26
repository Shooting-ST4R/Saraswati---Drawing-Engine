#include "text.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

const unsigned char* uiFontData(size_t* size);  // main.cpp: the embedded UI font (Roboto)

// ---------------------------------------------------------------------------
// UTF conversion

std::u32string utf8ToU32(const std::string& s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    char32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
    else { ++i; continue; }
    if (i + n > s.size()) break;
    for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
    out.push_back(cp);
    i += n;
  }
  return out;
}

std::string u32ToUtf8(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 0x3f)); }
    else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3f)); out += char(0x80 | (c & 0x3f)); }
    else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3f)); out += char(0x80 | ((c >> 6) & 0x3f)); out += char(0x80 | (c & 0x3f)); }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Font discovery: read just the table directory, 'name' and 'OS/2' of every face.

namespace {
struct Reader {
  std::ifstream f;
  bool read(uint64_t off, void* dst, size_t n) {
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(static_cast<char*>(dst), std::streamsize(n));
    return size_t(f.gcount()) == n;
  }
};
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string decodeName(const uint8_t* p, size_t n, int platform) {
  std::string out;
  if (platform == 3 || platform == 0) {  // UTF-16BE
    std::u32string u;
    for (size_t i = 0; i + 1 < n; i += 2) {
      char32_t c = be16(p + i);
      if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
        char32_t lo = be16(p + i + 2);
        c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
      u.push_back(c);
    }
    return u32ToUtf8(u);
  }
  for (size_t i = 0; i < n; ++i) out += char(p[i] < 0x80 ? p[i] : '?');  // Mac Roman: ASCII part
  return out;
}

bool readFace(Reader& r, uint32_t off, FontFace& face) {
  uint8_t hdr[12];
  if (!r.read(off, hdr, 12)) return false;
  uint16_t numTables = be16(hdr + 4);
  if (numTables == 0 || numTables > 200) return false;
  std::vector<uint8_t> dir(size_t(numTables) * 16);
  if (!r.read(off + 12, dir.data(), dir.size())) return false;
  uint32_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
  bool outlines = false;
  for (int i = 0; i < numTables; ++i) {
    const uint8_t* e = &dir[size_t(i) * 16];
    if (!memcmp(e, "name", 4)) { nameOff = be32(e + 8); nameLen = be32(e + 12); }
    if (!memcmp(e, "OS/2", 4)) { os2Off = be32(e + 8); os2Len = be32(e + 12); }
    if (!memcmp(e, "glyf", 4) || !memcmp(e, "CFF ", 4)) outlines = true;
  }
  if (!nameOff || !outlines || nameLen > (8u << 20)) return false;  // bitmap-only fonts (and CFF2) are skipped
  std::vector<uint8_t> nt(nameLen);
  if (!r.read(nameOff, nt.data(), nt.size()) || nameLen < 6) return false;
  uint16_t count = be16(&nt[2]), strOff = be16(&nt[4]);
  // best record per name id: Windows English, then any Windows, then Mac
  std::string best[18];
  int bestScore[18] = {};
  for (int i = 0; i < count && 6 + (i + 1) * 12 <= int(nameLen); ++i) {
    const uint8_t* rec = &nt[6 + size_t(i) * 12];
    int platform = be16(rec), lang = be16(rec + 4), id = be16(rec + 6), len = be16(rec + 8), o = be16(rec + 10);
    if (id >= 18 || (id != 1 && id != 2 && id != 16 && id != 17)) continue;
    int score = platform == 3 ? (lang == 0x409 ? 3 : 2) : platform == 1 && lang == 0 ? 1 : 0;
    if (score <= bestScore[id] || size_t(strOff) + o + len > nameLen) continue;
    best[id] = decodeName(&nt[size_t(strOff) + o], size_t(len), platform);
    bestScore[id] = score;
  }
  face.family = !best[16].empty() ? best[16] : best[1];
  face.style = !best[17].empty() ? best[17] : best[2];
  if (face.family.empty()) return false;
  if (face.style.empty()) face.style = "Regular";
  if (os2Off && os2Len >= 64) {
    uint8_t os2[64];
    if (r.read(os2Off, os2, 64)) {
      face.weight = std::clamp<int>(be16(os2 + 4), 1, 1000);
      face.italic = (be16(os2 + 62) & 1) != 0;
    }
  }
  std::string lower = face.style;
  for (char& c : lower) c = char(std::tolower((unsigned char)c));
  if (lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos) face.italic = true;
  return true;
}

void scanFile(const fs::path& p, std::vector<FontFace>& out) {
  Reader r;
  r.f.open(p, std::ios::binary);
  if (!r.f) return;
  uint8_t tag[16];
  if (!r.read(0, tag, 16)) return;
  std::string path = p.u8string().c_str() ? std::string(reinterpret_cast<const char*>(p.u8string().c_str())) : "";
  if (!memcmp(tag, "ttcf", 4)) {
    uint32_t n = std::min<uint32_t>(be32(tag + 8), 64);
    std::vector<uint8_t> offs(size_t(n) * 4);
    if (!r.read(12, offs.data(), offs.size())) return;
    for (uint32_t i = 0; i < n; ++i) {
      FontFace f;
      f.path = path;
      f.index = int(i);
      if (readFace(r, be32(&offs[size_t(i) * 4]), f)) out.push_back(f);
    }
  } else {
    FontFace f;
    f.path = path;
    if (readFace(r, 0, f)) out.push_back(f);
  }
}

std::vector<fs::path> fontDirectories() {
  std::vector<fs::path> dirs;
#ifdef _WIN32
  if (const wchar_t* w = _wgetenv(L"WINDIR")) dirs.push_back(fs::path(w) / L"Fonts");
  if (const wchar_t* l = _wgetenv(L"LOCALAPPDATA")) dirs.push_back(fs::path(l) / L"Microsoft" / L"Windows" / L"Fonts");
#else
  dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
  if (const char* h = getenv("HOME")) {
    dirs.push_back(fs::path(h) / ".local/share/fonts");
    dirs.push_back(fs::path(h) / ".fonts");
  }
  if (const char* x = getenv("XDG_DATA_HOME")) dirs.push_back(fs::path(x) / "fonts");
#endif
  return dirs;
}

#ifdef _WIN32
// fonts registered from other folders (e.g. installed by an application)
void registryFonts(std::vector<fs::path>& files) {
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    HKEY key;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
    wchar_t name[512];
    wchar_t data[1024];
    for (DWORD i = 0;; ++i) {
      DWORD nameLen = 512, dataLen = sizeof data, type = 0;
      if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen) != ERROR_SUCCESS) break;
      if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
      data[std::min<DWORD>(dataLen / sizeof(wchar_t), 1023)] = 0;
      fs::path p(data);
      if (p.is_absolute()) files.push_back(p);
    }
    RegCloseKey(key);
  }
}
#endif

int styleRank(const FontFace& f) { return f.weight * 2 + (f.italic ? 1 : 0); }
}  // namespace

FontLibrary::~FontLibrary() {
  if (thread_.joinable()) thread_.join();
}

FontFace FontLibrary::builtIn() {
  FontFace f;
  f.family = "Roboto (built-in)";
  f.style = "Medium";
  f.weight = 500;
  return f;
}

void FontLibrary::startScan(const std::string& cacheFile) {
  if (thread_.joinable() || ready_) return;
  thread_ = std::thread([this, cacheFile] { scan(cacheFile); });
}

void FontLibrary::scan(std::string cacheFile) {
  // cache: path \t mtime \t index \t weight \t italic \t family \t style
  std::unordered_map<std::string, std::pair<long long, std::vector<FontFace>>> cache;
  if (!cacheFile.empty()) {
    std::ifstream in(fs::u8path(cacheFile));
    std::string line;
    while (std::getline(in, line)) {
      std::vector<std::string> f;
      size_t a = 0;
      for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
      f.push_back(line.substr(a));
      if (f.size() != 7) continue;
      FontFace face;
      face.path = f[0];
      face.index = atoi(f[2].c_str());
      face.weight = atoi(f[3].c_str());
      face.italic = f[4] == "1";
      face.family = f[5];
      face.style = f[6];
      auto& e = cache[f[0]];
      e.first = atoll(f[1].c_str());
      if (!face.family.empty()) e.second.push_back(face);
    }
  }
  std::vector<fs::path> files;
  std::error_code ec;
  for (const fs::path& d : fontDirectories()) {
    if (!fs::is_directory(d, ec)) continue;
    for (fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec)) files.push_back(it->path());
    }
  }
#ifdef _WIN32
  registryFonts(files);
#endif
  std::vector<FontFace> faces;
  std::set<std::string> seen;
  std::string cacheOut;
  for (const fs::path& p : files) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".otc") continue;
    std::string key = reinterpret_cast<const char*>(p.u8string().c_str());
    if (!seen.insert(key).second) continue;
    long long mt = 0;
    auto t = fs::last_write_time(p, ec);
    if (!ec) mt = (long long)t.time_since_epoch().count();
    std::vector<FontFace> found;
    auto c = cache.find(key);
    if (c != cache.end() && c->second.first == mt) found = c->second.second;
    else scanFile(p, found);
    if (found.empty()) cacheOut += key + "\t" + std::to_string(mt) + "\t0\t0\t0\t\t\n";  // remember unusable files too
    for (FontFace& f : found) {
      cacheOut += key + "\t" + std::to_string(mt) + "\t" + std::to_string(f.index) + "\t" + std::to_string(f.weight) + "\t" +
                  (f.italic ? "1" : "0") + "\t" + f.family + "\t" + f.style + "\n";
      faces.push_back(std::move(f));
    }
  }
  if (!cacheFile.empty()) {
    std::ofstream o(fs::u8path(cacheFile), std::ios::binary);
    o << cacheOut;
  }
  faces.push_back(builtIn());
  std::map<std::string, FontFamily, std::less<>> byName;
  for (FontFace& f : faces) {
    FontFamily& fam = byName[f.family];
    fam.name = f.family;
    bool dup = false;
    for (auto& g : fam.faces) dup |= g.style == f.style;
    if (!dup) fam.faces.push_back(std::move(f));
  }
  std::vector<FontFamily> fams;
  for (auto& kv : byName) {
    std::sort(kv.second.faces.begin(), kv.second.faces.end(), [](const FontFace& a, const FontFace& b) { return styleRank(a) < styleRank(b); });
    fams.push_back(std::move(kv.second));
  }
  std::sort(fams.begin(), fams.end(), [](const FontFamily& a, const FontFamily& b) {
    std::string x = a.name, y = b.name;
    for (char& c : x) c = char(std::tolower((unsigned char)c));
    for (char& c : y) c = char(std::tolower((unsigned char)c));
    return x < y;
  });
  families_ = std::move(fams);
  ready_ = true;
}

const FontFamily* FontLibrary::family(const std::string& name) const {
  if (!ready_) return nullptr;
  for (const FontFamily& f : families_)
    if (f.name == name) return &f;
  return nullptr;
}

const FontFace* FontLibrary::face(const std::string& fam, const std::string& style) const {
  const FontFamily* f = family(fam);
  if (!f || f->faces.empty()) return nullptr;
  for (const FontFace& x : f->faces)
    if (x.style == style) return &x;
  for (const FontFace& x : f->faces)  // closest: Regular / Book / Normal, else the first
    if (x.weight == 400 && !x.italic) return &x;
  return &f->faces[0];
}

// ---------------------------------------------------------------------------
// Loaded fonts

struct LoadedFont {
  std::vector<uint8_t> data;
  const unsigned char* bytes = nullptr;
  stbtt_fontinfo info{};
  float lastSx = 0, lastSy = 0;
  struct Glyph { int x0, y0, w, h; std::vector<uint8_t> px; };
  std::unordered_map<uint64_t, Glyph> cache;  // glyph + subpixel step, for the current scale
  const Glyph& glyph(int g, float sx, float sy, int sub) {
    if (sx != lastSx || sy != lastSy || cache.size() > 4000) { cache.clear(); lastSx = sx; lastSy = sy; }
    uint64_t key = uint64_t(g) << 3 | uint64_t(sub);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Glyph gl{};
    float shift = sub / 4.0f;
    int x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&info, g, sx, sy, shift, 0, &gl.x0, &gl.y0, &x1, &y1);
    gl.w = std::max(0, x1 - gl.x0);
    gl.h = std::max(0, y1 - gl.y0);
    if (gl.w > 0 && gl.h > 0 && size_t(gl.w) * gl.h < (64u << 20)) {
      gl.px.resize(size_t(gl.w) * gl.h);
      stbtt_MakeGlyphBitmapSubpixel(&info, gl.px.data(), gl.w, gl.h, gl.w, sx, sy, shift, 0, g);
    } else {
      gl.w = gl.h = 0;
    }
    return cache.emplace(key, std::move(gl)).first->second;
  }
};

std::shared_ptr<LoadedFont> FontLibrary::load(const FontFace& f) {
  std::string key = f.path + "#" + std::to_string(f.index);
  auto it = loaded_.find(key);
  if (it != loaded_.end())
    if (auto sp = it->second.lock()) return sp;
  auto lf = std::make_shared<LoadedFont>();
  if (f.path.empty()) {
    size_t n = 0;
    lf->bytes = uiFontData(&n);
  } else {
    std::ifstream in(fs::u8path(f.path), std::ios::binary);
    if (!in) return nullptr;
    lf->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    lf->bytes = lf->data.data();
  }
  int off = stbtt_GetFontOffsetForIndex(lf->bytes, f.index);
  if (off < 0 || !stbtt_InitFont(&lf->info, lf->bytes, off)) return nullptr;
  loaded_[key] = lf;
  return lf;
}

// ---------------------------------------------------------------------------
// Layout

namespace {
struct Placed { int glyph; double x; int ci; };  // pen x relative to the line start
struct Line {
  int first = 0, last = 0;  // character range [first, last)
  double width = 0;         // without trailing spaces
  double baseline = 0;
  double startX = 0;        // document x of the line start
  double spaceExtra = 0;    // justify: added per space
  bool paraStart = false, paraEnd = false;
};
}  // namespace

bool layoutText(LoadedFont* font, const TextStyle& st, const TextBox& box, TextLayout& out, bool raster) {
  out = TextLayout{};
  if (!font) return false;
  const stbtt_fontinfo* fi = &font->info;
  float scale = stbtt_ScaleForMappingEmToPixels(fi, std::max(1.0f, st.size));
  float sx = scale * st.hScale / 100.0f, sy = scale * st.vScale / 100.0f;
  int asc, desc, gap;
  stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
  double ascent = asc * sy, descent = -desc * sy;
  double leading = st.leading > 0 ? st.leading : st.size * 1.2 * st.vScale / 100.0;
  double track = st.tracking * st.size / 1000.0;
  std::u32string text = box.text;
  if (st.allCaps)
    for (char32_t& c : text)
      if (c < 0x10000) c = char32_t(std::towupper(wint_t(c)));
  size_t n = text.size();
  // advance of each character (with kerning to the next one)
  std::vector<int> glyphs(n);
  std::vector<double> adv(n, 0);
  for (size_t i = 0; i < n; ++i) {
    char32_t c = text[i];
    if (c == U'\n') continue;
    glyphs[i] = stbtt_FindGlyphIndex(fi, int(c == U'\t' ? U' ' : c));
    int a, lsb;
    stbtt_GetGlyphHMetrics(fi, glyphs[i], &a, &lsb);
    adv[i] = a * sx + track;
    if (c == U'\t') adv[i] *= 4;
    if (st.kerning && i + 1 < n && text[i + 1] != U'\n')
      adv[i] += stbtt_GetGlyphKernAdvance(fi, glyphs[i], stbtt_FindGlyphIndex(fi, int(text[i + 1]))) * sx;
  }
  // break into lines
  std::vector<Line> lines;
  bool area = box.width > 0;
  size_t i = 0;
  bool paraStart = true;
  while (true) {
    Line L;
    L.first = int(i);
    L.paraStart = paraStart;
    double avail = box.width - st.indentLeft - st.indentRight - (paraStart ? st.indentFirst : 0);
    double w = 0;
    size_t lastBreak = std::string::npos;
    size_t j = i;
    for (; j < n && text[j] != U'\n'; ++j) {
      if (area && w + adv[j] > avail && j > i && text[j] != U' ') {
        if (lastBreak != std::string::npos) j = lastBreak + 1;  // after the last space
        break;
      }
      if (text[j] == U' ') lastBreak = j;
      w += adv[j];
    }
    L.last = int(j);
    bool hardEnd = j >= n || text[j] == U'\n';
    L.paraEnd = hardEnd;
    double ww = 0, lastInk = 0;
    for (int k = L.first; k < L.last; ++k) {
      ww += adv[k];
      if (text[k] != U' ') lastInk = ww;
    }
    L.width = lastInk;
    lines.push_back(L);
    if (j >= n) break;
    i = hardEnd ? j + 1 : j;
    paraStart = hardEnd;
  }
  // vertical placement and alignment
  double y = box.y;
  for (size_t li = 0; li < lines.size(); ++li) {
    Line& L = lines[li];
    if (L.paraStart && li > 0) y += st.spaceBefore;
    L.baseline = (li == 0 ? y + ascent : y + leading);
    y = L.baseline;
    if (L.paraEnd) y += st.spaceAfter;
    double indent = st.indentLeft + (L.paraStart ? st.indentFirst : 0);
    if (area) {
      double avail = box.width - st.indentRight - indent;
      double slack = avail - L.width;
      if (st.align == 1) L.startX = box.x + indent + slack / 2;
      else if (st.align == 2) L.startX = box.x + indent + slack;
      else L.startX = box.x + indent;
      if (st.align == 3 && !L.paraEnd) {
        int spaces = 0;
        int end = L.last;
        while (end > L.first && text[end - 1] == U' ') --end;
        for (int k = L.first; k < end; ++k) spaces += text[k] == U' ';
        if (spaces) L.spaceExtra = slack / spaces;
      }
    } else {  // point text: aligned around the anchor
      if (st.align == 1) L.startX = box.x - L.width / 2;
      else if (st.align == 2) L.startX = box.x - L.width;
      else L.startX = box.x + indent;
    }
  }
  // carets and glyph positions
  out.carets.resize(n + 1);
  std::vector<Placed> placed;
  double minX = 1e30, maxX = -1e30;
  for (size_t li = 0; li < lines.size(); ++li) {
    const Line& L = lines[li];
    double x = L.startX;
    double top = L.baseline - ascent, bottom = L.baseline + descent;
    for (int k = L.first; k < L.last; ++k) {
      out.carets[k] = {x, top, bottom, int(li)};
      placed.push_back({glyphs[k], x, k});
      x += adv[k] + (text[k] == U' ' ? L.spaceExtra : 0);
    }
    out.carets[L.last] = {x, top, bottom, int(li)};  // end of line (before the break / newline)
    minX = std::min(minX, L.startX);
    maxX = std::max(maxX, L.startX + L.width);
  }
  if (n > 0 && text[n - 1] == U'\n') {  // caret on the empty last line
    const Line& L = lines.back();
    out.carets[n] = {L.startX, L.baseline - ascent, L.baseline + descent, int(lines.size() - 1)};
  }
  const Line& last = lines.back();
  if (area) { out.boxX0 = box.x; out.boxX1 = box.x + box.width; }
  else { out.boxX0 = std::min(minX, box.x); out.boxX1 = std::max(maxX, box.x + 1); }
  out.boxY0 = box.y;
  out.boxY1 = last.baseline + descent;
  if (!raster) return true;
  // rasterise
  double shear = std::tan(st.skew * 3.14159265358979323846 / 180.0);
  struct Stamp { const LoadedFont::Glyph* g; int ox, oy; double base; };
  std::vector<Stamp> stamps;
  int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
  auto lineOf = [&](int ci) -> const Line& { return lines[size_t(out.carets[ci].line)]; };
  for (const Placed& p : placed) {
    char32_t c = text[size_t(p.ci)];
    if (c == U' ' || c == U'\t') continue;
    double base = lineOf(p.ci).baseline - st.baselineShift;
    double px = p.x;
    int ix = int(std::floor(px));
    int sub = std::clamp(int((px - ix) * 4), 0, 3);
    const LoadedFont::Glyph& g = font->glyph(p.glyph, sx, sy, sub);
    if (!g.w) continue;
    int iy = int(std::lround(base));
    stamps.push_back({&g, ix + g.x0, iy + g.y0, base});
    double s0 = shear * (base - (iy + g.y0)), s1 = shear * (base - (iy + g.y0 + g.h));
    bx0 = std::min(bx0, ix + g.x0 + int(std::floor(std::min(s0, s1))) - 1);
    bx1 = std::max(bx1, ix + g.x0 + g.w + int(std::ceil(std::max(s0, s1))) + 1);
    by0 = std::min(by0, iy + g.y0);
    by1 = std::max(by1, iy + g.y0 + g.h);
  }
  // underline / strike-through bars per line
  struct Bar { double x0, x1, y, t; };
  std::vector<Bar> bars;
  double thick = std::max(1.0, st.size * 0.055 * st.vScale / 100.0);
  for (const Line& L : lines) {
    if (L.width <= 0) continue;
    double w = L.width + L.spaceExtra * 0;  // justified lines: full width
    if (L.spaceExtra > 0) w = (out.carets[L.last].x - L.startX);
    double base = L.baseline - st.baselineShift;
    if (st.underline) bars.push_back({L.startX, L.startX + w, base + st.size * 0.12 * st.vScale / 100.0, thick});
    if (st.strike) bars.push_back({L.startX, L.startX + w, base - st.size * 0.28 * st.vScale / 100.0, thick});
  }
  for (const Bar& b : bars) {
    bx0 = std::min(bx0, int(std::floor(b.x0)) - 1);
    bx1 = std::max(bx1, int(std::ceil(b.x1)) + 1);
    by0 = std::min(by0, int(std::floor(b.y - b.t / 2)) - 1);
    by1 = std::max(by1, int(std::ceil(b.y + b.t / 2)) + 1);
  }
  if (bx0 >= bx1 || by0 >= by1) return true;
  out.x0 = bx0;
  out.y0 = by0;
  out.w = uint32_t(bx1 - bx0);
  out.h = uint32_t(by1 - by0);
  if (size_t(out.w) * out.h > (1ull << 31)) { out.w = out.h = 0; return true; }
  out.cov.assign(size_t(out.w) * out.h, 0);
  for (const Stamp& s : stamps) {
    const LoadedFont::Glyph& g = *s.g;
    for (int y = 0; y < g.h; ++y) {
      int dy = s.oy + y - out.y0;
      double off = shear * (s.base - (s.oy + y + 0.5));  // skew: shift each row by its height above the baseline
      int io = int(std::floor(off));
      float f = float(off - io);
      uint8_t* row = &out.cov[size_t(dy) * out.w];
      const uint8_t* src = &g.px[size_t(y) * g.w];
      for (int x = 0; x < g.w; ++x) {
        if (!src[x]) continue;
        int dx = s.ox + x + io - out.x0;
        float v0 = src[x] * (1 - f), v1 = src[x] * f;
        if (dx >= 0 && dx < int(out.w)) row[dx] = uint8_t(std::min(255.0f, row[dx] + v0));
        if (dx + 1 >= 0 && dx + 1 < int(out.w) && v1 > 0) row[dx + 1] = uint8_t(std::min(255.0f, row[dx + 1] + v1));
      }
    }
  }
  for (const Bar& b : bars) {
    double y0 = b.y - b.t / 2, y1 = b.y + b.t / 2;
    for (int y = int(std::floor(y0)); y < int(std::ceil(y1)); ++y) {
      float cy = float(std::min<double>(y + 1, y1) - std::max<double>(y, y0));
      if (y - out.y0 < 0 || y - out.y0 >= int(out.h) || cy <= 0) continue;
      for (int x = int(std::floor(b.x0)); x < int(std::ceil(b.x1)); ++x) {
        float cx = float(std::min<double>(x + 1, b.x1) - std::max<double>(x, b.x0));
        if (x - out.x0 < 0 || x - out.x0 >= int(out.w) || cx <= 0) continue;
        uint8_t& d = out.cov[size_t(y - out.y0) * out.w + (x - out.x0)];
        d = uint8_t(std::max<float>(d, std::min(1.0f, cx * cy) * 255));
      }
    }
  }
  return true;
}
