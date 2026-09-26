// Non-brush tools: selections, fill, gradient, line/shapes, eyedropper, transform, hand;
// plus the tool bar and per-tool settings.
#include "app.h"
#include "fileio.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "icons_lucide.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static constexpr double kPi = 3.14159265358979323846;

struct ToolInfo { ToolId id; const char* name; const char* key; };
static const ToolInfo kTools[] = {
    {ToolId::Brush, "Brush", "B"},          {ToolId::Eyedropper, "Eyedropper", "I  (Alt+click)"},
    {ToolId::Fill, "Fill", "G"},            {ToolId::Gradient, "Gradient", "Shift+G"},
    {ToolId::Line, "Line", "U"},            {ToolId::Rect, "Rectangle", "U"},
    {ToolId::Ellipse, "Ellipse", "U"},      {ToolId::SelRect, "Rectangle select", "M"},
    {ToolId::SelEllipse, "Ellipse select", "M"}, {ToolId::Lasso, "Lasso select", "L"},
    {ToolId::Wand, "Magic wand", "W"},      {ToolId::Transform, "Move / Transform", "V, Ctrl+T"},
    {ToolId::Hand, "Hand (pan)", "H, Space"}, {ToolId::Text, "Text", "T"}};

// ---------------------------------------------------------------------------
// Rasterisation (CPU): polygon coverage with 4 sub-scanlines and fractional span ends.

static void rasterPolygon(const std::vector<double>& pts, int docW, int docH, int& ox, int& oy, uint32_t& ow, uint32_t& oh,
                          std::vector<uint8_t>& cov) {
  size_t n = pts.size() / 2;
  ow = oh = 0;
  if (n < 3) return;
  double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
  for (size_t i = 0; i < n; ++i) {
    minx = std::min(minx, pts[2 * i]); maxx = std::max(maxx, pts[2 * i]);
    miny = std::min(miny, pts[2 * i + 1]); maxy = std::max(maxy, pts[2 * i + 1]);
  }
  int x0 = std::max(0, int(std::floor(minx))), y0 = std::max(0, int(std::floor(miny)));
  int x1 = std::min(docW, int(std::ceil(maxx)) + 1), y1 = std::min(docH, int(std::ceil(maxy)) + 1);
  if (x0 >= x1 || y0 >= y1) return;
  ox = x0; oy = y0; ow = uint32_t(x1 - x0); oh = uint32_t(y1 - y0);
  cov.assign(size_t(ow) * oh, 0);
  std::vector<float> acc(ow + 2);
  std::vector<double> xs;
  const int S = 4;
  for (int y = y0; y < y1; ++y) {
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (int sub = 0; sub < S; ++sub) {
      double sy = y + (sub + 0.5) / S;
      xs.clear();
      for (size_t i = 0; i < n; ++i) {
        double ax = pts[2 * i], ay = pts[2 * i + 1];
        double bx = pts[2 * ((i + 1) % n)], by = pts[2 * ((i + 1) % n) + 1];
        if ((ay <= sy) != (by <= sy)) xs.push_back(ax + (sy - ay) / (by - ay) * (bx - ax));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {  // even-odd
        double a = std::clamp(xs[k] - x0, 0.0, double(ow)), b = std::clamp(xs[k + 1] - x0, 0.0, double(ow));
        if (b <= a) continue;
        int ia = int(a), ib = int(b);
        if (ia == ib) { acc[ia] += float(b - a) / S; continue; }
        acc[ia] += float(ia + 1 - a) / S;
        for (int i = ia + 1; i < ib; ++i) acc[i] += 1.0f / S;
        if (ib < int(ow)) acc[ib] += float(b - ib) / S;
      }
    }
    uint8_t* row = cov.data() + size_t(y - y0) * ow;
    for (uint32_t i = 0; i < ow; ++i) row[i] = uint8_t(std::lround(std::clamp(acc[i], 0.0f, 1.0f) * 255));
  }
}

static std::vector<double> ellipsePoly(double x0, double y0, double x1, double y1) {
  double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = std::abs(x1 - x0) / 2, ry = std::abs(y1 - y0) / 2;
  int n = std::clamp(int((rx + ry) * 0.8), 32, 2048);
  std::vector<double> p;
  for (int i = 0; i < n; ++i) {
    double a = 2 * kPi * i / n;
    p.push_back(cx + rx * std::cos(a));
    p.push_back(cy + ry * std::sin(a));
  }
  return p;
}

static std::vector<double> rectPoly(double x0, double y0, double x1, double y1) {
  return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::min(y0, y1),
          std::max(x0, x1), std::max(y0, y1), std::min(x0, x1), std::max(y0, y1)};
}

// 4-point homography: maps src[i] -> dst[i] (row-major 3x3, h[8] = 1).
static bool homography(const double src[4][2], const double dst[4][2], double h[9]) {
  double A[8][9] = {};
  for (int i = 0; i < 4; ++i) {
    double x = src[i][0], y = src[i][1], u = dst[i][0], v = dst[i][1];
    double r0[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
    double r1[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
    memcpy(A[2 * i], r0, sizeof r0);
    memcpy(A[2 * i + 1], r1, sizeof r1);
  }
  for (int c = 0; c < 8; ++c) {
    int piv = c;
    for (int r = c + 1; r < 8; ++r) if (std::abs(A[r][c]) > std::abs(A[piv][c])) piv = r;
    if (std::abs(A[piv][c]) < 1e-12) return false;
    for (int k = 0; k < 9; ++k) std::swap(A[c][k], A[piv][k]);
    for (int r = 0; r < 8; ++r) {
      if (r == c) continue;
      double f = A[r][c] / A[c][c];
      for (int k = c; k < 9; ++k) A[r][k] -= f * A[c][k];
    }
  }
  for (int i = 0; i < 8; ++i) h[i] = A[i][8] / A[i][i];
  h[8] = 1;
  return true;
}

static bool invert3(const double m[9], double o[9]) {
  double d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (std::abs(d) < 1e-15) return false;
  o[0] = (m[4] * m[8] - m[5] * m[7]) / d; o[1] = (m[2] * m[7] - m[1] * m[8]) / d; o[2] = (m[1] * m[5] - m[2] * m[4]) / d;
  o[3] = (m[5] * m[6] - m[3] * m[8]) / d; o[4] = (m[0] * m[8] - m[2] * m[6]) / d; o[5] = (m[2] * m[3] - m[0] * m[5]) / d;
  o[6] = (m[3] * m[7] - m[4] * m[6]) / d; o[7] = (m[1] * m[6] - m[0] * m[7]) / d; o[8] = (m[0] * m[4] - m[1] * m[3]) / d;
  return true;
}

// ---------------------------------------------------------------------------
// Helpers

void App::docToScreen(double dx, double dy, float& sx, float& sy) const {
  double vx = dx - view.panX, vy = dy - view.panY;
  double c = std::cos(view.rotation), s = std::sin(view.rotation);
  sx = float(R.extent.width * 0.5 + view.zoom * (c * vx - s * vy) * (view.flipX ? -1 : 1));
  sy = float(R.extent.height * 0.5 + view.zoom * (s * vx + c * vy));
}

StrokeStyle App::currentStyle(bool eraser) {
  if (!eraser) noteColorUsed();
  const BrushSettings& b = brushes[tipIndex];
  StrokeStyle st;
  st.hardness = b.hardness;
  st.buildUp = b.buildUp;
  st.texStrength = b.texStrength;
  st.texScale = b.texScale;
  st.color[0] = color[0]; st.color[1] = color[1]; st.color[2] = color[2];
  st.opacity = b.opacity;
  st.eraser = eraser;
  BrushEngine tipsOnly;  // pattern texture / dual tip of imported brushes
  applyBrushTips(b, st, tipsOnly);
  return st;
}

void App::setTool(ToolId t) {
  if (t != ToolId::Text && textEdit.active) commitText();
  if (t != tool) {
    cancelPreviews();
    hoverResult = FloodResult{};
    hoverX = hoverY = -1;
  }
  if (xf.active && t != ToolId::Transform) applyTransform();
  tool = t;
  if (t == ToolId::Transform && !xf.active) startTransform();
}

// ---------------------------------------------------------------------------
// Selection

void App::resetSelection() {
  selCpu.clear();
  selActive = false;
  selX0 = selY0 = selX1 = selY1 = 0;
  R.setSelectionActive(false);
}

// Stores the current selection inside [x0,x1) x [y0,y1) (plus its bbox state) as an undo step.
void App::snapshotSelection(int x0, int y0, int x1, int y1) {
  int W = int(R.docW), H = int(R.docH);
  if (selCpu.size() != size_t(W) * H) selCpu.assign(size_t(W) * H, 0);
  if (selActive) { x0 = std::min(x0, selX0); y0 = std::min(y0, selY0); x1 = std::max(x1, selX1); y1 = std::max(y1, selY1); }
  x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W); y1 = std::min(y1, H);
  if (x0 >= x1 || y0 >= y1) return;
  std::vector<uint8_t> data(size_t(x1 - x0) * (y1 - y0));
  for (int yy = y0; yy < y1; ++yy) memcpy(&data[size_t(yy - y0) * (x1 - x0)], &selCpu[size_t(yy) * W + x0], size_t(x1 - x0));
  int st[5] = {selX0, selY0, selX1, selY1, selActive ? 1 : 0};
  R.pushSelectionUndo(std::move(data), x0, y0, x1 - x0, y1 - y0, st);
}

void App::combineSelection(int x, int y, uint32_t w, uint32_t h, const std::vector<uint8_t>& cov, int op) {
  if (!R.hasDocument()) return;
  int W = int(R.docW), H = int(R.docH);
  snapshotSelection(x, y, x + int(w), y + int(h));
  if (selCpu.size() != size_t(W) * H) selCpu.assign(size_t(W) * H, 0);
  int ux0 = x, uy0 = y, ux1 = x + int(w), uy1 = y + int(h);
  if (op == 0 && selActive) {  // replace: clear the old area
    for (int yy = selY0; yy < selY1; ++yy) memset(selCpu.data() + size_t(yy) * W + selX0, 0, size_t(selX1 - selX0));
  }
  if (selActive) { ux0 = std::min(ux0, selX0); uy0 = std::min(uy0, selY0); ux1 = std::max(ux1, selX1); uy1 = std::max(uy1, selY1); }
  for (uint32_t r = 0; r < h; ++r) {
    int yy = y + int(r);
    if (yy < 0 || yy >= H) continue;
    for (uint32_t c = 0; c < w; ++c) {
      int xx = x + int(c);
      if (xx < 0 || xx >= W) continue;
      uint8_t v = cov[size_t(r) * w + c];
      uint8_t& d = selCpu[size_t(yy) * W + xx];
      if (op == 2) d = uint8_t(std::min<int>(d, 255 - v));
      else d = std::max(d, v);
    }
  }
  ux0 = std::max(ux0, 0); uy0 = std::max(uy0, 0); ux1 = std::min(ux1, W); uy1 = std::min(uy1, H);
  // new bounding box
  int bx0 = W, by0 = H, bx1 = 0, by1 = 0;
  for (int yy = uy0; yy < uy1; ++yy) {
    const uint8_t* row = selCpu.data() + size_t(yy) * W;
    for (int xx = ux0; xx < ux1; ++xx)
      if (row[xx]) { bx0 = std::min(bx0, xx); bx1 = std::max(bx1, xx + 1); by0 = std::min(by0, yy); by1 = std::max(by1, yy + 1); }
  }
  std::string err;
  if (ux0 < ux1 && uy0 < uy1) R.uploadSelection(selCpu.data(), ux0, uy0, uint32_t(ux1 - ux0), uint32_t(uy1 - uy0), err);
  selActive = bx0 < bx1;
  if (selActive) { selX0 = bx0; selY0 = by0; selX1 = bx1; selY1 = by1; }
  R.setSelectionActive(selActive);
}

void App::selectAll() {
  if (!R.hasDocument()) return;
  std::vector<uint8_t> all(size_t(R.docW) * R.docH, 255);
  combineSelection(0, 0, R.docW, R.docH, all, 0);
}

void App::deselect() {
  if (!selActive) return;
  snapshotSelection(selX0, selY0, selX1, selY1);
  std::vector<uint8_t> none;
  int W = int(R.docW);
  for (int yy = selY0; yy < selY1; ++yy) memset(selCpu.data() + size_t(yy) * W + selX0, 0, size_t(selX1 - selX0));
  std::string err;
  R.uploadSelection(selCpu.data(), selX0, selY0, uint32_t(selX1 - selX0), uint32_t(selY1 - selY0), err);
  selActive = false;
  R.setSelectionActive(false);
}

void App::invertSelection() {
  if (!R.hasDocument()) return;
  size_t n = size_t(R.docW) * R.docH;
  if (selCpu.size() != n) selCpu.assign(n, 0);
  std::vector<uint8_t> inv(n);
  for (size_t i = 0; i < n; ++i) inv[i] = uint8_t(255 - selCpu[i]);
  selActive = false;
  combineSelection(0, 0, R.docW, R.docH, inv, 0);
}

// Edit > Fill (colour) or Delete (erase) inside the selection, or the whole layer without one.
void App::fillSelection(bool erase) {
  if (!R.hasDocument() || R.busy() || !needPixelLayer()) return;
  int x0 = 0, y0 = 0, x1 = int(R.docW), y1 = int(R.docH);
  if (selActive) { x0 = selX0; y0 = selY0; x1 = selX1; y1 = selY1; }
  std::vector<uint8_t> cov(size_t(x1 - x0) * (y1 - y0), 255);  // the commit clips to the selection
  StrokeStyle st = currentStyle(erase);
  st.opacity = 1;
  std::string err;
  if (!R.paintCoverage(active, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), cov.data(), st, err) && !err.empty()) error(err);
}

// Flood fill on the active layer or on all visible layers (premultiplied RGBA compared per
// channel). Only the painted bounds are read back; everything outside them has one known value
// (transparent, or the paper colour when sampling all layers).
void App::startFlood(double dx, double dy, bool isFill, bool hover) {
  if (flooding || !R.hasDocument()) return;
  int W = int(R.docW), H = int(R.docH);
  int sx = int(std::floor(dx)), sy = int(std::floor(dy));
  if (sx < 0 || sy < 0 || sx >= W || sy >= H) return;
  bool all = isFill ? fillSampleAll : wandSampleAll;
  int bx0 = W, by0 = H, bx1 = 0, by1 = 0;
  auto grow = [&](const Layer& l) {
    if (l.bx0 >= l.bx1) return;
    bx0 = std::min(bx0, l.bx0); by0 = std::min(by0, l.by0); bx1 = std::max(bx1, l.bx1); by1 = std::max(by1, l.by1);
  };
  if (all) { for (auto& l : R.layers) if (l.visible) grow(l); }
  else grow(R.layers[active]);
  uint8_t outside[4] = {0, 0, 0, 0};
  if (all && R.whitePaper) {
    for (int k = 0; k < 3; ++k) outside[k] = uint8_t(std::clamp(R.paperColor[k], 0.0f, 1.0f) * 255 + 0.5f);
    outside[3] = 255;
  }
  // GPU copies the painted area into host memory; the worker waits for it (the UI never does)
  // the read-back is cached while the picture does not change (hovering re-floods instantly)
  std::shared_ptr<AsyncRead> rd;
  std::string err;
  char key[160];
  snprintf(key, sizeof key, "%llu %llu %u %d %d %d %d %d", (unsigned long long)R.revision, (unsigned long long)R.docSerial,
           R.layers[active].id, all ? 1 : 0, bx0, by0, bx1, by1);
  if (bx0 < bx1) {
    if (floodCache && floodCacheKey == key) {
      rd = floodCache;
    } else {
      R.finishAsyncRead(floodCache);
      rd = R.readRegionAsync(all, active, bx0, by0, uint32_t(bx1 - bx0), uint32_t(by1 - by0), err);
      if (!rd) { error(err.empty() ? "Read-back failed" : err); return; }
      floodCache = rd;
      floodCacheKey = key;
    }
  }
  float tol = isFill ? fillTol : wandTol;
  bool contiguous = isFill ? fillContiguous : wandContiguous;
  bool grow1 = isFill && fillGrow;
  int op = selOp;
  uint32_t layerId = R.layers[active].id;
  std::array<uint8_t, 4> out{outside[0], outside[1], outside[2], outside[3]};
  flooding = true;
  floodRead = rd;
  floodRevision = R.revision;
  floodDoc = R.docSerial;
  floodJob = std::async(std::launch::async, [=]() {
    FloodResult res;
    res.isFill = isFill;
    res.hover = hover;
    res.op = op;
    res.layerId = layerId;
    if (rd && !rd->wait()) return res;
    auto pix = [&](int x, int y) -> const uint8_t* {
      if (x >= bx0 && x < bx1 && y >= by0 && y < by1) return rd->pixel(x, y);
      return out.data();
    };
    const uint8_t* s0 = pix(sx, sy);
    uint8_t seed[4] = {s0[0], s0[1], s0[2], s0[3]};
    int t = int(tol * 255 + 0.5f);
    auto match = [&](int x, int y) {
      const uint8_t* p = pix(x, y);
      for (int k = 0; k < 4; ++k) if (std::abs(int(p[k]) - int(seed[k])) > t) return false;
      return true;
    };
    std::vector<uint8_t> mask(size_t(W) * H, 0);
    int mx0 = W, my0 = H, mx1 = 0, my1 = 0;
    auto mark = [&](int l, int r, int y) {
      memset(&mask[size_t(y) * W + l], 255, size_t(r - l + 1));
      mx0 = std::min(mx0, l); mx1 = std::max(mx1, r + 1); my0 = std::min(my0, y); my1 = std::max(my1, y + 1);
    };
    if (!contiguous) {
      for (int y = 0; y < H; ++y) {
        int run = -1;
        for (int x = 0; x <= W; ++x) {
          bool m = x < W && match(x, y);
          if (m && run < 0) run = x;
          if (!m && run >= 0) { mark(run, x - 1, y); run = -1; }
        }
      }
    } else {
      std::vector<std::pair<int, int>> stack{{sx, sy}};
      while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        size_t row = size_t(y) * W;
        if (mask[row + x] || !match(x, y)) continue;
        int l = x, r = x;
        while (l > 0 && !mask[row + l - 1] && match(l - 1, y)) --l;
        while (r + 1 < W && !mask[row + r + 1] && match(r + 1, y)) ++r;
        mark(l, r, y);
        for (int ny : {y - 1, y + 1}) {
          if (ny < 0 || ny >= H) continue;
          size_t nrow = size_t(ny) * W;
          bool prev = false;
          for (int i = l; i <= r; ++i) {
            bool ok = !mask[nrow + i] && match(i, ny);
            if (ok && !prev) stack.push_back({i, ny});
            prev = ok;
          }
        }
      }
    }
    if (mx0 >= mx1) return res;
    if (grow1) {  // grow 1 px under anti-aliased line art so no halo is left
      mx0 = std::max(0, mx0 - 1); my0 = std::max(0, my0 - 1); mx1 = std::min(W, mx1 + 1); my1 = std::min(H, my1 + 1);
    }
    res.x = mx0; res.y = my0; res.w = uint32_t(mx1 - mx0); res.h = uint32_t(my1 - my0);
    res.cov.assign(size_t(res.w) * res.h, 0);
    for (int y = my0; y < my1; ++y)
      for (int x = mx0; x < mx1; ++x) {
        uint8_t v = mask[size_t(y) * W + x];
        if (!v && grow1)
          for (int oy = -1; oy <= 1 && !v; ++oy)
            for (int ox = -1; ox <= 1 && !v; ++ox) {
              int xx = x + ox, yy = y + oy;
              if (xx >= 0 && yy >= 0 && xx < W && yy < H && mask[size_t(yy) * W + xx]) v = 255;
            }
        res.cov[size_t(y - my0) * res.w + (x - mx0)] = v;
      }
    res.ok = true;
    return res;
  });
}

void App::pollFlood() {
  if (!flooding || floodJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  flooding = false;
  FloodResult r = floodJob.get();
  floodRead.reset();  // the read-back stays in floodCache for the next hover / click
  if (r.hover) {
    if (R.docSerial != floodDoc || R.revision != floodRevision || !(tool == ToolId::Fill || tool == ToolId::Wand)) return;
    hoverResult = r;
    hoverRevision = floodRevision;
    hoverDoc = floodDoc;
    showHoverPreview();
    return;
  }
  if (!r.ok || !r.w) return;
  // the canvas changed (or another document was opened) while computing: the result is stale
  if (R.docSerial != floodDoc || R.revision != floodRevision) {
    toast = r.isFill ? "Fill cancelled - the picture changed meanwhile" : "Selection cancelled - the picture changed meanwhile";
    toastUntil = SDL_GetTicksNS() + 3000000000ull;
    return;
  }
  if (r.isFill) {
    int li = R.indexOf(r.layerId);
    if (li < 0) return;
    std::string err;
    if (!R.paintCoverage(li, r.x, r.y, r.w, r.h, r.cov.data(), currentStyle(eraserToggle), err) && !err.empty()) error(err);
  } else {
    combineSelection(r.x, r.y, r.w, r.h, r.cov, r.op);
  }
}

// ---------------------------------------------------------------------------
// Transform

void App::startTransform() {
  if (xf.active || !R.hasDocument() || R.busy()) return;
  if (!needPixelLayer()) { tool = ToolId::Brush; return; }
  int W = int(R.docW), H = int(R.docH);
  std::string err;
  int x0, y0, x1, y1;
  std::vector<uint8_t> px;
  R.waitIdle();
  if (selActive) {
    x0 = selX0; y0 = selY0; x1 = selX1; y1 = selY1;
    if (!R.readLayerRegion(active, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), px, err)) { error(err); return; }
  } else {
    // whole layer: read back only its tracked painted bounds, then find the exact bounds inside
    const Layer& L = R.layers[active];
    int bx0 = L.bx0, by0 = L.by0, bx1 = L.bx1, by1 = L.by1;
    if (bx0 >= bx1) { error("The layer is empty - nothing to transform."); tool = ToolId::Brush; return; }
    int bw = bx1 - bx0;
    if (!R.readLayerRegion(active, bx0, by0, uint32_t(bw), uint32_t(by1 - by0), px, err)) { error(err); return; }
    x0 = W; y0 = H; x1 = 0; y1 = 0;
    for (int y = by0; y < by1; ++y)
      for (int x = bx0; x < bx1; ++x)
        if (px[(size_t(y - by0) * bw + (x - bx0)) * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
    if (x0 >= x1) { error("The layer is empty - nothing to transform."); tool = ToolId::Brush; return; }
    std::vector<uint8_t> crop(size_t(x1 - x0) * (y1 - y0) * 4);
    for (int y = y0; y < y1; ++y)
      memcpy(&crop[size_t(y - y0) * (x1 - x0) * 4], &px[(size_t(y - by0) * bw + (x0 - bx0)) * 4], size_t(x1 - x0) * 4);
    px.swap(crop);
  }
  uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
  std::vector<uint8_t> cov(size_t(w) * h, 255);
  if (selActive)
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t s = selCpu[size_t(y0 + int(y)) * W + x0 + int(x)];
        cov[size_t(y) * w + x] = s;
        uint8_t* p = &px[(size_t(y) * w + x) * 4];
        for (int k = 0; k < 4; ++k) p[k] = uint8_t((p[k] * s + 127) / 255);
      }
  unpremultiply(px);
  if (!R.createFloating(w, h, px.data(), xf.fl, err)) { error(err); return; }
  // lift the pixels off the layer (one undo step; Cancel undoes it without leaving a redo)
  StrokeStyle st = currentStyle(true);
  st.opacity = 1;
  if (!R.paintCoverage(active, x0, y0, w, h, cov.data(), st, err)) { R.destroyFloating(xf.fl); error(err); return; }
  xf.active = true;
  xf.layerId = R.layers[active].id;
  xf.srcX = x0;
  xf.srcY = y0;
  double q[4][2] = {{double(x0), double(y0)}, {double(x1), double(y0)}, {double(x1), double(y1)}, {double(x0), double(y1)}};
  memcpy(xf.q, q, sizeof q);
  xf.drag = 0;
  deselect();
  tool = ToolId::Transform;
}

void App::applyTransform() {
  if (!xf.active) return;
  while (R.busy()) {  // the lift must be committed first
    Renderer::FrameParams p;
    p.view = view;
    p.activeLayer = active;
    R.renderFrame(p);
  }
  double src[4][2] = {{0, 0}, {double(xf.fl.w), 0}, {double(xf.fl.w), double(xf.fl.h)}, {0, double(xf.fl.h)}};
  double hm[9], inv[9];
  std::string err;
  int li = R.indexOf(xf.layerId);  // the layer the pixels came from, even if another one is selected now
  if (li >= 0 && homography(src, xf.q, hm) && invert3(hm, inv)) {
    double mnx = 1e300, mny = 1e300, mxx = -1e300, mxy = -1e300;
    for (auto& c : xf.q) { mnx = std::min(mnx, c[0]); mxx = std::max(mxx, c[0]); mny = std::min(mny, c[1]); mxy = std::max(mxy, c[1]); }
    R.stamp(li, xf.fl, inv, int(std::floor(mnx)) - 1, int(std::floor(mny)) - 1, int(std::ceil(mxx)) + 1,
            int(std::ceil(mxy)) + 1, err);
    if (!err.empty()) error(err);
  } else {
    error("The transform is degenerate (corners overlap); cancelled.");
    R.undoDiscard();
  }
  R.destroyFloating(xf.fl);
  xf.active = false;
  tool = ToolId::Brush;
}

void App::cancelTransform() {
  if (!xf.active) return;
  while (R.busy()) {
    Renderer::FrameParams p;
    p.view = view;
    p.activeLayer = active;
    R.renderFrame(p);
  }
  R.undoDiscard();  // put the lifted pixels back
  R.destroyFloating(xf.fl);
  xf.active = false;
  tool = ToolId::Brush;
}

// ---------------------------------------------------------------------------
// Pointer handling (document coordinates + framebuffer-pixel screen position)

bool App::toolDown(double dx, double dy, float sx, float sy) {
  if (tool == ToolId::Text) { t0x = t1x = dx; t0y = t1y = dy; return textDown(dx, dy); }
  if (tool == ToolId::Brush && !altDown) return false;
  selOp = shiftDown ? 1 : altDown ? 2 : 0;
  t0x = t1x = dx;
  t0y = t1y = dy;
  toolDrag = true;
  switch (tool) {
    case ToolId::Brush:  // Alt+click
    case ToolId::Eyedropper:
      picking = true;
      for (int k = 0; k < 3; ++k) pickPrev[k] = color[k];
      R.requestPick(int(sx), int(sy));
      return true;
    case ToolId::Hand:
      drag = Drag::Pan;
      dragX = sx;
      dragY = sy;
      toolDrag = false;
      return true;
    case ToolId::Fill:
    case ToolId::Wand: {
      toolDrag = false;
      // apply exactly what the hover preview shows (if it is current and under the cursor)
      int px = int(std::floor(dx)), py = int(std::floor(dy));
      bool inside = hoverResult.ok && px >= hoverResult.x && py >= hoverResult.y && px < hoverResult.x + int(hoverResult.w) &&
                    py < hoverResult.y + int(hoverResult.h) &&
                    hoverResult.cov[size_t(py - hoverResult.y) * hoverResult.w + size_t(px - hoverResult.x)] > 0;
      if (inside && hoverPreviewOn && hoverRevision == R.revision && hoverDoc == R.docSerial) {
        if (tool == ToolId::Fill) {
          noteColorUsed();
          R.endStroke();  // commits the previewed fill (with undo)
          hoverPreviewOn = false;
        } else {
          FloodResult r = hoverResult;
          cancelPreviews();
          combineSelection(r.x, r.y, r.w, r.h, r.cov, selOp);
        }
        hoverResult = FloodResult{};
        return true;
      }
      cancelPreviews();
      startFlood(dx, dy, tool == ToolId::Fill);
      return true;
    }
    case ToolId::Lasso:
      lasso = {dx, dy};
      return true;
    case ToolId::Transform: {
      if (!xf.active) { startTransform(); toolDrag = false; return true; }
      // corners (screen distance), inside = move, outside = rotate
      float best = 1e9f;
      int bi = -1;
      for (int i = 0; i < 4; ++i) {
        float cx, cy;
        docToScreen(xf.q[i][0], xf.q[i][1], cx, cy);
        float d = std::hypot(cx - sx, cy - sy);
        if (d < best) { best = d; bi = i; }
      }
      memcpy(xf.start, xf.q, sizeof xf.q);
      xf.gx = dx;
      xf.gy = dy;
      if (best < 12 * density) {
        xf.corner = bi;
        xf.drag = ctrlDown ? 3 : 2;
      } else {
        std::vector<double> poly;
        for (auto& c : xf.q) { poly.push_back(c[0]); poly.push_back(c[1]); }
        bool inside = false;
        for (size_t i = 0, j = 3; i < 4; j = i++) {
          if ((poly[2 * i + 1] > dy) != (poly[2 * j + 1] > dy) &&
              dx < (poly[2 * j] - poly[2 * i]) * (dy - poly[2 * i + 1]) / (poly[2 * j + 1] - poly[2 * i + 1]) + poly[2 * i])
            inside = !inside;
        }
        xf.drag = inside ? 1 : 4;
        double cx = 0, cy = 0;
        for (auto& c : xf.q) { cx += c[0] / 4; cy += c[1] / 4; }
        xf.a0 = std::atan2(dy - cy, dx - cx);
      }
      return true;
    }
    default:
      if (isShapePreviewTool()) startShapePreview();
      return true;  // drag tools: gradient, line, shapes, rect/ellipse select
  }
}

// ---- live previews ----

void App::startShapePreview() {
  cancelPreviews();
  StrokeStyle st = currentStyle(eraserToggle || brushes[tipIndex].eraser);
  if (tool == ToolId::Gradient || (shapeFilled && tool != ToolId::Line)) st.buildUp = false;
  R.beginStroke(active, st);
  previewing = true;
  previewDirty = true;
  previewSeed = strokeSeed++;
}

// Redraws the preview from scratch into the stroke mask (GPU); shown exactly like the result.
void App::updateShapePreview() {
  if (!previewing) return;
  previewDirty = false;
  int W = int(R.docW), H = int(R.docH);
  std::string err;
  switch (tool) {
    case ToolId::Gradient: {
      int x0 = 0, y0 = 0, x1 = W, y1 = H;
      if (selActive) { x0 = selX0; y0 = selY0; x1 = selX1; y1 = selY1; }
      if (std::hypot(t1x - t0x, t1y - t0y) >= 1) R.previewGradient(float(t0x), float(t0y), float(t1x), float(t1y), x0, y0, x1, y1);
      else R.previewClear();
      break;
    }
    case ToolId::Line:
    case ToolId::Rect:
    case ToolId::Ellipse: {
      std::vector<double> poly;
      if (tool == ToolId::Line) poly = {t0x, t0y, t1x, t1y};
      else if (std::abs(t1x - t0x) >= 1 && std::abs(t1y - t0y) >= 1)
        poly = tool == ToolId::Rect ? rectPoly(t0x, t0y, t1x, t1y) : ellipsePoly(t0x, t0y, t1x, t1y);
      if (poly.size() < 4) { R.previewClear(); break; }
      if (tool != ToolId::Line && shapeFilled) {
        int ox, oy;
        uint32_t ow, oh;
        std::vector<uint8_t> cov;
        rasterPolygon(poly, W, H, ox, oy, ow, oh, cov);
        if (ow) R.previewCoverage(ox, oy, ow, oh, cov.data(), err);
        break;
      }
      R.previewClear();
      size_t n = poly.size() / 2;
      BrushEngine e;
      StrokeStyle unused;
      applyBrushTips(brushes[tipIndex], unused, e);
      e.begin({poly[0], poly[1], 1.0f}, brushes[tipIndex], previewSeed, false);
      size_t steps = tool == ToolId::Line ? 1 : n;
      for (size_t i = 1; i <= steps; ++i) e.add({poly[2 * (i % n)], poly[2 * (i % n) + 1], 1.0f});
      e.end();
      R.queueDabs(e.out);
      break;
    }
    default:
      break;
  }
}

void App::cancelPreviews() {
  if (previewing || hoverPreviewOn) R.abortStroke();
  previewing = false;
  hoverPreviewOn = false;
}

// Fill / magic wand: flood from the hovered pixel (worker thread, cached read-back) and preview it.
void App::updateHover(double dx, double dy) {
  if (!(tool == ToolId::Fill || tool == ToolId::Wand) || !R.hasDocument()) return;
  int px = int(std::floor(dx)), py = int(std::floor(dy));
  if (px < 0 || py < 0 || px >= int(R.docW) || py >= int(R.docH)) return;
  if (px == hoverX && py == hoverY) return;
  hoverX = px;
  hoverY = py;
  // still inside the area already previewed (and nothing changed)? then it is the same result
  const FloodResult& r = hoverResult;
  if (r.ok && hoverRevision == R.revision && hoverDoc == R.docSerial && r.isFill == (tool == ToolId::Fill) && px >= r.x &&
      py >= r.y && px < r.x + int(r.w) && py < r.y + int(r.h) && r.cov[size_t(py - r.y) * r.w + size_t(px - r.x)] > 0)
    return;
  hoverDirty = true;
}

void App::showHoverPreview() {
  const FloodResult& r = hoverResult;
  if (R.stroking() && !hoverPreviewOn) return;  // a real stroke is in progress
  if (hoverPreviewOn) R.abortStroke();
  hoverPreviewOn = false;
  if (!r.ok || !r.w) return;
  int li = R.indexOf(r.layerId);
  if (li < 0) return;
  StrokeStyle st;
  if (r.isFill) {
    const BrushSettings& b = brushes[tipIndex];
    st.color[0] = color[0]; st.color[1] = color[1]; st.color[2] = color[2];
    st.opacity = b.opacity;
    st.eraser = eraserToggle;
  } else {  // wand: translucent highlight of the area that would be selected
    st.color[0] = 0.35f; st.color[1] = 0.62f; st.color[2] = 1.0f;
    st.opacity = 0.45f;
    st.overlay = true;
  }
  hoverColor[0] = st.color[0]; hoverColor[1] = st.color[1]; hoverColor[2] = st.color[2]; hoverColor[3] = st.opacity;
  R.beginStroke(li, st);
  std::string err;
  R.previewCoverage(r.x, r.y, r.w, r.h, r.cov.data(), err);
  hoverPreviewOn = true;
}

void App::toolMove(double dx, double dy, float sx, float sy) {
  if (!toolDrag) return;
  if (tool == ToolId::Text) { t1x = dx; t1y = dy; textMove(dx, dy); return; }
  if (previewing) previewDirty = true;
  t1x = dx;
  t1y = dy;
  if (shiftDown && (tool == ToolId::Rect || tool == ToolId::Ellipse || tool == ToolId::SelRect || tool == ToolId::SelEllipse) &&
      selOp == 0) {
    double s = std::max(std::abs(t1x - t0x), std::abs(t1y - t0y));  // square / circle
    t1x = t0x + (t1x >= t0x ? s : -s);
    t1y = t0y + (t1y >= t0y ? s : -s);
  }
  if (tool == ToolId::Lasso) {
    size_t n = lasso.size();
    if (std::hypot(dx - lasso[n - 2], dy - lasso[n - 1]) * view.zoom >= 2.0) { lasso.push_back(dx); lasso.push_back(dy); }
  }
  if (tool == ToolId::Eyedropper || (tool == ToolId::Brush && picking)) R.requestPick(int(sx), int(sy));
  if (tool == ToolId::Transform && xf.active && xf.drag) {
    double mx = dx - xf.gx, my = dy - xf.gy;
    if (xf.drag == 1) {
      for (int i = 0; i < 4; ++i) { xf.q[i][0] = xf.start[i][0] + mx; xf.q[i][1] = xf.start[i][1] + my; }
    } else if (xf.drag == 3) {
      xf.q[xf.corner][0] = xf.start[xf.corner][0] + mx;
      xf.q[xf.corner][1] = xf.start[xf.corner][1] + my;
    } else if (xf.drag == 2) {
      // scale in the quad's own axes with the opposite corner fixed
      int o = (xf.corner + 2) % 4;
      double ox = xf.start[o][0], oy = xf.start[o][1];
      double ux = xf.start[1][0] - xf.start[0][0], uy = xf.start[1][1] - xf.start[0][1];
      double vx = xf.start[3][0] - xf.start[0][0], vy = xf.start[3][1] - xf.start[0][1];
      double det = ux * vy - uy * vx;
      if (std::abs(det) < 1e-9) return;
      auto decomp = [&](double px, double py, double& a, double& b) {
        a = (px * vy - py * vx) / det;
        b = (ux * py - uy * px) / det;
      };
      double a0, b0, a1, b1;
      decomp(xf.start[xf.corner][0] - ox, xf.start[xf.corner][1] - oy, a0, b0);
      decomp(dx - ox, dy - oy, a1, b1);
      double su = std::abs(a0) > 1e-9 ? a1 / a0 : 1, sv = std::abs(b0) > 1e-9 ? b1 / b0 : 1;
      if (shiftDown) { double m = (std::abs(su) + std::abs(sv)) / 2; su = su < 0 ? -m : m; sv = sv < 0 ? -m : m; }
      for (int i = 0; i < 4; ++i) {
        double a, b;
        decomp(xf.start[i][0] - ox, xf.start[i][1] - oy, a, b);
        a *= su;
        b *= sv;
        xf.q[i][0] = ox + a * ux + b * vx;
        xf.q[i][1] = oy + a * uy + b * vy;
      }
    } else if (xf.drag == 4) {
      double cx = 0, cy = 0;
      for (auto& c : xf.start) { cx += c[0] / 4; cy += c[1] / 4; }
      double ang = std::atan2(dy - cy, dx - cx) - xf.a0;
      if (shiftDown) ang = std::round(ang / (kPi / 12)) * (kPi / 12);  // 15 degree steps
      double c = std::cos(ang), s = std::sin(ang);
      for (int i = 0; i < 4; ++i) {
        double px = xf.start[i][0] - cx, py = xf.start[i][1] - cy;
        xf.q[i][0] = cx + c * px - s * py;
        xf.q[i][1] = cy + s * px + c * py;
      }
    }
  }
}

void App::toolUp(double dx, double dy) {
  if (tool == ToolId::Text) { textUp(dx, dy); return; }
  if (!toolDrag) return;
  toolDrag = false;
  picking = false;
  std::string err;
  int W = int(R.docW), H = int(R.docH);
  switch (tool) {
    case ToolId::Gradient:
    case ToolId::Line:
    case ToolId::Rect:
    case ToolId::Ellipse: {
      // the live preview already shows the exact result: just commit it
      if (!previewing) startShapePreview();
      updateShapePreview();
      bool empty = tool == ToolId::Gradient ? std::hypot(t1x - t0x, t1y - t0y) < 1
                 : tool == ToolId::Line    ? false
                                           : (std::abs(t1x - t0x) < 1 || std::abs(t1y - t0y) < 1);
      if (empty) R.abortStroke();
      else {
        if (!eraserToggle) noteColorUsed();
        R.endStroke();
      }
      previewing = false;
      break;
    }
    case ToolId::SelRect:
    case ToolId::SelEllipse:
    case ToolId::Lasso: {
      std::vector<double> poly;
      if (tool == ToolId::Lasso) poly = lasso;
      else if (std::abs(t1x - t0x) >= 1 && std::abs(t1y - t0y) >= 1) {
        if (tool == ToolId::SelRect) {
          // rectangles snap to whole pixels (crisp edges)
          poly = rectPoly(std::round(t0x), std::round(t0y), std::round(t1x), std::round(t1y));
        } else {
          poly = ellipsePoly(t0x, t0y, t1x, t1y);
        }
      }
      if (poly.size() < 6) {
        if (selOp == 0) deselect();  // a click without a drag deselects
        break;
      }
      int ox, oy;
      uint32_t ow, oh;
      std::vector<uint8_t> cov;
      rasterPolygon(poly, W, H, ox, oy, ow, oh, cov);
      if (ow) combineSelection(ox, oy, ow, oh, cov, selOp);
      else if (selOp == 0) deselect();
      break;
    }
    case ToolId::Transform:
      xf.drag = 0;
      break;
    default:
      break;
  }
  lasso.clear();
}

void App::toolKey(SDL_Keycode key, bool ctrl, bool shift, bool alt) {
  if (xf.active) {
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { applyTransform(); return; }
    if (key == SDLK_ESCAPE) { cancelTransform(); return; }
  }
  if (key == SDLK_ESCAPE && (previewing || hoverPreviewOn)) {
    cancelPreviews();
    toolDrag = false;
    return;
  }
  if (key == SDLK_ESCAPE && !ctrl && !shift && !alt) deselect();
}

// ---------------------------------------------------------------------------
// UI: tool bar with drawn icons, tool settings, on-canvas overlays

static void drawToolGlyph(ImDrawList* dl, ToolId t, ImVec2 c, float s, ImU32 col) {
  float r = s * 0.36f, th = std::max(1.0f, s * 0.07f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  switch (t) {
    case ToolId::Brush:
      dl->AddLine(P(0.9f, -0.9f), P(-0.2f, 0.2f), col, th * 1.6f);
      dl->AddCircleFilled(P(-0.45f, 0.45f), r * 0.38f, col);
      break;
    case ToolId::Eyedropper:
      dl->AddLine(P(0.2f, -0.2f), P(-0.8f, 0.8f), col, th * 1.5f);
      dl->AddCircleFilled(P(0.55f, -0.55f), r * 0.35f, col);
      break;
    case ToolId::Fill:
      dl->AddQuad(P(-0.8f, -0.1f), P(0.0f, -0.9f), P(0.8f, -0.1f), P(0.0f, 0.7f), col, th);
      dl->AddCircleFilled(P(0.85f, 0.6f), r * 0.2f, col);
      break;
    case ToolId::Gradient:
      dl->AddRectFilledMultiColor(P(-0.9f, -0.6f), P(0.9f, 0.6f), col, col & 0x00FFFFFF, col & 0x00FFFFFF, col);
      dl->AddRect(P(-0.9f, -0.6f), P(0.9f, 0.6f), col, 0, th);
      break;
    case ToolId::Line: dl->AddLine(P(-0.8f, 0.8f), P(0.8f, -0.8f), col, th * 1.4f); break;
    case ToolId::Rect: dl->AddRect(P(-0.8f, -0.6f), P(0.8f, 0.6f), col, 0, th * 1.3f); break;
    case ToolId::Ellipse: dl->AddEllipse(c, ImVec2(r * 0.85f, r * 0.62f), col, 0.0f, 0, th * 1.3f); break;
    case ToolId::SelRect:
      for (int i = 0; i < 4; ++i) {
        dl->AddLine(P(-0.8f + i * 0.45f, -0.6f), P(-0.6f + i * 0.45f, -0.6f), col, th);
        dl->AddLine(P(-0.8f + i * 0.45f, 0.6f), P(-0.6f + i * 0.45f, 0.6f), col, th);
      }
      for (int i = 0; i < 3; ++i) {
        dl->AddLine(P(-0.8f, -0.6f + i * 0.45f), P(-0.8f, -0.4f + i * 0.45f), col, th);
        dl->AddLine(P(0.8f, -0.6f + i * 0.45f), P(0.8f, -0.4f + i * 0.45f), col, th);
      }
      break;
    case ToolId::SelEllipse:
      for (int i = 0; i < 12; i += 2) {
        float a0 = float(i * kPi / 6), a1 = float((i + 1) * kPi / 6);
        dl->AddLine(P(0.85f * std::cos(a0), 0.62f * std::sin(a0)), P(0.85f * std::cos(a1), 0.62f * std::sin(a1)), col, th);
      }
      break;
    case ToolId::Lasso:
      dl->AddEllipse(ImVec2(c.x, c.y - r * 0.2f), ImVec2(r * 0.8f, r * 0.5f), col, 0.3f, 0, th);
      dl->AddBezierQuadratic(P(-0.4f, 0.2f), P(-0.7f, 0.7f), P(-0.2f, 0.95f), col, th);
      break;
    case ToolId::Wand:
      dl->AddLine(P(-0.8f, 0.8f), P(0.3f, -0.3f), col, th * 1.5f);
      dl->AddLine(P(0.6f, -0.95f), P(0.6f, -0.35f), col, th);
      dl->AddLine(P(0.3f, -0.65f), P(0.9f, -0.65f), col, th);
      break;
    case ToolId::Transform:
      dl->AddLine(P(-0.9f, 0), P(0.9f, 0), col, th);
      dl->AddLine(P(0, -0.9f), P(0, 0.9f), col, th);
      for (int k = 0; k < 4; ++k) {
        float a = float(k * kPi / 2);
        ImVec2 tip = P(0.9f * std::cos(a), 0.9f * std::sin(a));
        ImVec2 l = P(0.55f * std::cos(a) - 0.25f * std::sin(a), 0.55f * std::sin(a) + 0.25f * std::cos(a));
        ImVec2 rr = P(0.55f * std::cos(a) + 0.25f * std::sin(a), 0.55f * std::sin(a) - 0.25f * std::cos(a));
        dl->AddTriangleFilled(tip, l, rr, col);
      }
      break;
    case ToolId::Hand:
      dl->AddRect(P(-0.5f, -0.2f), P(0.5f, 0.85f), col, r * 0.2f, th);
      for (int k = 0; k < 4; ++k) dl->AddLine(P(-0.38f + k * 0.25f, -0.2f), P(-0.38f + k * 0.25f, -0.85f + (k == 0 || k == 3 ? 0.2f : 0)), col, th * 1.3f);
      break;
    default: break;
  }
}

// ---- icons: Lucide (ISC licence), rasterised once into a texture atlas ----

void App::loadIcons() {
  const int S = 64;  // raster size (icons are drawn at ~20-28 px, so they stay crisp on high-DPI)
  int n = int(sizeof kLucideIcons / sizeof kLucideIcons[0]);
  int cols = 8, rows = (n + cols - 1) / cols;
  std::vector<uint8_t> atlas(size_t(cols * S) * rows * S * 4, 0);
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  std::vector<uint8_t> tmp(size_t(S) * S * 4);
  for (int i = 0; i < n; ++i) {
    std::string svg = kLucideIcons[i].svg;
    for (size_t p; (p = svg.find("currentColor")) != std::string::npos;) svg.replace(p, 12, "#ffffff");
    NSVGimage* img = nsvgParse(svg.data(), "px", 96.0f);
    if (!img) continue;
    std::fill(tmp.begin(), tmp.end(), 0);
    float sc = S / 24.0f;
    nsvgRasterize(rast, img, 0, 0, sc, tmp.data(), S, S, S * 4);
    nsvgDelete(img);
    int cx = (i % cols) * S, cy = (i / cols) * S;
    for (int y = 0; y < S; ++y) memcpy(&atlas[(size_t(cy + y) * cols * S + cx) * 4], &tmp[size_t(y) * S * 4], size_t(S) * 4);
    float W = float(cols * S), H = float(rows * S);
    iconUV.push_back({kLucideIcons[i].name, {cx / W, cy / H, (cx + S) / W, (cy + S) / H}});
  }
  nsvgDeleteRasterizer(rast);
  std::string err;
  if (!R.createFloating(uint32_t(cols * S), uint32_t(rows * S), atlas.data(), iconAtlas, err)) {
    iconUV.clear();
    SDL_Log("icons: %s", err.c_str());
  }
}

bool App::icon(const char* name, IconUV& uv) const {
  for (auto& [n, u] : iconUV)
    if (n == name) { uv = u; return true; }
  return false;
}

void App::drawIcon(ImDrawList* dl, const char* name, ImVec2 c, float size, ImU32 col) const {
  IconUV uv;
  if (!icon(name, uv) || !iconAtlas.imguiTex) return;
  float h = size * 0.5f;
  dl->AddImage(ImTextureRef(ImTextureID(uint64_t(iconAtlas.imguiTex))), ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h),
               ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1), col);
}

// Square button with a centred icon; returns true when clicked.
bool App::iconButton(const char* id, const char* name, float size, bool selected, const char* tip, bool enabled) {
  ImGui::BeginDisabled(!enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
  bool r = ImGui::Button(id, ImVec2(size, size));
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
  ImU32 col = ImGui::GetColorU32(!enabled ? ImGuiCol_TextDisabled : selected ? ImGuiCol_Text : ImGuiCol_Text,
                                 enabled && !selected ? 0.72f : 1.0f);
  drawIcon(ImGui::GetWindowDrawList(), name, ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), std::min(size * 0.62f, 28.0f), col);
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
  return r;
}

void App::drawToolbar() {
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
  ImGui::SetNextWindowClass(&wc);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 3));
  if (!ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoScrollbar)) { ImGui::End(); ImGui::PopStyleVar(2); return; }
  float bs = std::min(ImGui::GetContentRegionAvail().x, 40.0f * ImGui::GetStyle().FontScaleDpi);
  bs = std::max(bs, 24.0f);
  int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + 2) / (bs + 2)));
  struct Entry { Act act; const char* icon; const char* extra; int group; };
  static const Entry entries[] = {
      {Act::ToolBrush, "paintbrush", "1-9 pick a brush", 0},
      {Act::ToolEraser, "eraser", nullptr, 0},
      {Act::ToolEyedropper, "pipette", "or Alt+click", 0},
      {Act::ToolFill, "paint-bucket", nullptr, 0},
      {Act::ToolGradient, nullptr, nullptr, 0},
      {Act::ToolLine, "slash", nullptr, 1},
      {Act::ToolRect, "square", nullptr, 1},
      {Act::ToolEllipse, "circle", nullptr, 1},
      {Act::ToolText, "type", nullptr, 1},
      {Act::ToolSelRect, "square-dashed", nullptr, 2},
      {Act::ToolSelEllipse, "circle-dashed", nullptr, 2},
      {Act::ToolLasso, "lasso", nullptr, 2},
      {Act::ToolWand, "wand-sparkles", nullptr, 2},
      {Act::ToolTransform, "move", nullptr, 3},
      {Act::ToolHand, "hand", nullptr, 3}};
  int i = 0, lastGroup = 0;
  for (const Entry& e : entries) {
    if (e.group != lastGroup) {
      ImGui::Dummy(ImVec2(0, 1));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0, 1));
      lastGroup = e.group;
      i = 0;
    }
    ImGui::PushID(&e);
    if (i % perRow) ImGui::SameLine();
    bool sel = toolActActive(e.act);
    std::string keysTxt = shortcutLabel(e.act);
    if (e.act == Act::ToolHand) keysTxt += (keysTxt.empty() ? "" : ", ") + std::string("hold ") + comboLabel(keys[int(Act::PanHold)][0]);
    if (e.extra) keysTxt += (keysTxt.empty() ? "" : ", ") + std::string(e.extra);
    char tip[200];
    if (keysTxt.empty()) snprintf(tip, sizeof tip, "%s", actionInfo(e.act).name);
    else snprintf(tip, sizeof tip, "%s  (%s)%s", actionInfo(e.act).name, keysTxt.c_str(),
                  prefs.springTools ? "\nHold the key to use it only while held" : "");
    bool clicked;
    if (e.icon) {
      clicked = iconButton("##t", e.icon, bs, sel, tip);
    } else {  // gradient: no suitable icon in the set, draw it
      ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
      clicked = ImGui::Button("##t", ImVec2(bs, bs));
      ImGui::PopStyleColor();
      ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
      drawToolGlyph(ImGui::GetWindowDrawList(), ToolId::Gradient, ImVec2((mn.x + mx.x) / 2, (mn.y + mx.y) / 2), bs * 0.8f,
               ImGui::GetColorU32(ImGuiCol_Text, sel ? 1.0f : 0.72f));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    }
    if (clicked) selectToolAct(e.act);
    ImGui::PopID();
    ++i;
  }
  // current colour swatch
  ImGui::Dummy(ImVec2(0, 4));
  ImGui::ColorButton("##cur", ImVec4(color[0], color[1], color[2], 1), ImGuiColorEditFlags_NoTooltip, ImVec2(bs, bs));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current colour");
  // brush size and opacity always at hand (vertical sliders, like CSP / Procreate)
  float left = ImGui::GetContentRegionAvail().y - 8;
  if (left > 120) {
    float vh = std::min(180.0f, (left - 8) / 2);
    float vw = std::max(8.0f, (bs - ImGui::GetStyle().ItemSpacing.x) / 2);
    BrushSettings& b = brushes[tipIndex];
    ImGui::VSliderFloat("##vsize", ImVec2(vw, vh), &b.size, 1.0f, 5000.0f, "", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Brush size %.1f px  ([ ])", b.size);
    ImGui::SameLine();
    ImGui::VSliderFloat("##vop", ImVec2(vw, vh), &b.opacity, 0.0f, 1.0f, "");
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Brush opacity %.0f %%", b.opacity * 100);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void App::drawToolOptions() {
  if (tool == ToolId::Brush) return;  // the brush settings follow in the panel
  const char* name = "Brush";
  for (auto& t : kTools) if (t.id == tool) name = t.name;
  ImGui::SeparatorText(name);
  switch (tool) {
    case ToolId::Fill:
      ImGui::SliderFloat("Tolerance", &fillTol, 0.0f, 1.0f, "%.2f");
      ImGui::Checkbox("Contiguous", &fillContiguous);
      ImGui::Checkbox("Grow 1 px under line art", &fillGrow);
      ImGui::Checkbox("Refer to all visible layers", &fillSampleAll);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Find the area from the whole picture (e.g. line art on another layer)\nand fill it on the current layer");
      ImGui::TextDisabled("Fills on the active layer. %s fills the selection.", comboLabel(keys[int(Act::FillSel)][0]).c_str());
      break;
    case ToolId::Wand:
      ImGui::SliderFloat("Tolerance", &wandTol, 0.0f, 1.0f, "%.2f");
      ImGui::Checkbox("Contiguous", &wandContiguous);
      ImGui::Checkbox("Refer to all visible layers##w", &wandSampleAll);
      [[fallthrough]];
    case ToolId::SelRect:
    case ToolId::SelEllipse:
    case ToolId::Lasso:
      ImGui::TextDisabled("Shift: add   Alt: subtract   %s all\n%s deselect   %s invert\n%s clears, %s fills",
                          comboLabel(keys[int(Act::SelectAll)][0]).c_str(), comboLabel(keys[int(Act::Deselect)][0]).c_str(),
                          comboLabel(keys[int(Act::InvertSel)][0]).c_str(), comboLabel(keys[int(Act::ClearSel)][0]).c_str(),
                          comboLabel(keys[int(Act::FillSel)][0]).c_str());
      if (ImGui::Button("Select all")) selectAll();
      ImGui::SameLine();
      if (ImGui::Button("Deselect")) deselect();
      ImGui::SameLine();
      if (ImGui::Button("Invert")) invertSelection();
      break;
    case ToolId::Rect:
    case ToolId::Ellipse:
      ImGui::Checkbox("Filled", &shapeFilled);
      ImGui::TextDisabled("Outline uses the brush below. Shift: square / circle.");
      break;
    case ToolId::Line:
      ImGui::TextDisabled("Drag to draw a straight line with the brush below.\nWith the Brush: Shift+click continues from the last stroke.");
      break;
    case ToolId::Gradient:
      ImGui::TextDisabled("Drag: colour -> transparent (inside the selection if any).");
      break;
    case ToolId::Eyedropper:
      ImGui::TextDisabled("Click or drag to pick the visible colour.\nAlt+click picks while painting.");
      break;
    case ToolId::Transform:
      if (!xf.active) { ImGui::TextDisabled("Click the canvas to transform the layer / selection."); break; }
      ImGui::TextDisabled("Drag inside: move   corners: scale (Shift keeps ratio)\nCtrl+corner: distort   outside: rotate (Shift 15 deg)");
      if (ImGui::Button("Flip H")) {
        double q[4][2]; memcpy(q, xf.q, sizeof q);
        int m[4] = {1, 0, 3, 2};
        for (int k = 0; k < 4; ++k) { xf.q[k][0] = q[m[k]][0]; xf.q[k][1] = q[m[k]][1]; }
      }
      ImGui::SameLine();
      if (ImGui::Button("Flip V")) {
        double q[4][2]; memcpy(q, xf.q, sizeof q);
        int m[4] = {3, 2, 1, 0};
        for (int k = 0; k < 4; ++k) { xf.q[k][0] = q[m[k]][0]; xf.q[k][1] = q[m[k]][1]; }
      }
      {
        // numeric scale / rotation (resets any corner distortion)
        double cx = 0, cy = 0;
        for (auto& c : xf.q) { cx += c[0] / 4; cy += c[1] / 4; }
        double ex = xf.q[1][0] - xf.q[0][0], ey = xf.q[1][1] - xf.q[0][1];
        double fx = xf.q[3][0] - xf.q[0][0], fy = xf.q[3][1] - xf.q[0][1];
        float sw = float(std::hypot(ex, ey) / std::max(1u, xf.fl.w) * 100), sh = float(std::hypot(fx, fy) / std::max(1u, xf.fl.h) * 100);
        float ang = float(std::atan2(ey, ex) * 180 / kPi);
        bool ch = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x / 3 - 4);
        ch |= ImGui::DragFloat("##tw", &sw, 0.5f, 1, 2000, "W %.1f %%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x / 2 - 4);
        ch |= ImGui::DragFloat("##th", &sh, 0.5f, 1, 2000, "H %.1f %%");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ch |= ImGui::DragFloat("##ta", &ang, 0.5f, -360, 360, "%.1f deg");
        if (ch) {
          double hw = xf.fl.w * sw / 200.0, hh = xf.fl.h * sh / 200.0, a = ang * kPi / 180, c = std::cos(a), s = std::sin(a);
          double loc[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
          for (int k = 0; k < 4; ++k) {
            xf.q[k][0] = cx + c * loc[k][0] - s * loc[k][1];
            xf.q[k][1] = cy + s * loc[k][0] + c * loc[k][1];
          }
        }
      }
      if (ImGui::Button("Apply (Enter)")) applyTransform();
      ImGui::SameLine();
      if (ImGui::Button("Cancel (Esc)")) cancelTransform();
      break;
    case ToolId::Hand:
      ImGui::TextDisabled("Drag to pan. Wheel zooms, %s+drag rotates.", comboLabel(keys[int(Act::RotateHold)][0]).c_str());
      break;
    default:
      break;
  }
}

void App::drawToolOverlay() {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float s = ImGui::GetIO().DisplayFramebufferScale.x;
  // everything drawn here belongs to the canvas: never over the panels
  ImVec2 clip0(canvasX / s, canvasY / s), clip1((canvasX + canvasW) / s, (canvasY + canvasH) / s);
  dl->PushClipRect(clip0, clip1, true);
  ImGui::GetBackgroundDrawList()->PushClipRect(clip0, clip1, true);
  struct PopClips { ImDrawList* a; ~PopClips() { a->PopClipRect(); ImGui::GetBackgroundDrawList()->PopClipRect(); } } popClips{dl};
  if (tool == ToolId::Text || textEdit.active) drawTextOverlay(dl);
  auto S = [&](double dx, double dy) { float x, y; docToScreen(dx, dy, x, y); return ImVec2(x / s, y / s); };
  ImU32 white = IM_COL32(255, 255, 255, 220), black = IM_COL32(0, 0, 0, 220);
  auto poly = [&](const std::vector<double>& p, bool closed) {
    std::vector<ImVec2> pts;
    for (size_t i = 0; i + 1 < p.size(); i += 2) pts.push_back(S(p[i], p[i + 1]));
    if (pts.size() < 2) return;
    dl->AddPolyline(pts.data(), int(pts.size()), black, 3.0f, closed ? ImDrawFlags_Closed : 0);
    dl->AddPolyline(pts.data(), int(pts.size()), white, 1.0f, closed ? ImDrawFlags_Closed : 0);
  };
  if (toolDrag) {
    switch (tool) {
      case ToolId::Line:
      case ToolId::Gradient: poly({t0x, t0y, t1x, t1y}, false); break;
      case ToolId::Rect:
      case ToolId::SelRect: poly(rectPoly(t0x, t0y, t1x, t1y), true); break;
      case ToolId::Ellipse:
      case ToolId::SelEllipse: poly(ellipsePoly(t0x, t0y, t1x, t1y), true); break;
      case ToolId::Lasso: poly(lasso, false); break;
      default: break;
    }
  }
  if (picking) {  // eyedropper ring: new colour on top, previous colour below
    ImVec2 c(lastX / s, lastY / s);
    ImU32 nc = ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], 1));
    ImU32 pc = ImGui::ColorConvertFloat4ToU32(ImVec4(pickPrev[0], pickPrev[1], pickPrev[2], 1));
    dl->PathArcTo(c, 34, float(kPi), float(2 * kPi), 24);
    dl->PathStroke(nc, 14.0f);
    dl->PathArcTo(c, 34, 0, float(kPi), 24);
    dl->PathStroke(pc, 14.0f);
    dl->AddCircle(c, 42, IM_COL32(0, 0, 0, 160), 48, 1.0f);
    dl->AddCircle(c, 26, IM_COL32(0, 0, 0, 160), 48, 1.0f);
  }
  if (xf.active) {
    ImVec2 p[4];
    for (int i = 0; i < 4; ++i) p[i] = S(xf.q[i][0], xf.q[i][1]);
    // live preview of the floating pixels (the real resampling happens on Apply)
    ImGui::GetBackgroundDrawList()->AddImageQuad(ImTextureRef(ImTextureID(uint64_t(xf.fl.imguiTex))), p[0], p[1], p[2], p[3]);
    dl->AddPolyline(p, 4, black, 3.0f, ImDrawFlags_Closed);
    dl->AddPolyline(p, 4, white, 1.0f, ImDrawFlags_Closed);
    for (auto& c : p) {
      dl->AddRectFilled(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), black);
      dl->AddRectFilled(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), white);
    }
  }
}

// ---------------------------------------------------------------------------
// --tooltest: scripted scene exercising every tool (automated visual test)

void App::buildToolTest() {
  auto drag = [this](ToolId t, double x0, double y0, double x1, double y1, int op = 0) {
    return [=, this] {
      tool = t;
      selOp = op;
      t0x = x0; t0y = y0; t1x = x1; t1y = y1;
      if (t == ToolId::Lasso) {
        lasso.clear();
        for (int i = 0; i < 40; ++i) {
          double a = i * 2 * kPi / 40, r = 110 + 40 * std::sin(a * 5);
          lasso.push_back(x0 + r * std::cos(a));
          lasso.push_back(y0 + r * std::sin(a));
        }
      }
      toolDrag = true;
      toolUp(x1, y1);
    };
  };
  auto rgb = [this](float r, float g, float b) { return [=, this] { color[0] = r; color[1] = g; color[2] = b; }; };
  demo.push_back([this] { newDocument(1600, 1000, true); });
  // selection + Edit > Fill: red rectangle, then a blue gradient clipped to an ellipse selection
  demo.push_back(drag(ToolId::SelRect, 100, 100, 500, 400));
  demo.push_back(rgb(0.85f, 0.15f, 0.15f));
  demo.push_back([this] { fillSelection(false); });
  demo.push_back(drag(ToolId::SelEllipse, 600, 100, 1000, 400));
  demo.push_back(rgb(0.1f, 0.3f, 0.9f));
  demo.push_back(drag(ToolId::Gradient, 600, 250, 1000, 250));
  // subtract a hole from the selection with Alt, then fill it (hole stays empty)
  demo.push_back(drag(ToolId::SelRect, 1100, 100, 1500, 400));
  demo.push_back(drag(ToolId::SelEllipse, 1200, 175, 1400, 325, 2));
  demo.push_back(rgb(0.2f, 0.6f, 0.3f));
  demo.push_back([this] { fillSelection(false); });
  demo.push_back([this] { deselect(); });
  // line + shape tools
  demo.push_back(rgb(0.05f, 0.05f, 0.05f));
  demo.push_back([this] { tipIndex = legacyTip(0); brushes[tipIndex].size = 8; });
  demo.push_back(drag(ToolId::Line, 100, 480, 1500, 480));
  demo.push_back(drag(ToolId::Rect, 100, 550, 400, 800));
  demo.push_back([this] { shapeFilled = true; color[0] = 0.95f; color[1] = 0.75f; color[2] = 0.1f; });
  demo.push_back(drag(ToolId::Ellipse, 450, 550, 750, 800));
  demo.push_back([this] { shapeFilled = false; });
  // fill tool: fill inside the outlined rectangle
  demo.push_back(rgb(0.6f, 0.3f, 0.8f));
  demo.push_back([this] { tool = ToolId::Fill; toolDown(250, 700, 0, 0); });
  // magic wand on the yellow ellipse + Delete clears it (only the centre part via subtract)
  demo.push_back([this] { tool = ToolId::Wand; selOp = 0; toolDown(600, 675, 0, 0); });
  demo.push_back(drag(ToolId::SelRect, 450, 550, 600, 800, 2));
  demo.push_back([this] { fillSelection(true); deselect(); });
  // transform: rotate + scale the red rectangle's right half
  demo.push_back(drag(ToolId::SelRect, 300, 100, 500, 400));
  demo.push_back([this] { startTransform(); });
  demo.push_back([this] {
    double cx = 400, cy = 250, a = 0.35;
    for (auto& c : xf.q) {
      double x = (c[0] - cx) * 0.8, y = (c[1] - cy) * 0.8;
      c[0] = cx + 850 + std::cos(a) * x - std::sin(a) * y;
      c[1] = cy + 550 + std::sin(a) * x + std::cos(a) * y;
    }
  });
  demo.push_back([this] { applyTransform(); });
  // layer undo: paint a teal dot on a new layer, delete the layer, undo -> the dot must come back;
  // then reorder + undo/redo, and a property change + undo
  demo.push_back([this] { addLayer(); });
  demo.push_back(rgb(0.0f, 0.55f, 0.55f));
  demo.push_back([this] { tipIndex = legacyTip(0); brushes[tipIndex].size = 120; strokeBegin({1450, 150, 1.0f}, false); strokeAdd({1451, 150, 1.0f}); strokeEnd(); });
  demo.push_back([this] { R.deleteLayer(active); active = 0; });
  demo.push_back([this] { R.undo(); });
  demo.push_back([this] { active = 1; R.moveLayerTo(1, 0); });
  demo.push_back([this] { R.undo(); });
  demo.push_back([this] { R.redo(); });
  demo.push_back([this] { R.undo(); });
  demo.push_back([this] { R.recordLayerProps(1); R.layers[1].opacity = 0.2f; R.markCachesDirty(); });
  demo.push_back([this] { R.undo(); });  // opacity back to 100 %
  demo.push_back([this] { brushes[legacyTip(0)].size = 8; });
  // fill on the (empty) top layer, referring to all visible layers: fills the hole in the green rectangle
  demo.push_back([this] { active = int(R.layers.size()) - 1; fillSampleAll = true; color[0] = 1.0f; color[1] = 0.55f; color[2] = 0.1f; });
  demo.push_back([this] { tool = ToolId::Fill; toolDown(1300, 250, 0, 0); });
  demo.push_back([this] { fillSampleAll = false; });
  // lasso selection left active: marching ants
  demo.push_back(drag(ToolId::Lasso, 1350, 700, 0, 0));
  demo.push_back([this] { tool = ToolId::Brush; });
  // edit operations: select the red rectangle, copy it to a new layer, hue-shift and flip it
  demo.push_back([this] {
    active = 0;
    std::vector<uint8_t> cov(size_t(220) * 320, 255);
    combineSelection(90, 90, 220, 320, cov, 0);
    copySelection(false, true);
  });
  demo.push_back([] {});
  demo.push_back([this] {
    float p[4] = {0.33f, 0, 0, 0};
    std::string err;
    R.applyAdjust(active, 2, p, err);  // red -> green-ish on the pasted layer
    std::vector<uint8_t> cov(size_t(220) * 320, 255);
    combineSelection(90, 90, 220, 320, cov, 0);
    growSelection(12);  // -> 244 x 344
    fprintf(stderr, "edittest: layers %zu, pasted '%s', selection %dx%d\n", R.layers.size(), R.layers[active].name.c_str(),
            selX1 - selX0, selY1 - selY0);
  });
  demo.push_back([this] { deselect(); transformLayer(0); });
  // text: a committed line, then a text frame still being edited (live preview + caret)
  demo.push_back([this] {
    fprintf(stderr, "texttest: %zu font families\n", fonts.families().size());
    for (const FontFamily& f : fonts.families())
      if (f.name.find("Serif") != std::string::npos) { textStyle.family = f.name; break; }
    textStyle.style = "Bold";
    textStyle.size = 72;
    textStyle.tracking = 40;
    textStyle.underline = true;
    setTool(ToolId::Text);
    textDown(1000, 470);
    textUp(1000, 470);
    textInsert(U"Saraswati Text");
  });
  demo.push_back([] {});
  demo.push_back([this] {
    commitText();
    textStyle = TextStyle{};
    textStyle.size = 30;
    textStyle.align = 3;
    textStyle.skew = 12;
    textDown(980, 560);
    t1x = 1480; t1y = 700;
    textUp(1480, 700);
    textInsert(U"A text frame wraps its lines at the frame width and this paragraph is justified.\nSecond paragraph");
    fprintf(stderr, "texttest: layers %zu, active '%s'\n", R.layers.size(), R.layers[active].name.c_str());
  });
  demo.push_back([] {});
  // speech bubble with a freehand tail, committed as text layer + bubble sublayer
  demo.push_back([this] {
    commitText();
    textStyle = TextStyle{};
    textStyle.size = 28;
    textStyle.align = 1;
    bubbleStyle = BubbleStyle{};
    bubbleStyle.enabled = true;
    color[0] = color[1] = color[2] = 0.05f;
    setTool(ToolId::Text);
    textDown(800, 660);
    textUp(800, 660);
    textInsert(U"Hello!\nIs this a bubble?");
  });
  demo.push_back([] {});
  demo.push_back([this] {
    textEdit.tails.push_back({{800, 700}, {760, 760}, {700, 820}, {640, 850}, {600, 870}});
    textEdit.dirty = true;
  });
  demo.push_back([] {});
  demo.push_back([this] {
    commitText();
    const Layer& t = R.layers[size_t(active)];
    const Layer& b = R.layers[size_t(active - 1)];
    fprintf(stderr, "bubbletest: '%s' over '%s' (sublayer %s)\n", t.name.c_str(), b.name.c_str(), b.parentId == t.id ? "yes" : "NO");
  });
  // folders: a Multiply folder (its contents are blended on their own, then multiplied)
  demo.push_back([this] {
    commitText();
    std::string err;
    int fi = R.addFolder(int(R.layers.size()), err);
    Layer& f = R.layers[size_t(fi)];
    f.name = "Multiply folder";
    f.passThrough = false;
    f.mode = BlendMode::Multiply;
    uint32_t fid = f.id;
    for (int i = 0; i < int(R.layers.size()); ++i)
      if (R.layers[size_t(i)].name == "Layer 2") { active = i; dropLayerBlock(i, R.indexOf(fid)); break; }
    std::string st;
    for (const Layer& l : R.layers) st += (l.folder ? "[" : "") + l.name + (l.folder ? "]" : "") + (l.folderId ? "<in" : "") + ", ";
    fprintf(stderr, "foldertest: %s\n", st.c_str());
    R.markCachesDirty();
  });
  demo.push_back([] {});
  // clipping: magenta stripes clipped to the green rectangle; a warm paper colour
  demo.push_back([this] {
    int base = -1;
    for (int i = 0; i < int(R.layers.size()); ++i) if (R.layers[size_t(i)].name == "Layer 1 copy") base = i;
    if (base < 0) return;
    std::string err;
    int ci = R.addLayer(base + 1, err);
    R.layers[size_t(ci)].name = "Clipped stripes";
    R.layers[size_t(ci)].clip = true;
    std::vector<uint8_t> cov(size_t(300) * 400);
    for (int y = 0; y < 400; ++y)
      for (int x = 0; x < 300; ++x) cov[size_t(y) * 300 + x] = ((x + y) / 20) % 2 ? 255 : 0;
    StrokeStyle st;
    st.color[0] = 0.9f; st.color[1] = 0.1f; st.color[2] = 0.7f;
    R.paintCoverage(ci, 60, 60, 300, 400, cov.data(), st, err);
    R.paperColor[0] = 1.0f; R.paperColor[1] = 0.96f; R.paperColor[2] = 0.86f;
    R.markCachesDirty();
    fprintf(stderr, "cliptest: clipped layer at %d over '%s'\n", ci, R.layers[size_t(base)].name.c_str());
  });
  demo.push_back([] {});
  // layer effects: tone on the bottom layer, layer colour on a text layer (non-destructive)
  demo.push_back([this] {
    commitText();
    Layer& b = R.layers[0];
    b.tone.on = true;
    b.tone.frequency = 30;
    b.tone.express = 1;
    for (Layer& l : R.layers)
      if (l.name.rfind("Text: A", 0) == 0) { l.lcolor.on = true; }
    R.markCachesDirty();
  });
  demo.push_back([] {});
  // automatic backup: written in the background, then restored and compared
  demo.push_back([this] {
    if (userData.empty()) { fprintf(stderr, "backuptest: skipped (no User-Data)\n"); return; }
    backupTestLayers = R.layers.size();
    startBackup();
  });
  demo.push_back([] {});
  demo.push_back([this] {
    if (userData.empty()) return;
    listBackups();
    bool ok = !backups.empty() && restoreBackup(backups.front().path);
    fprintf(stderr, "backuptest: %s (%zu layers restored of %zu, %s)\n", ok && R.layers.size() == backupTestLayers ? "ok" : "FAILED",
            R.layers.size(), backupTestLayers, backups.empty() ? "no file" : fmtBytes(double(backups.front().bytes)).c_str());
  });
  // keyboard: tap switches tools, hold (used on the canvas) returns to the previous tool
  demo.push_back([this] {
    auto key = [&](SDL_Keycode k, bool down, SDL_Keymod mod = 0) {
      SDL_KeyboardEvent e{};
      e.key = k;
      e.mod = mod;
      e.down = down;
      handleKey(e, down);
    };
    bool ok = true;
    auto expect = [&](bool c, const char* what) { if (!c) { ok = false; fprintf(stderr, "keytest FAILED: %s\n", what); } };
    setTool(ToolId::Brush); eraserToggle = false;
    key(SDLK_E, true);  expect(tool == ToolId::Brush && eraserToggle, "E selects the eraser");
    toolHold.used = true;  // (a stroke while E is held)
    key(SDLK_E, false); expect(tool == ToolId::Brush && !eraserToggle, "releasing a used E returns to the brush");
    key(SDLK_G, true);  key(SDLK_G, false); expect(tool == ToolId::Fill, "tapping G keeps the fill tool");
    key(SDLK_G, true, SDL_KMOD_LSHIFT); key(SDLK_G, false, SDL_KMOD_LSHIFT); expect(tool == ToolId::Gradient, "Shift+G = gradient");
    key(SDLK_SPACE, true); expect(spaceDown, "Space holds pan"); key(SDLK_SPACE, false); expect(!spaceDown, "Space released");
    int ui = hideUI; key(SDLK_TAB, true); key(SDLK_TAB, false); expect(int(hideUI) != ui, "Tab hides panels"); hideUI = ui;
    // rebinding: give the eraser F, which frees nothing; then G steals from Fill
    captureAct = int(Act::ToolEraser); captureSlot = 0; key(SDLK_G, true); key(SDLK_G, false);
    expect(keys[int(Act::ToolFill)][0].empty() && findAction(KeyCombo{SDLK_G, 0}) == int(Act::ToolEraser), "rebinding moves the key");
    resetShortcuts(); captureMsg.clear();
    setTool(ToolId::Brush);
    fprintf(stderr, "keytest: %s\n", ok ? "ok" : "FAILED");
  });
  // magic-wand hover preview over the red rectangle (tinted, not yet selected)
  demo.push_back([this] { setTool(ToolId::Wand); wandSampleAll = true; updateHover(230, 300); });
  demo.push_back([] {});
}
