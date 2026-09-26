// Manga tone support on the CPU: the dot shapes (shared with the shaders) and their areas.
#include "renderer.h"
#include <cmath>

namespace tone_shapes_cpp {
// the tiny subset of GLSL that tone_shapes.glsl uses
struct vec2 {
  float x, y;
  vec2(float a, float b) : x(a), y(b) {}
};
inline float abs(float v) { return std::fabs(v); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float floor(float v) { return std::floor(v); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float exp(float v) { return std::exp(v); }
inline float atan(float y, float x) { return std::atan2(y, x); }
#include "tone_shapes.glsl"
}  // namespace tone_shapes_cpp

const char* const* toneShapeNames() {
  static const char* names[] = {"Circle", "Square", "Lozenge", "Line", "Cross", "Ellipse", "Noise", "Sugar plum",
                                "Asterisk", "Star", "Carrot", "Cherry (round)", "Cherry (mid)", "Cherry (thin)",
                                "Flower (round)", "Flower (mid)", "Flower (thin)", "Clover (round)", "Clover (thin)",
                                "Ninja star", "Diamond", "Heart", "Clubs", "Spades", "Oval (chain dot)", "Triangle",
                                "Hexagon", "Wave lines", "Concentric circles", "Random dots"};
  return names;
}

// Area of {distance < 0} for the unit dot, measured once per shape (the dot is scaled so that its
// area equals the tone density).
float toneShapeArea(int shape) {
  static float areas[30] = {};
  if (shape >= 27) shape = 0;  // wave lines / rings are line tones, random dots are circles
  if (shape < 0 || shape >= 30) return 3.14159265f;
  if (areas[shape] == 0) {
    const int N = 400;
    const float R = 1.8f;
    int inside = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        tone_shapes_cpp::vec2 u(-R + (x + 0.5f) * 2 * R / N, -R + (y + 0.5f) * 2 * R / N);
        if (tone_shapes_cpp::toneShapeDistance(shape, u) < 0) ++inside;
      }
    areas[shape] = std::max(0.05f, float(inside) * (2 * R / N) * (2 * R / N));
  }
  return areas[shape];
}
