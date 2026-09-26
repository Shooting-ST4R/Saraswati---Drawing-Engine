#include "bubble.h"
#include <algorithm>
#include <cmath>

static constexpr double kPi = 3.14159265358979323846;

BubbleGeom bubbleGeometry(const BubbleStyle& st, double tx0, double ty0, double tx1, double ty1) {
  BubbleGeom g;
  g.cx = (tx0 + tx1) * 0.5;
  g.cy = (ty0 + ty1) * 0.5;
  double w = std::max(1.0, tx1 - tx0) * 0.5, h = std::max(1.0, ty1 - ty0) * 0.5;
  if (st.shape == 1 || st.shape == 2) {  // boxes hug the text rectangle
    g.hx = w + st.padX;
    g.hy = h + st.padY;
  } else {  // round shapes: the text rectangle's corners sit on the ellipse (sqrt 2), plus the distance
    g.hx = w * 1.41421356 + st.padX;
    g.hy = h * 1.41421356 + st.padY;
    if (st.shape == 3 || st.shape == 4) {  // room for the bumps / spikes
      g.hx += st.amount * g.hx * 0.5;
      g.hy += st.amount * g.hy * 0.5;
    }
  }
  return g;
}

static double sdEllipse(double px, double py, double a, double b) {
  // Inigo Quilez's cheap ellipse distance (good near the curve, which is all we need)
  double k1 = std::hypot(px / a, py / b), k2 = std::hypot(px / (a * a), py / (b * b));
  if (k2 < 1e-12) return -std::min(a, b);
  return k1 * (k1 - 1.0) / k2;
}

double bubbleShapeDistance(const BubbleStyle& st, const BubbleGeom& g, double x, double y) {
  double px = x - g.cx, py = y - g.cy;
  switch (st.shape) {
    case 1:
    case 2: {
      double r = st.shape == 1 ? std::min<double>(st.corner, std::min(g.hx, g.hy)) : 0.0;
      double qx = std::abs(px) - (g.hx - r), qy = std::abs(py) - (g.hy - r);
      return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
    }
    case 3:
    case 4: {
      double qx = px / g.hx, qy = py / g.hy;
      double rho = std::hypot(qx, qy), th = std::atan2(qy, qx);
      int n = std::max(3, st.bumps);
      double t = th / (2 * kPi) * n;
      double f = t - std::floor(t);  // 0..1 across one bump / spike
      double edge;
      if (st.shape == 3) edge = 1.0 - st.amount + st.amount * std::sin(f * kPi);  // scallops (cusps between bumps)
      else {
        // spikes with a little irregularity per spike, like hand-drawn bursts
        int k = int(std::floor(t)) % n;
        if (k < 0) k += n;
        double jitter = 0.75 + 0.5 * std::fmod(std::sin(k * 12.9898) * 43758.5453, 1.0) * (std::sin(k * 12.9898) < 0 ? -1 : 1);
        edge = 1.0 - st.amount * std::clamp(jitter, 0.4, 1.2) * (1.0 - std::abs(f * 2 - 1));  // valleys between tips
        edge = 1.0 - st.amount + (edge - (1.0 - st.amount));
        edge = 1.0 - st.amount * std::abs(f * 2 - 1) * std::clamp(jitter, 0.4, 1.2);
      }
      return (rho - edge) * std::min(g.hx, g.hy);
    }
    default:
      return sdEllipse(px, py, g.hx, g.hy);
  }
}

namespace {
struct Poly {
  std::vector<BubblePt> p;
  double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
};

// signed distance to a closed polygon (negative inside, even-odd)
double sdPoly(const Poly& poly, double x, double y) {
  const auto& v = poly.p;
  double d = 1e30;
  bool inside = false;
  for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) {
    double ex = v[j].x - v[i].x, ey = v[j].y - v[i].y, wx = x - v[i].x, wy = y - v[i].y;
    double l = ex * ex + ey * ey;
    double t = l > 0 ? std::clamp((wx * ex + wy * ey) / l, 0.0, 1.0) : 0.0;
    double dx = wx - ex * t, dy = wy - ey * t;
    d = std::min(d, dx * dx + dy * dy);
    if ((v[i].y > y) != (v[j].y > y) && x < (v[j].x - v[i].x) * (y - v[i].y) / (v[j].y - v[i].y) + v[i].x) inside = !inside;
  }
  return inside ? -std::sqrt(d) : std::sqrt(d);
}

// tail centre line -> ribbon polygon: full width inside the bubble, tapering to a point outside
Poly tailPolygon(const BubbleStyle& st, const BubbleGeom& g, const BubbleTail& raw) {
  Poly out;
  if (raw.size() < 2) return out;
  BubbleTail line;
  line.push_back({g.cx, g.cy});  // always starts in the middle of the bubble
  if (st.tailMode == 1) line.push_back(raw.back());
  else {
    // resample the hand-drawn path every few px and smooth it lightly (no wobble at the tip)
    double step = std::max(3.0, st.tailWidth * 0.15);
    BubblePt last = raw.front();
    line.push_back(last);
    for (size_t i = 1; i < raw.size(); ++i) {
      double d = std::hypot(raw[i].x - last.x, raw[i].y - last.y);
      if (d >= step || i + 1 == raw.size()) { line.push_back(raw[i]); last = raw[i]; }
    }
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i = 2; i + 1 < line.size(); ++i) {
        line[i].x = (line[i - 1].x + 2 * line[i].x + line[i + 1].x) / 4;
        line[i].y = (line[i - 1].y + 2 * line[i].y + line[i + 1].y) / 4;
      }
  }
  // arc length outside the bubble
  size_t n = line.size();
  std::vector<double> sOut(n, 0);
  double total = 0;
  for (size_t i = 1; i < n; ++i) {
    double seg = std::hypot(line[i].x - line[i - 1].x, line[i].y - line[i - 1].y);
    bool outside = bubbleShapeDistance(st, g, line[i].x, line[i].y) > 0;
    total += outside ? seg : 0;
    sOut[i] = total;
  }
  if (total <= 0) return out;
  std::vector<BubblePt> left, right;
  for (size_t i = 0; i < n; ++i) {
    size_t a = i == 0 ? 0 : i - 1, b = std::min(n - 1, i + 1);
    double dx = line[b].x - line[a].x, dy = line[b].y - line[a].y, l = std::hypot(dx, dy);
    if (l < 1e-9) continue;
    double nx = -dy / l, ny = dx / l;
    double w = st.tailWidth * 0.5 * std::pow(std::max(0.0, 1.0 - sOut[i] / total), 0.9);
    left.push_back({line[i].x + nx * w, line[i].y + ny * w});
    right.push_back({line[i].x - nx * w, line[i].y - ny * w});
  }
  out.p = left;
  out.p.push_back(line.back());
  for (auto it = right.rbegin(); it != right.rend(); ++it) out.p.push_back(*it);
  for (auto& q : out.p) {
    out.x0 = std::min(out.x0, q.x); out.y0 = std::min(out.y0, q.y);
    out.x1 = std::max(out.x1, q.x); out.y1 = std::max(out.y1, q.y);
  }
  return out;
}
}  // namespace

void renderBubble(const BubbleStyle& st, const BubbleGeom& g, const std::vector<BubbleTail>& tails, BubbleImage& out) {
  out = BubbleImage{};
  out.geom = g;
  double m = st.borderWidth + 2;
  double x0 = g.cx - g.hx - m, y0 = g.cy - g.hy - m, x1 = g.cx + g.hx + m, y1 = g.cy + g.hy + m;
  std::vector<Poly> polys;
  for (const BubbleTail& t : tails) {
    Poly p = tailPolygon(st, g, t);
    if (p.p.size() < 3) continue;
    x0 = std::min(x0, p.x0 - m); y0 = std::min(y0, p.y0 - m);
    x1 = std::max(x1, p.x1 + m); y1 = std::max(y1, p.y1 + m);
    polys.push_back(std::move(p));
  }
  out.x0 = int(std::floor(x0));
  out.y0 = int(std::floor(y0));
  out.w = uint32_t(std::max(0, int(std::ceil(x1)) - out.x0));
  out.h = uint32_t(std::max(0, int(std::ceil(y1)) - out.y0));
  if (!out.w || !out.h || uint64_t(out.w) * out.h > (400ull << 20)) { out.w = out.h = 0; return; }
  out.rgba.assign(size_t(out.w) * out.h * 4, 0);
  double bw = std::max(0.0f, st.borderWidth);
  for (uint32_t yy = 0; yy < out.h; ++yy) {
    double y = out.y0 + yy + 0.5;
    for (uint32_t xx = 0; xx < out.w; ++xx) {
      double x = out.x0 + xx + 0.5;
      double d = bubbleShapeDistance(st, g, x, y);
      for (const Poly& p : polys)
        if (x >= p.x0 - m && x <= p.x1 + m && y >= p.y0 - m && y <= p.y1 + m) d = std::min(d, sdPoly(p, x, y));
      if (d > 1.0) continue;
      double outer = std::clamp(0.5 - d, 0.0, 1.0);
      double inner = std::clamp(0.5 - (d + bw), 0.0, 1.0);
      double ba = (outer - inner) * st.borderOpacity, fa = inner * st.fillOpacity;
      double a = ba + fa * (1 - ba);
      if (a <= 0) continue;
      uint8_t* o = &out.rgba[(size_t(yy) * out.w + xx) * 4];
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::clamp((st.border[k] * ba + st.fill[k] * fa * (1 - ba)) / a, 0.0, 1.0) * 255 + 0.5);
      o[3] = uint8_t(std::clamp(a, 0.0, 1.0) * 255 + 0.5);
    }
  }
}
