// Brush tip library (imported sampled tips and pattern textures): grayscale images kept as PNG in
// User-Data/settings/tips, uploaded to the GPU tip atlas the first time a brush uses them.
#include "app.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

static std::string tipDir(const std::string& userData) {
  if (userData.empty()) return {};
  auto u = (fs::u8path(userData) / "settings" / "tips").u8string();
  return std::string(reinterpret_cast<const char*>(u.c_str()), u.size());
}

void App::loadTipLibrary() {
  std::string dir = tipDir(userData);
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(fs::u8path(dir), ec)) return;
  for (auto& f : fs::directory_iterator(fs::u8path(dir), ec)) {
    if (f.path().extension() != ".png") continue;
    std::string name = reinterpret_cast<const char*>(f.path().stem().u8string().c_str());
    if (tipLib.count(name)) continue;
    int w = 0, h = 0, n = 0;
    std::string p = reinterpret_cast<const char*>(f.path().u8string().c_str());
    unsigned char* px = stbi_load(p.c_str(), &w, &h, &n, 1);
    if (!px) continue;
    TipImage t;
    t.w = uint32_t(w);
    t.h = uint32_t(h);
    t.a.assign(px, px + size_t(w) * h);
    stbi_image_free(px);
    tipLib[name] = std::move(t);
  }
}

// Adds a tip (alpha 0..255, 255 = paint); large tips are reduced to 1024 px (box filter).
std::string App::addTipImage(const std::string& base, uint32_t w, uint32_t h, std::vector<uint8_t> alpha) {
  const uint32_t kMax = 1024;
  if (w > kMax || h > kMax) {
    float s = float(kMax) / float(std::max(w, h));
    uint32_t nw = std::max(1u, uint32_t(w * s + 0.5f)), nh = std::max(1u, uint32_t(h * s + 0.5f));
    std::vector<uint8_t> out(size_t(nw) * nh);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x) {
        uint32_t x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw), y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
        uint32_t sum = 0, cnt = 0;
        for (uint32_t yy = y0; yy < y1 && yy < h; ++yy)
          for (uint32_t xx = x0; xx < x1 && xx < w; ++xx) { sum += alpha[size_t(yy) * w + xx]; ++cnt; }
        out[size_t(y) * nw + x] = uint8_t(cnt ? sum / cnt : 0);
      }
    alpha.swap(out);
    w = nw;
    h = nh;
  }
  std::string clean;
  for (char c : base) clean += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
  if (clean.empty()) clean = "tip";
  if (clean.size() > 60) clean.resize(60);
  std::string name = clean;
  for (int i = 2; tipLib.count(name); ++i) name = clean + "_" + std::to_string(i);
  TipImage t;
  t.w = w;
  t.h = h;
  t.a = std::move(alpha);
  std::string dir = tipDir(userData);
  if (!dir.empty()) {
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    auto p = (fs::u8path(dir) / fs::u8path(name + ".png")).u8string();
    stbi_write_png(reinterpret_cast<const char*>(p.c_str()), int(w), int(h), 1, t.a.data(), int(w));
  }
  tipLib[name] = std::move(t);
  return name;
}

int App::tipSlot(const std::string& name) {
  auto it = tipLib.find(name);
  if (it == tipLib.end()) return -1;
  TipImage& t = it->second;
  if (t.slot < 0 && !t.failed) {
    std::string err;
    t.slot = R.tipAdd(t.w, t.h, t.a.data(), err);
    if (t.slot < 0) { t.failed = true; showToast("Brush tip not available: " + err); }
  }
  return t.slot;
}

const App::TipImage* App::tipImage(const std::string& name) const {
  auto it = tipLib.find(name);
  return it == tipLib.end() ? nullptr : &it->second;
}

// Resolves the brush's tips / texture / dual tip to atlas slots for a stroke.
void App::applyBrushTips(const BrushSettings& b, StrokeStyle& st, BrushEngine& e) {
  e.tipSlots.clear();
  for (const std::string& n : b.tips) {
    int s = tipSlot(n);
    if (s >= 0) e.tipSlots.push_back(s);
  }
  st.texSlot = b.texture.empty() ? -1 : tipSlot(b.texture);
  if (st.texSlot >= 0) {
    st.texMode = b.texMode;
    st.texInvert = b.texInvert;
    st.patScale = 1.0f / std::max(0.01f, b.patScale);
    st.texBrightness = b.texBrightness;
    st.texContrast = b.texContrast;
    st.texStrength = b.texStrength;
  }
  st.dualSlot = b.dualTip.empty() ? -1 : tipSlot(b.dualTip);
  st.dualScale = b.dualScale;
}
