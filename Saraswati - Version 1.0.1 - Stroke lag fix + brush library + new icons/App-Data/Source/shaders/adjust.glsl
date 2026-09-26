// Per-pixel colour adjustments (Edit > Tonal correction etc.). Shared by the live preview in the
// frame composite and by adjust.comp, which applies them to the layer - so the result is exactly
// what was previewed. c is premultiplied RGBA.
const int ADJ_BRIGHTNESS_CONTRAST = 1;  // p.x brightness -1..1, p.y contrast -1..1
const int ADJ_HSL = 2;                  // p.x hue shift (turns), p.y saturation -1..1, p.z luminosity -1..1
const int ADJ_INVERT = 3;
const int ADJ_POSTERIZE = 4;            // p.x levels
const int ADJ_THRESHOLD = 5;            // p.x threshold 0..1
const int ADJ_BRIGHTNESS_TO_ALPHA = 6;  // p.xyz colour of the result
const int ADJ_TO_COLOR = 7;             // p.xyz new colour, alpha kept

vec3 adjRgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 adjHsv2rgb(vec3 c) {
  vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
  return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

vec4 applyAdjust(vec4 c, int type, vec4 p) {
  float a = c.a;
  if (a <= 0.0) return c;
  vec3 rgb = c.rgb / a;
  float l = dot(rgb, vec3(0.299, 0.587, 0.114));
  if (type == ADJ_BRIGHTNESS_CONTRAST) {
    rgb += p.x;
    float k = p.y >= 0.0 ? 1.0 / max(1e-3, 1.0 - p.y) : 1.0 + p.y;
    rgb = (rgb - 0.5) * k + 0.5;
  } else if (type == ADJ_HSL) {
    vec3 h = adjRgb2hsv(clamp(rgb, 0.0, 1.0));
    h.x = fract(h.x + p.x);
    h.y = clamp(h.y * (1.0 + p.y), 0.0, 1.0);
    rgb = adjHsv2rgb(h);
    rgb = p.z >= 0.0 ? mix(rgb, vec3(1.0), p.z) : rgb * (1.0 + p.z);
  } else if (type == ADJ_INVERT) {
    rgb = 1.0 - rgb;
  } else if (type == ADJ_POSTERIZE) {
    float n = max(p.x, 2.0);
    rgb = floor(clamp(rgb, 0.0, 1.0) * (n - 1e-3)) / (n - 1.0);
  } else if (type == ADJ_THRESHOLD) {
    rgb = vec3(l >= p.x ? 1.0 : 0.0);
  } else if (type == ADJ_BRIGHTNESS_TO_ALPHA) {
    a *= clamp(1.0 - l, 0.0, 1.0);
    rgb = p.xyz;
  } else if (type == ADJ_TO_COLOR) {
    rgb = p.xyz;
  }
  rgb = clamp(rgb, 0.0, 1.0);
  return vec4(rgb * a, a);
}
