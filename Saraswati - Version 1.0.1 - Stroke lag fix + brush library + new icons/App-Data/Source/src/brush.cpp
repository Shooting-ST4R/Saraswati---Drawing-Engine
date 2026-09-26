#include "brush.h"
#include <algorithm>
#include <cmath>

static constexpr double kPi = 3.14159265358979323846;

std::vector<BrushSettings> builtInBrushes() {
  std::vector<BrushSettings> v;
  auto add = [&](const char* group, const char* name, auto&& setup) {
    BrushSettings b;
    b.group = group;
    b.name = name;
    b.builtIn = true;
    setup(b);
    v.push_back(b);
  };
  // Pencils
  add("Pencil", "Pencil", [](BrushSettings& b) {
    b.size = 5; b.hardness = 0.85f; b.texStrength = 0.85f; b.texScale = 2.5f; b.spacing = 0.1f;
    b.minSize = 0.5f; b.pressureOpacity = true; b.gamma = 1.2f;
  });
  add("Pencil", "Mechanical pencil", [](BrushSettings& b) {
    b.size = 3; b.texStrength = 0.55f; b.texScale = 2.0f; b.minSize = 0.8f; b.pressureOpacity = true;
  });
  add("Pencil", "Soft pencil (6B)", [](BrushSettings& b) {
    b.size = 9; b.hardness = 0.7f; b.texStrength = 1.0f; b.texScale = 3.5f; b.minSize = 0.4f; b.pressureOpacity = true;
    b.gamma = 0.8f;
  });
  // Ink
  add("Ink", "G-Pen", [](BrushSettings& b) { b.size = 10; b.minSize = 0.05f; b.gamma = 1.3f; b.spacing = 0.05f; });
  add("Ink", "Mapping pen", [](BrushSettings& b) { b.size = 4; b.minSize = 0.2f; b.spacing = 0.05f; });
  add("Ink", "Hard round", [](BrushSettings& b) { b.size = 12; });
  add("Ink", "Textured pen", [](BrushSettings& b) {
    b.size = 24; b.texStrength = 0.7f; b.texScale = 6; b.spacing = 0.05f;
  });
  add("Ink", "Calligraphy", [](BrushSettings& b) {
    b.size = 24; b.roundness = 0.22f; b.angle = 45; b.minSize = 0.3f; b.spacing = 0.03f;
  });
  // Markers
  add("Marker", "Marker", [](BrushSettings& b) {
    b.size = 30; b.hardness = 0.9f; b.opacity = 0.6f; b.pressureSize = false; b.spacing = 0.05f;
  });
  add("Marker", "Chisel marker", [](BrushSettings& b) {
    b.size = 36; b.hardness = 0.95f; b.roundness = 0.35f; b.angle = 30; b.opacity = 0.65f; b.pressureSize = false;
    b.spacing = 0.04f;
  });
  add("Marker", "Highlighter", [](BrushSettings& b) {
    b.size = 40; b.hardness = 0.95f; b.roundness = 0.3f; b.angle = 70; b.opacity = 0.35f; b.pressureSize = false;
    b.spacing = 0.04f;
  });
  // Paint & texture
  add("Paint", "Flat brush", [](BrushSettings& b) {
    b.size = 40; b.hardness = 0.8f; b.roundness = 0.3f; b.followStroke = true; b.angle = 90; b.minSize = 0.5f;
    b.pressureOpacity = true; b.spacing = 0.04f;
  });
  add("Paint", "Dry brush", [](BrushSettings& b) {
    b.size = 45; b.hardness = 0.7f; b.roundness = 0.35f; b.followStroke = true; b.angle = 90; b.texStrength = 1.0f;
    b.texScale = 3; b.pressureOpacity = true; b.spacing = 0.04f;
  });
  add("Paint", "Chalk", [](BrushSettings& b) {
    b.size = 40; b.hardness = 0.7f; b.texStrength = 1.0f; b.texScale = 10; b.scatter = 0.04f; b.minSize = 0.6f;
    b.pressureOpacity = true;
  });
  add("Paint", "Charcoal", [](BrushSettings& b) {
    b.size = 30; b.hardness = 0.75f; b.roundness = 0.45f; b.angle = 20; b.texStrength = 0.9f; b.texScale = 5;
    b.flow = 0.6f; b.buildUp = true; b.pressureOpacity = true; b.minSize = 0.6f;
  });
  add("Paint", "Soft round", [](BrushSettings& b) {
    b.size = 150; b.hardness = 0.0f; b.flow = 0.15f; b.spacing = 0.1f; b.pressureSize = false; b.pressureOpacity = true;
  });
  // Airbrush
  add("Airbrush", "Soft airbrush", [](BrushSettings& b) {
    b.size = 200; b.hardness = 0.0f; b.flow = 0.08f; b.buildUp = true; b.spacing = 0.08f; b.pressureSize = false;
    b.pressureOpacity = true;
  });
  add("Airbrush", "Hard airbrush", [](BrushSettings& b) {
    b.size = 80; b.hardness = 0.6f; b.flow = 0.15f; b.buildUp = true; b.spacing = 0.08f; b.pressureSize = false;
    b.pressureOpacity = true;
  });
  add("Airbrush", "Spray", [](BrushSettings& b) {
    b.size = 90; b.hardness = 0.8f; b.flow = 0.7f; b.buildUp = true; b.particles = 14; b.particleSize = 2.0f;
    b.spacing = 0.35f; b.pressureSize = false; b.pressureOpacity = true;
  });
  add("Airbrush", "Coarse spray", [](BrushSettings& b) {
    b.size = 140; b.hardness = 0.7f; b.flow = 0.8f; b.buildUp = true; b.particles = 8; b.particleSize = 5.0f;
    b.sizeJitter = 0.6f; b.spacing = 0.4f; b.pressureSize = false; b.pressureOpacity = true;
  });
  // Erasers
  add("Eraser", "Hard eraser", [](BrushSettings& b) { b.size = 30; b.eraser = true; b.pressureSize = false; });
  add("Eraser", "Soft eraser", [](BrushSettings& b) {
    b.size = 120; b.hardness = 0.0f; b.flow = 0.3f; b.buildUp = true; b.eraser = true; b.pressureSize = false;
    b.pressureOpacity = true;
  });
  return v;
}

float BrushEngine::rnd() {
  // xorshift32: deterministic per stroke (reproducible tests)
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return float(rng_ & 0xffffff) / float(0x1000000);
}

float BrushEngine::diameterAt(float pressure) const {
  float p = std::pow(std::clamp(pressure, 0.0f, 1.0f), b_.gamma);
  return b_.size * (b_.pressureSize ? (b_.minSize + (1.0f - b_.minSize) * p) : 1.0f);
}

void BrushEngine::emit(double x, double y, float pressure) {
  float p = std::pow(std::clamp(pressure, 0.0f, 1.0f), b_.gamma);
  float r = diameterAt(pressure) * 0.5f;
  float a = b_.flow * (b_.pressureOpacity ? p : 1.0f);
  if (b_.sizeJitter > 0 && b_.particles == 0) r *= 1.0f - b_.sizeJitter * rnd();
  if (b_.flowJitter > 0) a *= 1.0f - b_.flowJitter * rnd();
  if (b_.scatter > 0) {
    float ang = rnd() * float(2 * kPi), d = std::sqrt(rnd()) * b_.scatter * r * 2;
    x += d * std::cos(ang);
    y += d * std::sin(ang);
  }
  double ang = b_.angle * kPi / 180.0;
  if (b_.followStroke) ang += std::atan2(dirY_, dirX_);
  float ca = float(std::cos(ang)), sa = float(std::sin(ang));
  float invRound = 1.0f / std::clamp(b_.roundness, 0.05f, 1.0f);
  if (a <= 0.0f) return;
  if (b_.particles > 0) {  // spray: dots spread over the dab's disc
    for (int i = 0; i < b_.particles; ++i) {
      float pa = rnd() * float(2 * kPi), pd = std::sqrt(rnd()) * r;
      float pr = std::max(0.5f, b_.particleSize * 0.5f * (1.0f - b_.sizeJitter * rnd()));
      out.push_back({float(x + pd * std::cos(pa)), float(y + pd * std::sin(pa)), pr, a, 1, 0, 1, 0});
    }
    return;
  }
  if (r < 0.5f) {  // sub-pixel dab: keep a 1 px footprint, scale alpha by area
    a *= (r * r) / 0.25f;
    r = 0.5f;
  }
  out.push_back({float(x), float(y), r, a, ca, sa, invRound, 0});
}

void BrushEngine::begin(const PenSample& s, const BrushSettings& b, uint32_t seed, bool spline) {
  b_ = b;
  spline_ = spline;
  pts_.clear();
  pts_.push_back(s);
  fx_ = s.x;
  fy_ = s.y;
  active_ = true;
  last_ = s;
  rng_ = seed * 2654435761u + 12345u;
  if (!rng_) rng_ = 1;
  dirX_ = 1;
  dirY_ = 0;
  emit(s.x, s.y, s.pressure);
  toNext_ = std::max(0.5, double(b_.spacing) * diameterAt(s.pressure));
}

void BrushEngine::add(const PenSample& s) {
  if (!active_) return;
  if (!spline_) { walkTo(s); return; }
  PenSample in = s;
  if (quant_ > 0.25) {
    // Pixel-snapped input: a change of the snapped value means the pen just crossed the border
    // between two screen pixels, i.e. it is really at the midpoint. Use those crossing points.
    if (std::abs(s.x - fx_) < 1e-9 && std::abs(s.y - fy_) < 1e-9) {  // same pixel: pressure only
      pts_.back().pressure = s.pressure;
      return;
    }
    in.x = std::abs(s.x - fx_) > 1e-9 ? (s.x + fx_) * 0.5 : s.x;
    in.y = std::abs(s.y - fy_) > 1e-9 ? (s.y + fy_) * 0.5 : s.y;
    fx_ = s.x;
    fy_ = s.y;
  }
  const PenSample& prev = pts_.back();
  if (std::hypot(s.x - prev.x, s.y - prev.y) < 0.05) {  // no movement: only the pressure changed
    pts_.back().pressure = s.pressure;
    return;
  }
  pts_.push_back(in);
  if (pts_.size() == 3) curveSegment(pts_[0], pts_[0], pts_[1], pts_[2]);
  else if (pts_.size() == 4) {
    curveSegment(pts_[0], pts_[1], pts_[2], pts_[3]);
    pts_.erase(pts_.begin());
  }
}

void BrushEngine::end() {
  if (active_ && spline_) {  // draw the last segment (its "next" point is the end point itself)
    size_t n = pts_.size();
    if (n == 2) curveSegment(pts_[0], pts_[0], pts_[1], pts_[1]);
    else if (n >= 3) curveSegment(pts_[n - 3], pts_[n - 2], pts_[n - 1], pts_[n - 1]);
  }
  pts_.clear();
  active_ = false;
}

// Centripetal Catmull-Rom segment p1 -> p2 (Barry-Goldman form), walked as short straight pieces.
void BrushEngine::curveSegment(const PenSample& p0, const PenSample& p1, const PenSample& p2, const PenSample& p3) {
  auto knot = [](const PenSample& a, const PenSample& b) {
    return std::max(1e-4, std::sqrt(std::hypot(b.x - a.x, b.y - a.y)));
  };
  double t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
  double len = std::hypot(p2.x - p1.x, p2.y - p1.y);
  int pieces = std::clamp(int(std::ceil(len / 2.0)), 1, 256);
  for (int i = 1; i <= pieces; ++i) {
    double t = t1 + (t2 - t1) * i / pieces;
    auto lerp = [](double a, double b, double ta, double tb, double tt) {
      return tb - ta < 1e-9 ? b : (a * (tb - tt) + b * (tt - ta)) / (tb - ta);
    };
    double a1x = lerp(p0.x, p1.x, t0, t1, t), a1y = lerp(p0.y, p1.y, t0, t1, t);
    double a2x = lerp(p1.x, p2.x, t1, t2, t), a2y = lerp(p1.y, p2.y, t1, t2, t);
    double a3x = lerp(p2.x, p3.x, t2, t3, t), a3y = lerp(p2.y, p3.y, t2, t3, t);
    double b1x = lerp(a1x, a2x, t0, t2, t), b1y = lerp(a1y, a2y, t0, t2, t);
    double b2x = lerp(a2x, a3x, t1, t3, t), b2y = lerp(a2y, a3y, t1, t3, t);
    double cx = lerp(b1x, b2x, t1, t2, t), cy = lerp(b1y, b2y, t1, t2, t);
    float pr = float(p1.pressure + (p2.pressure - p1.pressure) * double(i) / pieces);
    walkTo({cx, cy, pr});
  }
}

void BrushEngine::walkTo(const PenSample& s) {
  double dx = s.x - last_.x, dy = s.y - last_.y;
  double len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0) { last_.pressure = s.pressure; return; }
  dirX_ = dx / len;
  dirY_ = dy / len;
  double t = 0;  // distance travelled along this segment
  while (len - t >= toNext_) {
    t += toNext_;
    double f = t / len;
    float p = float(last_.pressure + (s.pressure - last_.pressure) * f);
    emit(last_.x + dx * f, last_.y + dy * f, p);
    toNext_ = std::max(0.5, double(b_.spacing) * diameterAt(p));
  }
  toNext_ -= (len - t);
  last_ = s;
}
