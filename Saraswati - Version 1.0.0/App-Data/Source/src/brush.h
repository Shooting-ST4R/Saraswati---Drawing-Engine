// CPU side of the brush: turns pen samples into evenly spaced dabs (no smoothing).
#pragma once
#include "renderer.h"
#include <vector>

struct BrushSettings {
  Tip tip = Tip::Hard;
  float size = 20;         // diameter in document px (1..5000)
  float opacity = 1.0f;    // whole stroke
  float flow = 1.0f;       // per dab
  float spacing = 0.08f;   // fraction of the current diameter
  float hardness = 0.5f;   // soft round
  float texStrength = 0.7f, texScale = 6.0f;  // textured pen
  bool pressureSize = true;
  float minSize = 0.2f;    // fraction of size at zero pressure
  bool pressureOpacity = false;
  float gamma = 1.0f;      // pressure curve
};

struct PenSample { double x, y; float pressure; };

class BrushEngine {
 public:
  void begin(const PenSample& s, const BrushSettings& b);
  void add(const PenSample& s);
  bool active() const { return active_; }
  void end() { active_ = false; }
  std::vector<Dab> out;  // dabs produced since last take()
 private:
  void emit(double x, double y, float pressure);
  float diameterAt(float pressure) const;
  BrushSettings b_;
  PenSample last_{};
  double toNext_ = 0;
  bool active_ = false;
};
