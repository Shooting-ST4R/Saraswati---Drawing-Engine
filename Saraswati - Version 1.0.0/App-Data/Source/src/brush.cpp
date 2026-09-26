#include "brush.h"
#include <algorithm>
#include <cmath>

float BrushEngine::diameterAt(float pressure) const {
  float p = std::pow(std::clamp(pressure, 0.0f, 1.0f), b_.gamma);
  return b_.size * (b_.pressureSize ? (b_.minSize + (1.0f - b_.minSize) * p) : 1.0f);
}

void BrushEngine::emit(double x, double y, float pressure) {
  float p = std::pow(std::clamp(pressure, 0.0f, 1.0f), b_.gamma);
  float r = diameterAt(pressure) * 0.5f;
  float a = b_.flow * (b_.pressureOpacity ? p : 1.0f);
  if (r < 0.5f) {  // sub-pixel dab: keep a 1 px footprint, scale alpha by area
    a *= (r * r) / 0.25f;
    r = 0.5f;
  }
  if (a <= 0.0f) return;
  out.push_back({float(x), float(y), r, a});
}

void BrushEngine::begin(const PenSample& s, const BrushSettings& b) {
  b_ = b;
  active_ = true;
  last_ = s;
  emit(s.x, s.y, s.pressure);
  toNext_ = std::max(0.5, double(b_.spacing) * diameterAt(s.pressure));
}

void BrushEngine::add(const PenSample& s) {
  if (!active_) return;
  double dx = s.x - last_.x, dy = s.y - last_.y;
  double len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0) { last_.pressure = s.pressure; return; }
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
