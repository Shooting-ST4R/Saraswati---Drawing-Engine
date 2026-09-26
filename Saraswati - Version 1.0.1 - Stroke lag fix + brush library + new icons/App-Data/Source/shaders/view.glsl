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
const int FLAG_NOCLIP = 1024; // stroke is a preview overlay: ignore selection and lock
const int FLAG_SEL    = 256;  // a selection is active: live stroke clipped to it, marching ants drawn  // cache init: leave "above" alone (flatten for saving)

vec2 screenToDoc(vec2 s) {
  vec2 v = (s - pc.centre) / pc.zoom;
  return pc.pan + vec2(pc.cosT * v.x + pc.sinT * v.y, -pc.sinT * v.x + pc.cosT * v.y);
}

bool insideDoc(vec2 d) {
  return all(greaterThanEqual(d, vec2(0.0))) && all(lessThan(d, pc.docSize));
}

vec4 layerTexel(ivec2 p) {
  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, ivec2(pc.docSize)))) return vec4(0.0);
  vec4 c = imageLoad(layerImg, p);
#ifdef WITH_STROKE
  if ((pc.flags & FLAG_STROKE) != 0) {
    float a = imageLoad(maskImg, p).r * pc.color.a;
    if ((pc.flags & (FLAG_SEL | FLAG_NOCLIP)) == FLAG_SEL) a *= imageLoad(selImg, p).r;
    if ((pc.flags & (FLAG_LOCK | FLAG_NOCLIP)) == FLAG_LOCK) {
      if ((pc.flags & FLAG_ERASER) == 0 && c.a > 0.0) c = vec4(mix(c.rgb / c.a, pc.color.rgb, a) * c.a, c.a);
    } else if ((pc.flags & FLAG_ERASER) != 0) c *= (1.0 - a);
    else c = vec4(pc.color.rgb, 1.0) * a + c * (1.0 - a);
  }
#endif
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
