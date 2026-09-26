#version 450
#extension GL_GOOGLE_include_directive : require
#define IMG_QUAL readonly
#include "common.glsl"
#include "view.glsl"

layout(location = 0) out vec4 outColor;

// Work image over a checkerboard (inside the document) or workspace grey (outside).
void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  vec4 c = imageLoad(workImg, px);
  vec3 bg;
  if (insideDoc(screenToDoc(gl_FragCoord.xy))) {
    bool odd = ((px.x >> 3) + (px.y >> 3)) % 2 == 1;
    bg = odd ? vec3(0.80) : vec3(1.0);
  } else {
    bg = vec3(0.20);  // neutral workspace grey (no colour cast around the canvas)
  }
  outColor = vec4(c.rgb + bg * (1.0 - c.a), 1.0);
}
