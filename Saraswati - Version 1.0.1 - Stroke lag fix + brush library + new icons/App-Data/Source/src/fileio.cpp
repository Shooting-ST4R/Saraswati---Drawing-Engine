#include "fileio.h"

#include "nanosvg.h"
#include "nanosvgrast.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Utilities

static FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
  fs::path p(reinterpret_cast<const char8_t*>(path.c_str()));
  std::wstring wmode(mode, mode + strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return fopen(path.c_str(), mode);
#endif
}

// Everything written is on the disk before the file is renamed into place (power loss / crash
// can never leave a half-written file under the real name).
static bool syncAndClose(FILE* f) {
  bool ok = fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  return (fclose(f) == 0) && ok;
}

// Atomically replaces `path` with `tmp` (the old file stays intact if anything fails).
bool commitFileReplace(const std::string& tmp, const std::string& path, std::string& err) {
  fs::path from(reinterpret_cast<const char8_t*>(tmp.c_str())), to(reinterpret_cast<const char8_t*>(path.c_str()));
#ifdef _WIN32
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  err = "Could not replace " + path + " (error " + std::to_string(GetLastError()) + "). The new file was kept as " + tmp;
  return false;
#else
  std::error_code ec;
  fs::rename(from, to, ec);  // POSIX rename replaces atomically
  if (!ec) return true;
  err = "Could not replace " + path + ": " + ec.message() + ". The new file was kept as " + tmp;
  return false;
#endif
}

static bool seek64(FILE* f, int64_t off) {
#ifdef _WIN32
  return _fseeki64(f, off, SEEK_SET) == 0;
#else
  return fseeko(f, off_t(off), SEEK_SET) == 0;
#endif
}
static int64_t tell64(FILE* f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return int64_t(ftello(f));
#endif
}

static std::string lowerExt(const std::string& path) {
  std::string e = fs::path(reinterpret_cast<const char8_t*>(path.c_str())).extension().string();
  for (auto& c : e) c = char(std::tolower((unsigned char)c));
  return e;
}

bool isPsdPath(const std::string& path) {
  std::string e = lowerExt(path);
  return e == ".psd" || e == ".psb";
}

void premultiply(std::vector<uint8_t>& p) {
  for (size_t i = 0; i + 3 < p.size(); i += 4) {
    unsigned a = p[i + 3];
    if (a == 255) continue;
    for (int k = 0; k < 3; ++k) p[i + k] = uint8_t((p[i + k] * a + 127) / 255);
  }
}

void unpremultiply(std::vector<uint8_t>& p) {
  for (size_t i = 0; i + 3 < p.size(); i += 4) {
    unsigned a = p[i + 3];
    if (a == 255) continue;
    if (a == 0) { p[i] = p[i + 1] = p[i + 2] = 0; continue; }
    for (int k = 0; k < 3; ++k) p[i + k] = uint8_t(std::min(255u, (p[i + k] * 255 + a / 2) / a));
  }
}

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& data, std::string& err) {
  FILE* f = openUtf8(path, "rb");
  if (!f) { err = "Cannot open " + path; return false; }
  seek64(f, 0);
  fseek(f, 0, SEEK_END);
  int64_t n = tell64(f);
  seek64(f, 0);
  data.resize(size_t(std::max<int64_t>(0, n)));
  bool ok = n >= 0 && fread(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  if (!ok) err = "Read error: " + path;
  return ok;
}

// ---------------------------------------------------------------------------
// Image import

bool loadImageFile(const std::string& path, ImageRGBA& out, std::string& err) {
  std::string ext = lowerExt(path);
  std::vector<uint8_t> data;
  if (!readWholeFile(path, data, err)) return false;
  if (ext == ".webp") {
    int w = 0, h = 0;
    uint8_t* px = WebPDecodeRGBA(data.data(), data.size(), &w, &h);
    if (!px) { err = "Not a valid WebP file: " + path; return false; }
    out.w = uint32_t(w);
    out.h = uint32_t(h);
    out.rgba.assign(px, px + size_t(w) * h * 4);
    WebPFree(px);
    return true;
  }
  if (ext == ".svg") {
    data.push_back(0);
    NSVGimage* img = nsvgParse(reinterpret_cast<char*>(data.data()), "px", 96.0f);
    if (!img) { err = "Could not parse SVG: " + path; return false; }
    float w = img->width, h = img->height;
    float scale = 1.0f;
    if (w <= 0 || h <= 0) { w = h = 1024; }
    // rasterise small icons at a useful size, cap huge ones
    float longSide = std::max(w, h);
    if (longSide < 1024) scale = 1024 / longSide;
    if (longSide * scale > 16384) scale = 16384 / longSide;
    out.w = uint32_t(std::ceil(w * scale));
    out.h = uint32_t(std::ceil(h * scale));
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, img, 0, 0, scale, out.rgba.data(), int(out.w), int(out.h), int(out.w) * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return true;
  }
  int w = 0, h = 0, n = 0;
  stbi_uc* px = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &n, 4);  // GIF: first frame
  if (!px) {
    err = "Unsupported or damaged image (" + std::string(stbi_failure_reason() ? stbi_failure_reason() : "?") + "): " + path;
    return false;
  }
  out.w = uint32_t(w);
  out.h = uint32_t(h);
  out.rgba.assign(px, px + size_t(w) * h * 4);
  stbi_image_free(px);
  return true;
}

bool exportImage(const std::string& path, const ImageRGBA& img, std::string& err) {
  std::string ext = lowerExt(path);
  int ok = 0;
  if (ext == ".jpg" || ext == ".jpeg") {
    std::vector<uint8_t> rgb(size_t(img.w) * img.h * 3);
    for (size_t i = 0, n = size_t(img.w) * img.h; i < n; ++i) {
      unsigned a = img.rgba[i * 4 + 3];
      for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = uint8_t((img.rgba[i * 4 + k] * a + 255 * (255 - a) + 127) / 255);
    }
    ok = stbi_write_jpg((path + ".saving").c_str(), int(img.w), int(img.h), 3, rgb.data(), 92);
  } else {
    ok = stbi_write_png((path + ".saving").c_str(), int(img.w), int(img.h), 4, img.rgba.data(), int(img.w) * 4);
  }
  if (!ok) {
    err = "Could not write " + path + " (disk full or no permission?)";
    std::error_code ec;
    fs::remove(fs::path(reinterpret_cast<const char8_t*>((path + ".saving").c_str())), ec);
    return false;
  }
  return commitFileReplace(path + ".saving", path, err);
}

// ---------------------------------------------------------------------------
// PSD / PSB

struct BlendKey { BlendMode mode; char key[5]; };
static const BlendKey kKeys[] = {
    {BlendMode::Normal, "norm"},     {BlendMode::Darken, "dark"},      {BlendMode::Multiply, "mul "},
    {BlendMode::ColorBurn, "idiv"},  {BlendMode::LinearBurn, "lbrn"},  {BlendMode::Subtract, "fsub"},
    {BlendMode::Lighten, "lite"},    {BlendMode::Screen, "scrn"},      {BlendMode::ColorDodge, "div "},
    {BlendMode::GlowDodge, "div "},  {BlendMode::Add, "lddg"},         {BlendMode::AddGlow, "lddg"},
    {BlendMode::Overlay, "over"},    {BlendMode::SoftLight, "sLit"},   {BlendMode::HardLight, "hLit"},
    {BlendMode::Difference, "diff"}, {BlendMode::VividLight, "vLit"},  {BlendMode::LinearLight, "lLit"},
    {BlendMode::PinLight, "pLit"},   {BlendMode::HardMix, "hMix"},     {BlendMode::Exclusion, "smud"},
    {BlendMode::DarkerColor, "dkCl"}, {BlendMode::LighterColor, "lgCl"}, {BlendMode::Divide, "fdiv"},
    {BlendMode::Hue, "hue "},        {BlendMode::Saturation, "sat "},  {BlendMode::Color, "colr"},
    {BlendMode::Brightness, "lum "}};

static const char* keyFor(BlendMode m) {
  for (auto& k : kKeys) if (k.mode == m) return k.key;
  return "norm";
}
static bool modeFor(const char* key, BlendMode& m) {
  for (auto& k : kKeys)
    if (!memcmp(k.key, key, 4)) { m = k.mode; return true; }
  return false;
}

// Big-endian buffered reader over a FILE*.
class Reader {
 public:
  explicit Reader(FILE* f) : f_(f) {}
  bool ok() const { return ok_; }
  void bytes(void* dst, size_t n) { if (ok_ && n && fread(dst, 1, n, f_) != n) ok_ = false; }
  uint8_t u8() { uint8_t v = 0; bytes(&v, 1); return v; }
  uint16_t u16() { uint8_t b[2] = {}; bytes(b, 2); return uint16_t(b[0] << 8 | b[1]); }
  uint32_t u32() { uint8_t b[4] = {}; bytes(b, 4); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
  uint64_t u64() { uint64_t hi = u32(); return hi << 32 | u32(); }
  int16_t i16() { return int16_t(u16()); }
  int32_t i32() { return int32_t(u32()); }
  uint64_t len(bool psb) { return psb ? u64() : u32(); }
  void skip(int64_t n) { if (ok_ && n > 0) ok_ = seek64(f_, tell64(f_) + n); }
  int64_t pos() { return tell64(f_); }
  void seek(int64_t p) { if (ok_) ok_ = seek64(f_, p); }
 private:
  FILE* f_;
  bool ok_ = true;
};

static void unpackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
  size_t i = 0, o = 0;
  while (i < srcLen && o < dstLen) {
    int8_t n = int8_t(src[i++]);
    if (n >= 0) {
      size_t c = std::min<size_t>(size_t(n) + 1, std::min(dstLen - o, srcLen - i));
      memcpy(dst + o, src + i, c);
      i += size_t(n) + 1;
      o += c;
    } else if (n != -128) {
      size_t c = std::min<size_t>(size_t(1 - n), dstLen - o);
      if (i < srcLen) memset(dst + o, src[i], c);
      ++i;
      o += c;
    }
  }
}

// Decodes one channel (w x h, 8-bit) given its compression and payload.
static bool decodeChannel(uint16_t comp, const std::vector<uint8_t>& in, uint32_t w, uint32_t h, bool psb,
                          std::vector<uint8_t>& out) {
  out.assign(size_t(w) * h, 0);
  if (!w || !h) return true;
  if (comp == 0) {
    memcpy(out.data(), in.data(), std::min(in.size(), out.size()));
    return true;
  }
  if (comp == 1) {
    size_t cw = psb ? 4 : 2;
    size_t off = size_t(h) * cw;
    if (in.size() < off) return false;
    for (uint32_t y = 0; y < h; ++y) {
      size_t n = 0;
      for (size_t k = 0; k < cw; ++k) n = n << 8 | in[y * cw + k];
      if (off + n > in.size()) return false;
      unpackBits(in.data() + off, n, out.data() + size_t(y) * w, w);
      off += n;
    }
    return true;
  }
  if (comp == 2 || comp == 3) {
    int outLen = 0;
    char* z = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(in.data()), int(in.size()),
                                                           int(std::min<size_t>(out.size(), 1u << 30)), &outLen, 1);
    if (!z) return false;
    memcpy(out.data(), z, std::min(size_t(outLen), out.size()));
    free(z);
    if (comp == 3)
      for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = out.data() + size_t(y) * w;
        for (uint32_t x = 1; x < w; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
      }
    return true;
  }
  return false;
}

bool loadPsd(const std::string& path, DocFile& doc, std::string& err) {
  FILE* f = openUtf8(path, "rb");
  if (!f) { err = "Cannot open " + path; return false; }
  Reader r(f);
  struct Closer { FILE* f; ~Closer() { fclose(f); } } closer{f};
  char sig[4];
  r.bytes(sig, 4);
  if (!r.ok() || memcmp(sig, "8BPS", 4)) { err = "Not a Photoshop file: " + path; return false; }
  uint16_t version = r.u16();
  bool psb = version == 2;
  if (version != 1 && version != 2) { err = "Unknown PSD version"; return false; }
  r.skip(6);
  uint16_t channels = r.u16();
  doc.h = r.u32();
  doc.w = r.u32();
  uint16_t depth = r.u16();
  uint16_t colorMode = r.u16();
  if (depth != 8) { err = "Only 8-bit PSD/PSB files are supported (this one is " + std::to_string(depth) + "-bit)."; return false; }
  if (colorMode != 3 && colorMode != 1) { err = "Only RGB and grayscale PSD/PSB files are supported."; return false; }
  bool gray = colorMode == 1;
  r.skip(r.u32());  // colour mode data
  r.skip(r.u32());  // image resources
  uint64_t lmLen = r.len(psb);
  int64_t lmEnd = r.pos() + int64_t(lmLen);

  struct Rec {
    DocLayer layer;
    std::vector<std::pair<int16_t, uint64_t>> ch;
    int section = 0;  // lsct: 1/2 group folder, 3 group end marker
    bool clipped = false;
  };
  std::vector<Rec> recs;
  if (lmLen > 0) {
    uint64_t liLen = r.len(psb);
    int64_t liEnd = r.pos() + int64_t(liLen);
    if (liLen > 0) {
      int16_t count = r.i16();
      int n = std::abs(count);
      recs.resize(size_t(n));
      for (auto& rec : recs) {
        int32_t top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
        rec.layer.x = left;
        rec.layer.y = top;
        rec.layer.w = uint32_t(std::max(0, right - left));
        rec.layer.h = uint32_t(std::max(0, bottom - top));
        uint16_t nch = r.u16();
        for (uint16_t c = 0; c < nch; ++c) {
          int16_t id = r.i16();
          uint64_t l = r.len(psb);
          rec.ch.push_back({id, l});
        }
        r.skip(4);  // 8BIM
        char key[4];
        r.bytes(key, 4);
        if (!modeFor(key, rec.layer.mode)) {
          rec.layer.mode = BlendMode::Normal;
          if (memcmp(key, "pass", 4)) doc.warnings.push_back(std::string("Unsupported blend mode '") + std::string(key, 4) + "' shown as Normal");
        }
        rec.layer.opacity = r.u8() / 255.0f;
        if (r.u8() != 0) rec.clipped = true;  // clipping mask
        uint8_t flags = r.u8();
        rec.layer.visible = !(flags & 2);
        r.u8();
        uint32_t extra = r.u32();
        int64_t extraEnd = r.pos() + extra;
        r.skip(r.u32());  // layer mask data
        r.skip(r.u32());  // blending ranges
        uint8_t nameLen = r.u8();
        std::string name(nameLen, '\0');
        r.bytes(name.data(), nameLen);
        r.skip((4 - (1 + nameLen) % 4) % 4);
        rec.layer.name = name;
        while (r.ok() && r.pos() + 12 <= extraEnd) {
          char s2[4], k2[4];
          r.bytes(s2, 4);
          r.bytes(k2, 4);
          if (memcmp(s2, "8BIM", 4) && memcmp(s2, "8B64", 4)) break;
          bool longLen = psb && (!memcmp(k2, "LMsk", 4) || !memcmp(k2, "Lr16", 4) || !memcmp(k2, "Lr32", 4) ||
                                 !memcmp(k2, "Layr", 4) || !memcmp(k2, "Mt16", 4) || !memcmp(k2, "Mt32", 4) ||
                                 !memcmp(k2, "Mtrn", 4) || !memcmp(k2, "Alph", 4) || !memcmp(k2, "FMsk", 4) ||
                                 !memcmp(k2, "lnk2", 4) || !memcmp(k2, "FEid", 4) || !memcmp(k2, "FXid", 4) ||
                                 !memcmp(k2, "PxSD", 4));
          uint64_t l = longLen ? r.u64() : r.u32();
          int64_t start = r.pos();
          if (!memcmp(k2, "luni", 4) && l >= 4) {
            uint32_t cnt = r.u32();
            std::string u8;
            for (uint32_t i = 0; i < cnt && r.ok(); ++i) {
              uint32_t c = r.u16();
              if (c >= 0xD800 && c < 0xDC00 && i + 1 < cnt) { uint32_t lo = r.u16(); ++i; c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); }
              if (c == 0) continue;
              if (c < 0x80) u8 += char(c);
              else if (c < 0x800) { u8 += char(0xC0 | c >> 6); u8 += char(0x80 | (c & 63)); }
              else if (c < 0x10000) { u8 += char(0xE0 | c >> 12); u8 += char(0x80 | (c >> 6 & 63)); u8 += char(0x80 | (c & 63)); }
              else { u8 += char(0xF0 | c >> 18); u8 += char(0x80 | (c >> 12 & 63)); u8 += char(0x80 | (c >> 6 & 63)); u8 += char(0x80 | (c & 63)); }
            }
            rec.layer.name = u8;
          } else if ((!memcmp(k2, "lsct", 4) || !memcmp(k2, "lsdk", 4)) && l >= 4) {
            rec.section = int(r.u32());
          }
          r.seek(start + int64_t(l));
          if (l % 2) r.skip(1);
        }
        r.seek(extraEnd);
      }
      // channel image data, layer by layer
      std::vector<uint8_t> payload, plane;
      for (auto& rec : recs) {
        DocLayer& L = rec.layer;
        bool keep = rec.section == 0 && L.w && L.h;
        if (keep) L.rgba.assign(size_t(L.w) * L.h * 4, 255);
        bool hasAlpha = false;
        for (auto& [id, l] : rec.ch) {
          if (l < 2) { r.skip(int64_t(l)); continue; }
          uint16_t comp = r.u16();
          bool useful = keep && (id == -1 || (id >= 0 && id <= (gray ? 0 : 2)));
          if (!useful) { r.skip(int64_t(l) - 2); continue; }
          payload.resize(size_t(l - 2));
          r.bytes(payload.data(), payload.size());
          if (!decodeChannel(comp, payload, L.w, L.h, psb, plane)) {
            doc.warnings.push_back("Layer '" + L.name + "': unsupported channel compression " + std::to_string(comp));
            continue;
          }
          if (id == -1) hasAlpha = true;
          for (size_t i = 0, n = size_t(L.w) * L.h; i < n; ++i) {
            if (id == -1) L.rgba[i * 4 + 3] = plane[i];
            else if (gray) L.rgba[i * 4] = L.rgba[i * 4 + 1] = L.rgba[i * 4 + 2] = plane[i];
            else L.rgba[i * 4 + id] = plane[i];
          }
        }
        (void)hasAlpha;
      }
      r.seek(liEnd);
    }
  }
  if (!r.ok()) { err = "The file is truncated or damaged."; return false; }

  // Groups: apply hidden folders to their children, then flatten the tree.
  bool anyGroup = false;
  std::vector<bool> stack{true};
  for (int i = int(recs.size()) - 1; i >= 0; --i) {  // top to bottom
    Rec& rec = recs[size_t(i)];
    if (rec.section == 1 || rec.section == 2) { anyGroup = true; stack.push_back(stack.back() && rec.layer.visible); continue; }
    if (rec.section == 3) { if (stack.size() > 1) stack.pop_back(); continue; }
    rec.layer.visible = rec.layer.visible && stack.back();
  }
  bool anyMask = false, anyClip = false;
  for (auto& rec : recs) {
    for (auto& c : rec.ch) anyMask |= c.first <= -2;
    anyClip |= rec.clipped && rec.section == 0;
  }
  if (anyMask) doc.warnings.push_back("Layer masks are not supported yet and were ignored (the unmasked pixels are shown).");
  if (anyClip) doc.warnings.push_back("Clipping masks are not supported yet; clipped layers are shown unclipped.");
  if (anyGroup) doc.warnings.push_back("Layer folders were flattened (1.0.0 has no groups); hidden folders keep their layers hidden.");
  for (auto& rec : recs)
    if (rec.section == 0) doc.layers.push_back(std::move(rec.layer));

  if (doc.layers.empty()) {
    // flat file: use the merged image
    r.seek(lmEnd);
    uint16_t comp = r.u16();
    DocLayer L;
    L.name = "Background";
    L.w = doc.w;
    L.h = doc.h;
    L.rgba.assign(size_t(L.w) * L.h * 4, 255);
    size_t plane = size_t(L.w) * L.h;
    int nc = std::min<int>(channels, gray ? 2 : 4);
    std::vector<uint8_t> buf(plane);
    if (comp == 1) {
      size_t cw = psb ? 4 : 2;
      std::vector<uint64_t> counts(size_t(channels) * L.h);
      for (auto& c : counts) c = cw == 4 ? r.u32() : r.u16();
      std::vector<uint8_t> row;
      for (int c = 0; c < channels; ++c) {
        for (uint32_t y = 0; y < L.h; ++y) {
          row.resize(size_t(counts[size_t(c) * L.h + y]));
          r.bytes(row.data(), row.size());
          unpackBits(row.data(), row.size(), buf.data() + size_t(y) * L.w, L.w);
        }
        if (c >= nc) continue;
        for (size_t i = 0; i < plane; ++i) {
          if (gray) { if (c == 0) L.rgba[i * 4] = L.rgba[i * 4 + 1] = L.rgba[i * 4 + 2] = buf[i]; else L.rgba[i * 4 + 3] = buf[i]; }
          else L.rgba[i * 4 + c] = buf[i];
        }
      }
    } else if (comp == 0) {
      for (int c = 0; c < nc; ++c) {
        r.bytes(buf.data(), plane);
        for (size_t i = 0; i < plane; ++i) {
          if (gray) { if (c == 0) L.rgba[i * 4] = L.rgba[i * 4 + 1] = L.rgba[i * 4 + 2] = buf[i]; else L.rgba[i * 4 + 3] = buf[i]; }
          else L.rgba[i * 4 + c] = buf[i];
        }
      }
    } else {
      err = "Unsupported merged-image compression in a flat PSD.";
      return false;
    }
    if (!r.ok()) { err = "The file is truncated or damaged."; return false; }
    doc.layers.push_back(std::move(L));
  }
  return true;
}

// ---------------------------------------------------------------------------
// PSD / PSB writer

class Writer {
 public:
  explicit Writer(FILE* f) : f_(f) {}
  bool ok() const { return ok_; }
  void bytes(const void* p, size_t n) { if (ok_ && n && fwrite(p, 1, n, f_) != n) ok_ = false; }
  void u8(uint8_t v) { bytes(&v, 1); }
  void u16(uint16_t v) { uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)}; bytes(b, 2); }
  void u32(uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; bytes(b, 4); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
  void len(bool psb, uint64_t v) { if (psb) u64(v); else u32(uint32_t(v)); }
  int64_t pos() { return tell64(f_); }
  void patchLen(int64_t at, bool psb, uint64_t v) {
    int64_t here = pos();
    if (!seek64(f_, at)) { ok_ = false; return; }
    len(psb, v);
    if (!seek64(f_, here)) ok_ = false;
  }
  void patch32(int64_t at, uint32_t v) {
    int64_t here = pos();
    if (!seek64(f_, at)) { ok_ = false; return; }
    u32(v);
    if (!seek64(f_, here)) ok_ = false;
  }
 private:
  FILE* f_;
  bool ok_ = true;
};

static void packBitsRow(const uint8_t* s, size_t n, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < n) {
    size_t run = 1;
    while (i + run < n && run < 128 && s[i + run] == s[i]) ++run;
    if (run >= 2) {
      out.push_back(uint8_t(int8_t(1 - int(run))));
      out.push_back(s[i]);
      i += run;
      continue;
    }
    size_t lit = 1;
    while (i + lit < n && lit < 128) {
      if (i + lit + 1 < n && s[i + lit] == s[i + lit + 1]) break;
      ++lit;
    }
    out.push_back(uint8_t(lit - 1));
    out.insert(out.end(), s + i, s + i + lit);
    i += lit;
  }
}

// RLE-encodes one channel of a w x h region of an RGBA image (stride = full image width).
// Output: row byte counts (2 or 4 bytes each) followed by the packed rows. Multi-threaded.
static void encodeChannelRle(const uint8_t* rgba, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                             int comp, bool psb, std::vector<uint8_t>& out) {
  unsigned threads = std::max(1u, std::min(32u, std::thread::hardware_concurrency()));
  uint32_t per = std::max<uint32_t>(64, (h + threads - 1) / threads);
  std::vector<std::vector<uint8_t>> parts((h + per - 1) / per);
  std::vector<std::vector<uint32_t>> counts(parts.size());
  auto work = [&](size_t p) {
    std::vector<uint8_t> row(w);
    uint32_t ya = uint32_t(p) * per, yb = std::min(h, ya + per);
    for (uint32_t y = ya; y < yb; ++y) {
      const uint8_t* src = rgba + (size_t(y0 + y) * stride + x0) * 4 + comp;
      for (uint32_t x = 0; x < w; ++x) row[x] = src[size_t(x) * 4];
      size_t before = parts[p].size();
      packBitsRow(row.data(), w, parts[p]);
      counts[p].push_back(uint32_t(parts[p].size() - before));
    }
  };
  std::vector<std::thread> pool;
  for (size_t p = 1; p < parts.size(); ++p) pool.emplace_back(work, p);
  if (!parts.empty()) work(0);
  for (auto& t : pool) t.join();
  out.clear();
  size_t cw = psb ? 4 : 2;
  size_t total = size_t(h) * cw;
  for (auto& p : parts) total += p.size();
  out.reserve(total);
  for (auto& c : counts)
    for (uint32_t n : c) {
      if (psb) { out.push_back(uint8_t(n >> 24)); out.push_back(uint8_t(n >> 16)); }
      out.push_back(uint8_t(n >> 8));
      out.push_back(uint8_t(n));
    }
  for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
}

bool writePsdFile(const std::string& tmp, const DocFile& doc, const ImageRGBA& merged, std::string& err, float* progress) {
  const std::string& path = tmp;
  // Tight bounds of each layer (skip fully transparent borders).
  struct Box { uint32_t x0, y0, x1, y1; };
  std::vector<Box> boxes;
  double rawBytes = double(doc.w) * doc.h * 4;
  for (auto& L : doc.layers) {
    Box b{L.w, L.h, 0, 0};
    for (uint32_t y = 0; y < L.h; ++y) {
      const uint8_t* row = L.rgba.data() + size_t(y) * L.w * 4;
      for (uint32_t x = 0; x < L.w; ++x)
        if (row[x * 4 + 3]) {
          b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x + 1);
          b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y + 1);
        }
    }
    if (b.x0 >= b.x1) b = {0, 0, 0, 0};
    boxes.push_back(b);
    rawBytes += double(b.x1 - b.x0) * (b.y1 - b.y0) * 4.1;
  }
  bool psb = doc.w > 30000 || doc.h > 30000 || rawBytes > 3.9e9;
  FILE* f = openUtf8(tmp, "wb");
  std::string shown = tmp.size() > 7 && tmp.compare(tmp.size() - 7, 7, ".saving") == 0 ? tmp.substr(0, tmp.size() - 7) : tmp;
  if (!f) { err = "Cannot write " + shown + " (folder missing, no permission, or disk full)"; return false; }
  Writer w(f);
  w.bytes("8BPS", 4);
  w.u16(psb ? 2 : 1);
  const uint8_t zero6[6] = {};
  w.bytes(zero6, 6);
  bool mergedAlpha = false;
  for (size_t i = 3; i < merged.rgba.size(); i += 4)
    if (merged.rgba[i] != 255) { mergedAlpha = true; break; }
  w.u16(mergedAlpha ? 4 : 3);
  w.u32(doc.h);
  w.u32(doc.w);
  w.u16(8);
  w.u16(3);  // RGB
  w.u32(0);  // colour mode data
  w.u32(0);  // image resources

  int64_t lmAt = w.pos();
  w.len(psb, 0);
  int64_t liAt = w.pos();
  w.len(psb, 0);
  int64_t liStart = w.pos();
  w.u16(uint16_t(int16_t(-int(doc.layers.size()))));  // negative: first alpha channel = merged transparency
  std::vector<std::array<int64_t, 4>> chLenAt(doc.layers.size());
  for (size_t li = 0; li < doc.layers.size(); ++li) {
    const DocLayer& L = doc.layers[li];
    const Box& b = boxes[li];
    if (b.x1 > b.x0) {  // layer pixels may start anywhere in the document (L.x, L.y)
      w.u32(uint32_t(L.y + int32_t(b.y0))); w.u32(uint32_t(L.x + int32_t(b.x0)));
      w.u32(uint32_t(L.y + int32_t(b.y1))); w.u32(uint32_t(L.x + int32_t(b.x1)));
    } else {
      w.u32(0); w.u32(0); w.u32(0); w.u32(0);
    }
    w.u16(4);
    const int16_t ids[4] = {-1, 0, 1, 2};
    for (int c = 0; c < 4; ++c) {
      w.u16(uint16_t(ids[c]));
      chLenAt[li][size_t(c)] = w.pos();
      w.len(psb, 2);
    }
    w.bytes("8BIM", 4);
    w.bytes(keyFor(L.mode), 4);
    w.u8(uint8_t(std::lround(std::clamp(L.opacity, 0.0f, 1.0f) * 255)));
    w.u8(0);
    w.u8(L.visible ? 0 : 2);
    w.u8(0);
    // extra data: mask (0), blending ranges (0), pascal name, luni
    std::string ascii;
    for (char c : L.name) ascii += (unsigned char)c < 128 ? c : '_';
    if (ascii.size() > 255) ascii.resize(255);
    std::vector<uint8_t> extra;
    auto e32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) extra.push_back(uint8_t(v >> s)); };
    e32(0);
    e32(0);
    extra.push_back(uint8_t(ascii.size()));
    extra.insert(extra.end(), ascii.begin(), ascii.end());
    while ((extra.size() - 8) % 4) extra.push_back(0);
    // UTF-8 -> UTF-16 for 'luni'
    std::vector<uint16_t> u16;
    for (size_t i = 0; i < L.name.size();) {
      uint32_t c = (unsigned char)L.name[i];
      int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      if (n > 1) { c &= (0xFF >> (n + 1)); for (int k = 1; k < n && i + k < L.name.size(); ++k) c = c << 6 | ((unsigned char)L.name[i + k] & 63); }
      i += size_t(n);
      if (c >= 0x10000) { c -= 0x10000; u16.push_back(uint16_t(0xD800 + (c >> 10))); u16.push_back(uint16_t(0xDC00 + (c & 1023))); }
      else u16.push_back(uint16_t(c));
    }
    extra.insert(extra.end(), {'8', 'B', 'I', 'M', 'l', 'u', 'n', 'i'});
    uint32_t ul = uint32_t(4 + u16.size() * 2);
    uint32_t ulPadded = (ul + 3) & ~3u;
    e32(ulPadded);
    e32(uint32_t(u16.size()));
    for (uint16_t c : u16) { extra.push_back(uint8_t(c >> 8)); extra.push_back(uint8_t(c)); }
    for (uint32_t k = ul; k < ulPadded; ++k) extra.push_back(0);
    w.u32(uint32_t(extra.size()));
    w.bytes(extra.data(), extra.size());
  }
  // channel image data
  std::vector<uint8_t> enc;
  for (size_t li = 0; li < doc.layers.size(); ++li) {
    const DocLayer& L = doc.layers[li];
    const Box& b = boxes[li];
    const int comps[4] = {3, 0, 1, 2};
    for (int c = 0; c < 4; ++c) {
      if (b.x1 == b.x0) {
        w.u16(0);
        continue;
      }
      encodeChannelRle(L.rgba.data(), L.w, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, comps[c], psb, enc);
      w.u16(1);
      w.bytes(enc.data(), enc.size());
      w.patchLen(chLenAt[li][size_t(c)], psb, enc.size() + 2);
    }
    if (progress) *progress = float(li + 1) / float(doc.layers.size() + 1);
  }
  int64_t liEnd = w.pos();
  uint64_t liLen = uint64_t(liEnd - liStart);
  if (liLen % 4) { for (uint64_t k = liLen % 4; k < 4; ++k) w.u8(0); liLen = uint64_t(w.pos() - liStart); }
  w.patchLen(liAt, psb, liLen);
  w.u32(0);  // global layer mask info
  w.patchLen(lmAt, psb, uint64_t(w.pos() - liAt));

  // merged image (RLE, planar: all row counts first, then rows)
  w.u16(1);
  int nc = mergedAlpha ? 4 : 3;
  std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(nc));
  const int mcomps[4] = {0, 1, 2, 3};
  for (int c = 0; c < nc; ++c) encodeChannelRle(merged.rgba.data(), merged.w, 0, 0, merged.w, merged.h, mcomps[c], psb, planes[size_t(c)]);
  size_t cw = psb ? 4 : 2;
  for (auto& p : planes) w.bytes(p.data(), size_t(merged.h) * cw);
  for (auto& p : planes) w.bytes(p.data() + size_t(merged.h) * cw, p.size() - size_t(merged.h) * cw);
  bool ok = w.ok();
  if (!syncAndClose(f)) ok = false;
  if (!ok) {
    std::error_code ec;
    fs::remove(fs::path(reinterpret_cast<const char8_t*>(tmp.c_str())), ec);
    err = "Write error while saving " + shown + " (disk full?)";
    return false;
  }
  if (progress) *progress = 1.0f;
  return true;
}

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

// What the file must contain: per layer the written rectangle, its pixels' hash and the properties.
std::vector<PsdCheck> psdChecks(const DocFile& doc) {
  std::vector<PsdCheck> out;
  for (const DocLayer& L : doc.layers) {
    PsdCheck c;
    uint32_t x0 = L.w, y0 = L.h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < L.h; ++y)
      for (uint32_t x = 0; x < L.w; ++x)
        if (L.rgba[(size_t(y) * L.w + x) * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
    if (x0 < x1) {
      c.x = L.x + int32_t(x0); c.y = L.y + int32_t(y0); c.w = x1 - x0; c.h = y1 - y0;
      uint64_t h = 1469598103934665603ull;
      for (uint32_t y = y0; y < y1; ++y) h = fnv(h, &L.rgba[(size_t(y) * L.w + x0) * 4], size_t(c.w) * 4);
      c.hash = h;
    }
    c.visible = L.visible;
    c.opacity = uint8_t(std::lround(std::clamp(L.opacity, 0.0f, 1.0f) * 255));
    c.mode = L.mode;
    out.push_back(c);
  }
  return out;
}

// Reads the written file back with the normal loader and compares everything that was saved.
bool verifyPsd(const std::string& file, uint32_t w, uint32_t h, const std::vector<PsdCheck>& expect, std::string& err) {
  DocFile d;
  std::string e;
  if (!loadPsd(file, d, e)) { err = "the written file could not be read back: " + e; return false; }
  if (d.w != w || d.h != h) { err = "the written file has the wrong size"; return false; }
  if (d.layers.size() != expect.size()) {
    err = "the written file has " + std::to_string(d.layers.size()) + " layers instead of " + std::to_string(expect.size());
    return false;
  }
  for (size_t i = 0; i < expect.size(); ++i) {
    const PsdCheck& c = expect[i];
    const DocLayer& L = d.layers[i];
    std::string which = "layer " + std::to_string(i + 1);
    if (L.visible != c.visible || uint8_t(std::lround(L.opacity * 255)) != c.opacity || L.mode != c.mode) {
      err = which + ": properties differ after saving";
      return false;
    }
    if (c.w == 0) {
      bool any = false;
      for (size_t k = 3; k < L.rgba.size(); k += 4) any |= L.rgba[k] != 0;
      if (any) { err = which + ": should be empty"; return false; }
      continue;
    }
    // the loader may return a larger rect; compare exactly the expected one
    if (L.x > c.x || L.y > c.y || L.x + int64_t(L.w) < c.x + int64_t(c.w) || L.y + int64_t(L.h) < c.y + int64_t(c.h)) {
      err = which + ": pixels are missing after saving";
      return false;
    }
    uint64_t hsh = 1469598103934665603ull;
    for (uint32_t y = 0; y < c.h; ++y)
      hsh = fnv(hsh, &L.rgba[(size_t(c.y - L.y + int32_t(y)) * L.w + size_t(c.x - L.x)) * 4], size_t(c.w) * 4);
    if (hsh != c.hash) { err = which + ": pixels differ after saving"; return false; }
  }
  return true;
}

bool savePsd(const std::string& path, const DocFile& doc, const ImageRGBA& merged, std::string& err, float* progress) {
  std::string tmp = path + ".saving";
  if (!writePsdFile(tmp, doc, merged, err, progress)) return false;
  std::string verr;
  if (!verifyPsd(tmp, doc.w, doc.h, psdChecks(doc), verr)) {
    std::error_code ec;
    fs::remove(fs::path(reinterpret_cast<const char8_t*>(tmp.c_str())), ec);
    err = "Saving was stopped - " + verr + ". The previous file was not touched.";
    return false;
  }
  return commitFileReplace(tmp, path, err);
}

bool encodePngMemory(const ImageRGBA& img, std::vector<uint8_t>& out) {
  out.clear();
  auto write = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  };
  return stbi_write_png_to_func(write, &out, int(img.w), int(img.h), 4, img.rgba.data(), int(img.w) * 4) != 0;
}

bool decodeImageMemory(const void* data, size_t size, ImageRGBA& out, std::string& err) {
  int w = 0, h = 0, n = 0;
  stbi_uc* px = stbi_load_from_memory(static_cast<const stbi_uc*>(data), int(size), &w, &h, &n, 4);
  if (!px) { err = std::string("Unsupported image: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown"); return false; }
  out.w = uint32_t(w);
  out.h = uint32_t(h);
  out.rgba.assign(px, px + size_t(w) * h * 4);
  stbi_image_free(px);
  return true;
}
