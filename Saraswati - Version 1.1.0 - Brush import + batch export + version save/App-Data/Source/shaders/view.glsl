// Screen -> document sampling shared by the composite and cache passes.
// doc = pan + R(-theta) * (screenPx - centre) / zoom
layout(push_constant) uniform ViewPC {
  vec2 pan;
  vec2 centre;
  vec2 docSize;
  float zoom;
  float cosT;
  float sinT;
  int samples;        // 0 = nearest, -1 = bilinear, n > 0 = n x n box filter
  float layerOpacity;
  int flags;          // see FLAG_* below
  vec4 color;         // stroke colour (straight rgb) + stroke opacity
  vec4 paper;         // premultiplied paper colour
  int mode;           // blend mode of the sampled layer
  int antsPhase;      // marching-ants animation offset
  float flipX;        // -1 = view mirrored horizontally
  int adjType;        // live colour-adjustment preview on the active layer (FLAG_ADJ)
  vec4 adjP;
  int fx;             // this layer's record in the effect buffer, -1 = none
} pc;

const int FLAG_STROKE = 1;   // live stroke in the mask applies to the sampled layer
const int FLAG_ERASER = 2;
const int FLAG_LAYER  = 4;   // composite: an active, visible layer exists
const int FLAG_ABOVE_CACHE = 8;  // frame: all layers above are Normal, use the "above" cache
const int FLAG_ABOVE  = 16;  // cache: blend into the "above" cache
const int FLAG_INIT   = 32;  // cache: init pass (paper / transparent)
const int FLAG_WORK   = 64;  // cache: blend into this frame's work image
const int FLAG_ONLY_BELOW = 128;
const int FLAG_LOCK   = 512;  // live stroke on an alpha-locked layer
const int FLAG_ANTS   = 4096;  // frame: draw the marching ants
const int FLAG_GROUP_BLEND = 8192;   // cache: blend the finished folder image (binding 3) into binding 2
const int FLAG_ANTS_ONLY = 16384;    // frame: only draw the ants over the finished work image
const int FLAG_CLIP = 32768;         // cache: clipped layer - drawn atop, keeps the alpha below

const int FLAG_ADJ    = 2048; // frame: preview a colour adjustment on the active layer
const int FLAG_NOCLIP = 1024; // stroke is a preview overlay: ignore selection and lock
const int FLAG_SEL    = 256;  // a selection is active: live stroke clipped to it, marching ants drawn  // cache init: leave "above" alone (flatten for saving)

vec2 screenToDoc(vec2 s) {
  vec2 v = (s - pc.centre) / pc.zoom;
  v.x *= pc.flipX;
  return pc.pan + vec2(pc.cosT * v.x + pc.sinT * v.y, -pc.sinT * v.x + pc.cosT * v.y);
}

bool insideDoc(vec2 d) {
  return all(greaterThanEqual(d, vec2(0.0))) && all(lessThan(d, pc.docSize));
}

#include "adjust.glsl"
#include "tone_shapes.glsl"

// Per-layer effects (Layer Properties), one record per layer index. Non-destructive: applied to
// the layer colour while compositing, so the layer's pixels never change.
struct LayerFx {
  int flags;        // 1 tone, 2 layer colour, 4 tone reflects layer opacity, 8 tone in black
  int shape;
  int density;      // 0 from the brightness of the image, 1 from its opacity
  int levels;       // posterization levels, 0 = off
  float cell;       // dot period in document px
  float cosA, sinA;
  float offX, offY;
  float noiseSize, noiseFactor;
  float area;       // area of the unit dot shape
  float opacity;    // layer opacity (reflected in the dot size)
  float pad0, pad1, pad2;
  vec4 mainColor;
  vec4 subColor;    // a = 0: light parts become transparent
};
layout(std430, set = 0, binding = 6) readonly buffer FxBuf { LayerFx fx[]; };

float toneHash(vec2 g) { return fract(sin(dot(g, vec2(12.9898, 78.233))) * 43758.5453); }

// coverage of the tone dots at cell position q for density d (0..1), anti-aliased over one pixel
float toneDots(int shape, vec2 q, float d, float A, float cell) {
  if (d <= 0.0) return 0.0;
  if (d >= 1.0) return 1.0;
  if (shape == 3) {  // lines: thickness = density
    float y = abs(fract(q.y) - 0.5);
    return clamp((d * 0.5 - y) * cell + 0.5, 0.0, 1.0);
  }
  if (shape == 27) {  // wave lines
    float y = abs(fract(q.y + 0.22 * sin(q.x * 3.14159265)) - 0.5);
    return clamp((d * 0.5 - y) * cell + 0.5, 0.0, 1.0);
  }
  if (shape == 29) {  // random dots: each dot moved inside its cell
    vec2 id = floor(q);
    vec2 j = vec2(toneHash(id), toneHash(id + 17.3)) - 0.5;
    float s = sqrt(min(d, 0.5) / A);
    vec2 u = fract(q) - 0.5 - j * max(0.0, 1.0 - 2.0 * s) * 0.9;
    float c = clamp(0.5 - (length(u) - s) * cell, 0.0, 1.0);
    return d <= 0.5 ? c : mix(c, 1.0, (d - 0.5) * 2.0);
  }
  if (shape == 4) {  // cross: arm width r with 4r - 4r^2 = d
    vec2 u = abs(fract(q) - 0.5);
    float r = (1.0 - sqrt(max(0.0, 1.0 - d))) * 0.5;
    return clamp((r - min(u.x, u.y)) * cell + 0.5, 0.0, 1.0);
  }
  if (d <= 0.5) {  // growing dots
    vec2 u = fract(q) - 0.5;
    float s = sqrt(d / A);
    return clamp(0.5 - toneShapeDistance(shape, u / s) * s * cell, 0.0, 1.0);
  }
  // dark tones: shrinking holes of the same shape between the dots
  vec2 u = fract(q + 0.5) - 0.5;
  float s = sqrt((1.0 - d) / A);
  return 1.0 - clamp(0.5 - toneShapeDistance(shape, u / s) * s * cell, 0.0, 1.0);
}

vec4 applyLayerFx(vec4 c, ivec2 p, LayerFx f) {
  if ((f.flags & 2) != 0 && c.a > 0.0) {  // layer colour: dark -> main colour, light -> sub colour
    vec3 rgb = c.rgb / c.a;
    float l = clamp(dot(rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
    vec3 col = mix(f.mainColor.rgb, f.subColor.rgb, l);
    float a = c.a * mix(1.0, f.subColor.a, l);
    c = vec4(col * a, a);
  }
  if ((f.flags & 1) != 0) {  // manga tone
    if (c.a <= 0.0) return vec4(0.0);
    vec3 rgb = c.rgb / c.a;
    float d = f.density == 0 ? 1.0 - clamp(dot(rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0) : c.a;
    if ((f.flags & 4) != 0) d *= f.opacity;
    if (f.levels > 1) d = floor(d * float(f.levels - 1) + 0.5) / float(f.levels - 1);
    d = clamp(d, 0.0, 1.0);
    float cov;
    if (f.shape == 6) {  // noise
      float h = toneHash(floor((vec2(p) + 0.5) / max(f.noiseSize, 1.0)));
      cov = mix(0.5, h, f.noiseFactor) < d ? 1.0 : 0.0;
    } else if (f.shape == 28) {  // concentric circles around the dot position (X / Y)
      float r = length(vec2(p) + 0.5 - vec2(f.offX, f.offY)) / f.cell;
      float y = abs(fract(r) - 0.5);
      cov = clamp((d * 0.5 - y) * f.cell + 0.5, 0.0, 1.0);
    } else {
      vec2 q0 = vec2(p) + 0.5 - vec2(f.offX, f.offY);
      vec2 q = vec2(f.cosA * q0.x + f.sinA * q0.y, -f.sinA * q0.x + f.cosA * q0.y) / f.cell;
      cov = toneDots(f.shape, q, d, f.area, f.cell);
    }
    float a = cov * (f.density == 0 ? c.a : 1.0);
    vec3 col = (f.flags & 8) != 0 ? vec3(0.0) : rgb;
    c = vec4(col * a, a);
  }
  return c;
}

vec4 layerTexel(ivec2 p) {
  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, ivec2(pc.docSize)))) return vec4(0.0);
  vec4 c = imageLoad(layerImg, p);
#ifdef WITH_STROKE
  if ((pc.flags & FLAG_ADJ) != 0) {
    vec4 r = applyAdjust(c, pc.adjType, pc.adjP);
    c = (pc.flags & FLAG_SEL) != 0 ? mix(c, r, imageLoad(selImg, p).r) : r;
  }
  if ((pc.flags & FLAG_STROKE) != 0) {
    float a = imageLoad(maskImg, p).r * pc.color.a;
    if ((pc.flags & (FLAG_SEL | FLAG_NOCLIP)) == FLAG_SEL) a *= imageLoad(selImg, p).r;
    if ((pc.flags & (FLAG_LOCK | FLAG_NOCLIP)) == FLAG_LOCK) {
      if ((pc.flags & FLAG_ERASER) == 0 && c.a > 0.0) c = vec4(mix(c.rgb / c.a, pc.color.rgb, a) * c.a, c.a);
    } else if ((pc.flags & FLAG_ERASER) != 0) c *= (1.0 - a);
    else c = vec4(pc.color.rgb, 1.0) * a + c * (1.0 - a);
  }
#endif
  if (pc.fx >= 0 && fx[pc.fx].flags != 0) c = applyLayerFx(c, p, fx[pc.fx]);
  return c;
}

// Layer colour (premultiplied) seen by the screen pixel whose centre is s.
vec4 sampleLayer(vec2 s) {
  if (pc.samples == 0) {
    return layerTexel(ivec2(floor(screenToDoc(s))));
  } else if (pc.samples < 0) {
    vec2 t = screenToDoc(s) - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - vec2(i);
    vec4 a = mix(layerTexel(i), layerTexel(i + ivec2(1, 0)), f.x);
    vec4 b = mix(layerTexel(i + ivec2(0, 1)), layerTexel(i + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
  }
  int n = pc.samples;
  vec4 sum = vec4(0.0);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      vec2 o = (vec2(x, y) + 0.5) / float(n) - 0.5;
      sum += layerTexel(ivec2(floor(screenToDoc(s + o))));
    }
  return sum / float(n * n);
}

// ---------------------------------------------------------------------------
// Blend modes (Clip Studio Paint's list). b = backdrop, s = source, both premultiplied.
// Result follows the W3C compositing model:
//   co = cs*as*(1-ab) + cb*ab*(1-as) + as*ab*B(cb, cs),  ao = as + ab*(1-as)

float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clipColor(vec3 c) {
  float l = lum(c), n = min(c.r, min(c.g, c.b)), x = max(c.r, max(c.g, c.b));
  if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
  if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
  return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
vec3 setSat(vec3 c, float s) {
  float mn = min(c.r, min(c.g, c.b)), mx = max(c.r, max(c.g, c.b));
  return mx > mn ? (c - mn) * s / (mx - mn) : vec3(0.0);
}
vec3 colorBurn(vec3 b, vec3 s) {
  vec3 r = 1.0 - min(vec3(1.0), (1.0 - b) / max(s, 1e-6));
  return mix(mix(r, vec3(0.0), lessThanEqual(s, vec3(0.0))), vec3(1.0), greaterThanEqual(b, vec3(1.0)));
}
vec3 colorDodge(vec3 b, vec3 s) {
  vec3 r = min(vec3(1.0), b / max(1.0 - s, 1e-6));
  return mix(mix(r, vec3(1.0), greaterThanEqual(s, vec3(1.0))), vec3(0.0), lessThanEqual(b, vec3(0.0)));
}
vec3 screenB(vec3 b, vec3 s) { return b + s - b * s; }
vec3 hardLight(vec3 b, vec3 s) {
  return mix(2.0 * b * s, screenB(b, 2.0 * s - 1.0), greaterThan(s, vec3(0.5)));
}
vec3 softLight(vec3 b, vec3 s) {
  vec3 d = mix(sqrt(b), ((16.0 * b - 12.0) * b + 4.0) * b, lessThanEqual(b, vec3(0.25)));
  return mix(b + (2.0 * s - 1.0) * (d - b), b - (1.0 - 2.0 * s) * b * (1.0 - b), lessThanEqual(s, vec3(0.5)));
}

vec3 blendFn(int m, vec3 b, vec3 s) {
  switch (m) {
    case 1:  return min(b, s);                                    // Darken
    case 2:  return b * s;                                        // Multiply
    case 3:  return colorBurn(b, s);                              // Color burn
    case 4:  return max(b + s - 1.0, 0.0);                        // Linear burn
    case 5:  return max(b - s, 0.0);                              // Subtract
    case 6:  return max(b, s);                                    // Lighten
    case 7:  return screenB(b, s);                                // Screen
    case 8:  return colorDodge(b, s);                             // Color dodge
    case 10: return min(b + s, 1.0);                              // Add
    case 12: return hardLight(s, b);                              // Overlay
    case 13: return softLight(b, s);                              // Soft light
    case 14: return hardLight(b, s);                              // Hard light
    case 15: return abs(b - s);                                   // Difference
    case 16: return mix(colorBurn(b, 2.0 * s), colorDodge(b, 2.0 * s - 1.0), greaterThan(s, vec3(0.5)));  // Vivid light
    case 17: return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);           // Linear light
    case 18: return mix(min(b, 2.0 * s), max(b, 2.0 * s - 1.0), greaterThan(s, vec3(0.5)));              // Pin light
    case 19: return step(1.0, b + s);                             // Hard mix
    case 20: return b + s - 2.0 * b * s;                          // Exclusion
    case 21: return lum(s) < lum(b) ? s : b;                      // Darker color
    case 22: return lum(s) > lum(b) ? s : b;                      // Lighter color
    case 23: return mix(min(vec3(1.0), b / max(s, 1e-6)), mix(vec3(0.0), vec3(1.0), greaterThan(b, vec3(0.0))), lessThanEqual(s, vec3(0.0)));  // Divide
    case 24: return setLum(setSat(s, sat(b)), lum(b));            // Hue
    case 25: return setLum(setSat(b, sat(s)), lum(b));            // Saturation
    case 26: return setLum(s, lum(b));                            // Color
    case 27: return setLum(b, lum(s));                            // Brightness (luminosity)
  }
  return s;                                                       // Normal
}

vec4 blendLayer(vec4 b, vec4 s, int m) {
  if (m == 0 || b.a <= 0.0) return s + b * (1.0 - s.a);
  if (s.a <= 0.0) return b;
  vec3 cb = b.rgb / b.a;
  if (m == 9 || m == 11) {
    // Glow dodge / Add (glow): the effect uses the premultiplied source, so semi-transparent
    // pixels still brighten ("glow") the backdrop instead of fading out.
    vec3 e = (m == 9) ? colorDodge(cb, s.rgb) : min(cb + s.rgb, 1.0);
    return vec4(s.rgb * (1.0 - b.a) + b.a * e, s.a + b.a * (1.0 - s.a));
  }
  vec3 cs = s.rgb / s.a;
  vec3 co = s.rgb * (1.0 - b.a) + b.rgb * (1.0 - s.a) + s.a * b.a * clamp(blendFn(m, cb, cs), 0.0, 1.0);
  return vec4(co, s.a + b.a * (1.0 - s.a));
}

// "Source atop" with a blend mode: the result keeps the alpha of what is below (clipping masks).
vec4 blendClipped(vec4 d, vec4 s, int mode) {
  if (mode == 0) return s * d.a + d * (1.0 - s.a);  // exact for Normal
  vec4 r = blendLayer(d, s * d.a, mode);
  r.a = d.a;
  r.rgb = min(r.rgb, vec3(r.a));
  return r;
}
