// Manga speech bubbles around text: shape + tails as one outline, fill and border, rendered on the
// CPU as a signed distance field (anti-aliased, exact border width).
#pragma once
#include <cstdint>
#include <vector>

struct BubbleStyle {
  bool enabled = false;
  int shape = 0;              // 0 ellipse, 1 rounded rectangle, 2 rectangle, 3 cloud (thought), 4 burst (shout)
  float padX = 30, padY = 24; // distance from the text
  float corner = 30;          // rounded rectangle radius
  int bumps = 14;             // cloud bumps / burst spikes
  float amount = 0.18f;       // cloud bump / burst spike depth (fraction of the size)
  float fill[3] = {1, 1, 1};
  float fillOpacity = 1.0f;
  float border[3] = {0, 0, 0};
  float borderWidth = 4;
  float borderOpacity = 1.0f;
  float tailWidth = 40;       // tail width where it leaves the bubble
  int tailMode = 0;           // 0 freehand (follows the pen), 1 straight
  bool operator==(const BubbleStyle&) const = default;
};

struct BubblePt { double x, y; };
using BubbleTail = std::vector<BubblePt>;  // centre line, document px, from the bubble outwards

struct BubbleGeom {
  double cx = 0, cy = 0, hx = 0, hy = 0;  // shape centre and half size
};

struct BubbleImage {
  int x0 = 0, y0 = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight alpha
  BubbleGeom geom;
};

// text box in document px -> bubble geometry for the style
BubbleGeom bubbleGeometry(const BubbleStyle& st, double tx0, double ty0, double tx1, double ty1);
// signed distance to the bubble shape without tails (negative inside)
double bubbleShapeDistance(const BubbleStyle& st, const BubbleGeom& g, double x, double y);
void renderBubble(const BubbleStyle& st, const BubbleGeom& g, const std::vector<BubbleTail>& tails, BubbleImage& out);
