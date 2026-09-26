// Text tool engine: installed-font discovery, InDesign-style character / paragraph formatting,
// layout (point text and area text with wrapping) and anti-aliased rasterisation (stb_truetype).
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FontFace {
  std::string family, style, path;  // path empty = the built-in font
  int index = 0;                    // face inside a .ttc collection
  int weight = 400;
  bool italic = false;
};

struct FontFamily {
  std::string name;
  std::vector<FontFace> faces;  // sorted by weight, upright before italic
};

struct LoadedFont;  // font file bytes + stb_truetype info + glyph cache

// All fonts installed on the system (plus the built-in one), found on a background thread.
// Only each file's name / OS/2 tables are read; results are cached by path + modification time.
class FontLibrary {
 public:
  ~FontLibrary();
  void startScan(const std::string& cacheFile);
  bool ready() const { return ready_; }
  const std::vector<FontFamily>& families() const { return families_; }  // valid once ready()
  const FontFamily* family(const std::string& name) const;
  const FontFace* face(const std::string& family, const std::string& style) const;
  std::shared_ptr<LoadedFont> load(const FontFace& f);  // cached; nullptr if the file is unusable
  static FontFace builtIn();

 private:
  void scan(std::string cacheFile);
  std::thread thread_;
  std::atomic<bool> ready_{false};
  std::vector<FontFamily> families_;
  std::map<std::string, std::weak_ptr<LoadedFont>> loaded_;
};

// Character + paragraph formatting (the basic options of InDesign's Character / Paragraph panels).
struct TextStyle {
  std::string family, style = "Regular";
  float size = 48;          // px (em size)
  float leading = 0;        // px between baselines; 0 = Auto (120 % of the size)
  int kerning = 1;          // 0 none, 1 metrics (the font's kerning pairs)
  float tracking = 0;       // 1/1000 em added after every character
  float hScale = 100, vScale = 100;  // %
  float baselineShift = 0;  // px, up
  float skew = 0;           // degrees (false italic)
  bool allCaps = false, underline = false, strike = false;
  int align = 0;            // 0 left, 1 centre, 2 right, 3 justify (last line left)
  float indentLeft = 0, indentRight = 0, indentFirst = 0;
  float spaceBefore = 0, spaceAfter = 0;
  bool operator==(const TextStyle&) const = default;
};

struct TextBox {
  double x = 0, y = 0;  // top-left (area text) / anchor at the top of the first line (point text)
  double width = 0;     // 0 = point text: lines only break at Enter
  std::u32string text;
};

struct TextLayout {
  int x0 = 0, y0 = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> cov;  // document-space coverage over (x0, y0, w, h)
  struct Caret { double x, top, bottom; int line; };
  std::vector<Caret> carets;  // one per text position 0..n
  double boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0;  // the frame drawn around the text
};

// Lays out and (if raster) rasterises the text. Returns false without a usable font.
bool layoutText(LoadedFont* font, const TextStyle& st, const TextBox& box, TextLayout& out, bool raster);

std::u32string utf8ToU32(const std::string& s);
std::string u32ToUtf8(const std::u32string& s);
