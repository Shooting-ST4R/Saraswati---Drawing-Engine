// File formats: PSD/PSB (open + save, layered), and image import (PNG, JPEG, GIF, BMP, TGA, WebP, SVG).
// All pixel buffers here are 8-bit RGBA with straight (non-premultiplied) alpha.
#pragma once
#include "renderer.h"
#include <cstdint>
#include <string>
#include <vector>

struct ImageRGBA {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

struct DocLayer {
  std::string name;
  int32_t x = 0, y = 0;  // position of the pixel rect in the document
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
  bool visible = true;
  float opacity = 1.0f;
  BlendMode mode = BlendMode::Normal;
};

struct DocFile {
  uint32_t w = 0, h = 0;
  std::vector<DocLayer> layers;  // bottom to top
  std::vector<std::string> warnings;
};

bool isPsdPath(const std::string& path);
bool loadImageFile(const std::string& path, ImageRGBA& out, std::string& err);
bool loadPsd(const std::string& path, DocFile& out, std::string& err);
// Layers must be document-size (x = y = 0, w/h = document). Written as PSD, or PSB when the
// document is larger than 30000 px or the data would overflow PSD's 32-bit lengths.
// `progress` (0..1) is updated from the writing thread.
bool savePsd(const std::string& path, const DocFile& doc, const ImageRGBA& merged, std::string& err,
             float* progress = nullptr);

// Flattened PNG (with alpha) or JPEG (over white), chosen by the file extension.
bool exportImage(const std::string& path, const ImageRGBA& img, std::string& err);

void premultiply(std::vector<uint8_t>& rgba);
void unpremultiply(std::vector<uint8_t>& rgba);
