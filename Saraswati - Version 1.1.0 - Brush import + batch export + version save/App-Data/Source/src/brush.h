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
  // --- imported brushes (Photoshop .abr / Clip Studio .sut) ---
  std::vector<std::string> tips;  // sampled tip images (tip library names); empty = round tip
  int tipOrder = 0;               // several tips: 0 in order, 1 random
  bool flipXJitter = false, flipYJitter = false;
  float angleJitter = 0;          // 0..1 of a full turn
  float roundJitter = 0;          // 0..1
  int count = 1;                  // dabs per step (scattering)
  bool bothAxes = true;           // scatter in all directions (else across the stroke)
  std::string texture;            // pattern image (replaces the paper grain); texStrength = depth
  float patScale = 1;             // pattern scale (1 = one texel per document px)
  int texMode = 0;                // 0 multiply, 1 subtract, 2 darken, 3 height
  bool texInvert = false;
  float texBrightness = 0, texContrast = 0;  // -1..1
  std::string dualTip;            // dual brush: second tip multiplied in
  float dualScale = 1;
  float taperIn = 0;              // px: the stroke starts thin and grows over this length
  bool pressureFlow = false;
  std::string notes;              // what the importer could not reproduce
};

std::vector<BrushSettings> builtInBrushes();
std::string brushExtraToString(const BrushSettings& b);  // imported-brush fields ("key=value;...")
void brushExtraFromString(BrushSettings& b, const std::string& s);

struct PenSample { double x, y; float pressure; };

class BrushEngine {
 public:
  // spline = interpolate a centripetal Catmull-Rom curve THROUGH the pen samples (not a
  // stabiliser: the line still passes exactly through every sample). Removes the faceted /
  // wobbly look when zoomed out, where each input sample jumps many document pixels. Costs one
  // sample of delay (~4 ms at 240 Hz). Straight-line tools pass spline = false.
  void begin(const PenSample& s, const BrushSettings& b, uint32_t seed, bool spline = true);
  void add(const PenSample& s);
  // Input positions arrive snapped to whole screen pixels; zoomed out, one screen pixel is many
  // document pixels and the stroke turns into a staircase. q = half a screen pixel in document
  // units. When q is significant, pixel-crossing midpoints are used as spline points (the pen's
  // real position at that moment), correcting at most the snapping error itself - no lag or
  // drift like a stabiliser.
  void setQuantization(double q) { quant_ = q; }
  std::vector<int> tipSlots;  // atlas slots of the brush's tips (set before begin)
  bool active() const { return active_; }
  const PenSample& lastSample() const { return pts_.empty() ? last_ : pts_.back(); }
  void end();
  std::vector<Dab> out;  // dabs produced since last take()
 private:
  void emit(double x, double y, float pressure);
  void walkTo(const PenSample& s);  // straight segment from last_ to s, emitting spaced dabs
  void curveSegment(const PenSample& p0, const PenSample& p1, const PenSample& p2, const PenSample& p3);
  std::vector<PenSample> pts_;      // spline control points not yet fully drawn
  bool spline_ = true;
  double quant_ = 0;
  double fx_ = 0, fy_ = 0;  // last raw (snapped) input position
  float diameterAt(float pressure) const;
  float rnd();  // 0..1
  BrushSettings b_;
  PenSample last_{};
  double toNext_ = 0;
  double dirX_ = 1, dirY_ = 0;
  uint32_t rng_ = 1;
  double dist_ = 0, emX_ = 0, emY_ = 0;  // stroke length so far (start taper)
  bool emitted_ = false;
  uint32_t tipCounter_ = 0;
  bool active_ = false;
};
