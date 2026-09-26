// CPU side of the brush: turns pen samples into evenly spaced dabs (no smoothing).
#pragma once
#include "renderer.h"
#include <cstdint>
#include <string>
#include <vector>

// One brush of the library. Every brush is a round/elliptical tip with optional paper grain,
// scatter, jitter and spray particles; the combinations give pencils, pens, markers, chalk,
// airbrushes, sprays and erasers.
struct BrushSettings {
  std::string name = "Brush", group = "Custom";
  bool builtIn = false;
  float size = 20;          // diameter in document px (1..5000)
  float opacity = 1.0f;     // whole stroke
  float flow = 1.0f;        // per dab
  float spacing = 0.08f;    // fraction of the current diameter
  float hardness = 1.0f;    // 1 = crisp anti-aliased edge, 0 = fully soft falloff
  float roundness = 1.0f;   // 1 = round, smaller = flat/elliptical tip
  float angle = 0.0f;       // tip angle in degrees
  bool followStroke = false;  // tip angle follows the stroke direction
  float texStrength = 0.0f, texScale = 6.0f;  // paper grain fixed in canvas space
  float scatter = 0.0f;     // random offset, fraction of the diameter
  float sizeJitter = 0.0f, flowJitter = 0.0f;
  bool buildUp = false;     // airbrush accumulation inside a stroke (else even, marker-like)
  int particles = 0;        // spray: dots per dab (0 = normal dabs)
  float particleSize = 2.0f;
  bool pressureSize = true;
  float minSize = 0.2f;     // fraction of size at zero pressure
  bool pressureOpacity = false;
  float gamma = 1.0f;       // pressure curve
  bool eraser = false;      // this brush erases
};

std::vector<BrushSettings> builtInBrushes();

struct PenSample { double x, y; float pressure; };

class BrushEngine {
 public:
  void begin(const PenSample& s, const BrushSettings& b, uint32_t seed);
  void add(const PenSample& s);
  bool active() const { return active_; }
  const PenSample& lastSample() const { return last_; }
  void end() { active_ = false; }
  std::vector<Dab> out;  // dabs produced since last take()
 private:
  void emit(double x, double y, float pressure);
  float diameterAt(float pressure) const;
  float rnd();  // 0..1
  BrushSettings b_;
  PenSample last_{};
  double toNext_ = 0;
  double dirX_ = 1, dirY_ = 0;
  uint32_t rng_ = 1;
  bool active_ = false;
};
