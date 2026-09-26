// Manga tone dot shapes (Layer Properties > Tone). Each shape is a signed distance in "unit"
// coordinates: the dot is about radius 1 around (0, 0), negative inside, y pointing down.
// This file is compiled as GLSL (shaders) AND as C++ (tone.cpp measures each shape's area so the
// dot size matches the requested density), so it only uses a small common subset of both.

float tsFract(float v) { return v - floor(v); }
float tsLen(vec2 v) { return sqrt(v.x * v.x + v.y * v.y); }
float tsSeg(vec2 p, vec2 a, vec2 b, float r) {  // capsule
  vec2 pa = vec2(p.x - a.x, p.y - a.y), ba = vec2(b.x - a.x, b.y - a.y);
  float h = clamp((pa.x * ba.x + pa.y * ba.y) / (ba.x * ba.x + ba.y * ba.y), 0.0, 1.0);
  return tsLen(vec2(pa.x - ba.x * h, pa.y - ba.y * h)) - r;
}
float tsCircle(vec2 p, vec2 c, float r) { return tsLen(vec2(p.x - c.x, p.y - c.y)) - r; }
// polar shape: radius R(angle) -> approximate distance
float tsPolar(vec2 p, float r) { return tsLen(p) - r; }
float tsAngle(vec2 p) { return atan(p.y, p.x); }  // -pi..pi

float tsHeart(vec2 p) {  // Inigo Quilez, tip at (0,0), lobes towards +y, height ~1.1
  p.x = abs(p.x);
  if (p.y + p.x > 1.0) return tsLen(vec2(p.x - 0.25, p.y - 0.75)) - 0.35355339;
  float k = 0.5 * max(p.x + p.y, 0.0);
  float a = (p.x) * (p.x) + (p.y - 1.0) * (p.y - 1.0);
  float b = (p.x - k) * (p.x - k) + (p.y - k) * (p.y - k);
  float s = p.x - p.y > 0.0 ? 1.0 : -1.0;
  return sqrt(min(a, b)) * s;
}

float toneShapeDistance(int shape, vec2 u) {
  const float PI = 3.14159265;
  float ang = tsAngle(u);
  float len = tsLen(u);
  if (shape == 0) return len - 1.0;                                            // circle
  if (shape == 1) return max(abs(u.x), abs(u.y)) - 1.0;                         // square
  if (shape == 2) return (abs(u.x) + abs(u.y) - 1.0) * 0.70710678;             // lozenge
  if (shape == 5) return (tsLen(vec2(u.x / 1.3, u.y / 0.75)) - 1.0) * 0.75;    // ellipse
  if (shape == 7) return tsPolar(u, 0.86 + 0.14 * cos(ang * 10.0));             // sugar plum (konpeito)
  if (shape == 8) {                                                            // asterisk
    float d = 1e9;
    for (int i = 0; i < 3; ++i) {
      float a = float(i) * PI / 3.0 + PI / 2.0;
      d = min(d, tsSeg(u, vec2(-cos(a), -sin(a)), vec2(cos(a), sin(a)), 0.2));
    }
    return d;
  }
  if (shape == 9 || shape == 19) {                                             // star / ninja star
    float n = shape == 9 ? 5.0 : 4.0;
    float t = tsFract((ang + PI / 2.0) / (2.0 * PI) * n);
    if (shape == 19) t = pow(t, 0.55);                                         // pinwheel blades
    float r = 1.0 + (0.42 - 1.0) * (1.0 - abs(t * 2.0 - 1.0));
    if (shape == 19) r = 1.0 + (0.3 - 1.0) * (1.0 - abs(t * 2.0 - 1.0));
    return (len - r) * 0.6;
  }
  if (shape == 10) {                                                           // carrot (drop, point down)
    float c = cos(ang - PI / 2.0);                                             // 1 = straight down
    return (len - (0.62 + 0.55 * pow(max(c, 0.0), 8.0))) * 0.8;
  }
  if (shape >= 11 && shape <= 13) {                                            // cherry blossom
    float a = shape == 11 ? 0.55 : shape == 12 ? 0.35 : 0.15;
    float notch = shape == 11 ? 0.12 : shape == 12 ? 0.18 : 0.24;
    float th = ang + PI / 2.0;
    float r = a + (1.0 - a) * pow(abs(cos(th * 2.5)), 0.8);
    float k = tsFract(th / (2.0 * PI / 5.0) + 0.5) - 0.5;                     // offset from the nearest petal tip
    r *= 1.0 - notch * exp(-k * k * 180.0);
    return (len - r) * 0.7;
  }
  if (shape >= 14 && shape <= 16) {                                            // flower (6 petals)
    float a = shape == 14 ? 0.6 : shape == 15 ? 0.4 : 0.2;
    float e = shape == 16 ? 1.4 : 0.6;
    return (len - (a + (1.0 - a) * pow(abs(cos(ang * 3.0)), e))) * 0.7;
  }
  if (shape == 17 || shape == 18) {                                            // clover (4 lobes)
    float a = shape == 17 ? 0.35 : 0.12;
    float e = shape == 17 ? 0.5 : 1.3;
    return (len - (a + (1.0 - a) * pow(abs(cos(ang * 2.0 + PI / 4.0)), e))) * 0.7;
  }
  if (shape == 20) return (abs(u.x) * 1.45 + abs(u.y) - 1.0) * 0.56;           // diamond (card suit)
  if (shape == 21) return tsHeart(vec2(u.x * 0.6, -u.y * 0.6 + 0.55)) / 0.6;   // heart
  if (shape == 22) {                                                           // clubs
    float d = min(tsCircle(u, vec2(0.0, -0.45), 0.42), min(tsCircle(u, vec2(-0.45, 0.12), 0.42), tsCircle(u, vec2(0.45, 0.12), 0.42)));
    d = min(d, tsCircle(u, vec2(0.0, 0.0), 0.3));
    return min(d, tsSeg(u, vec2(0.0, 0.1), vec2(0.0, 0.95), 0.12));
  }
  if (shape == 23) {                                                           // spades
    float d = tsHeart(vec2(u.x * 0.62, u.y * 0.62 + 0.62)) / 0.62;
    return min(d, tsSeg(u, vec2(0.0, 0.2), vec2(0.0, 0.98), 0.12));
  }
  if (shape == 24) {                                                           // oval (chain dot): ellipse turned 45 deg
    vec2 r = vec2((u.x + u.y) * 0.70710678, (u.y - u.x) * 0.70710678);
    return (tsLen(vec2(r.x / 1.25, r.y / 0.8)) - 1.0) * 0.8;
  }
  if (shape == 25) {                                                           // triangle (point up)
    const float k = 1.7320508;
    vec2 p = vec2(abs(u.x) - 1.0, -u.y * 1.1 + 1.0 / k);
    if (p.x + k * p.y > 0.0) p = vec2((p.x - k * p.y) * 0.5, (-k * p.x - p.y) * 0.5);
    p.x -= clamp(p.x, -2.0, 0.0);
    return -tsLen(p) * (p.y > 0.0 ? 1.0 : -1.0) * 0.9;
  }
  if (shape == 26) {                                                           // hexagon
    vec2 p = vec2(abs(u.x), abs(u.y));
    float d1 = p.x * 0.8660254 + p.y * 0.5;
    return (max(d1, p.y) - 0.92) * 0.9;
  }
  return len - 1.0;
}
