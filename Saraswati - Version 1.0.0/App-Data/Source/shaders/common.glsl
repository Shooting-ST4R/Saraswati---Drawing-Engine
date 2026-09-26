// Shared declarations for every Saraswati shader.
// Set 0 (per frame slot): stroke mask, dab ring buffer, screen caches.
// Set 1 (per layer): the layer image. All images stay in VK_IMAGE_LAYOUT_GENERAL
// and are read with imageLoad (texelFetch-style, no sampler/filter features needed).

// Fragment shaders define IMG_QUAL readonly (no fragmentStoresAndAtomics needed).
#ifndef IMG_QUAL
#define IMG_QUAL
#endif

#ifdef MASK_R32F
layout(set = 0, binding = 0, r32f) IMG_QUAL uniform image2D maskImg;
#else
layout(set = 0, binding = 0, r16) IMG_QUAL uniform image2D maskImg;
#endif

struct Dab { vec2 pos; float radius; float alpha; };
layout(std430, set = 0, binding = 1) readonly buffer DabBuf { Dab dabs[]; };

layout(set = 0, binding = 2, rgba8) IMG_QUAL uniform image2D belowImg;
layout(set = 0, binding = 3, rgba8) IMG_QUAL uniform image2D aboveImg;

layout(set = 1, binding = 0, rgba8) IMG_QUAL uniform image2D layerImg;
layout(set = 0, binding = 4, rgba8) IMG_QUAL uniform image2D workImg;   // this frame's composite
// Selection coverage (document size). Only read when a selection is active.
#ifdef MASK_R32F
layout(set = 0, binding = 5, r32f) IMG_QUAL uniform image2D selImg;
#else
layout(set = 0, binding = 5, r8) IMG_QUAL uniform image2D selImg;
#endif
