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
  int section = 0;          // 0 layer, 1 folder (open), 2 folder (closed), 3 end of a folder's contents
  bool passThrough = false; // folder blend "Through" (PSD 'pass')
  bool clip = false;        // clipped to the layer below
  std::string meta;         // Saraswati-only layer data (effects, editable text, ...), private PSD block
};

struct DocFile {
  uint32_t w = 0, h = 0;
  std::vector<DocLayer> layers;  // bottom to top
  std::vector<std::string> warnings;
  // paper (a colour under all layers, not pixels); stored in a private PSD resource
  bool hasPaper = false, paperVisible = true;
  uint8_t paper[3] = {255, 255, 255};
  float dpi = 350;
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

// Safe saving: write to a temporary file (flushed to disk), read it back and compare, then replace.
struct PsdCheck { int32_t x = 0, y = 0; uint32_t w = 0, h = 0; uint64_t hash = 0; bool visible = true; uint8_t opacity = 255; BlendMode mode = BlendMode::Normal; int section = 0; bool passThrough = false; bool clip = false; std::string meta; };
bool writePsdFile(const std::string& tmp, const DocFile& doc, const ImageRGBA& merged, std::string& err, float* progress = nullptr);
std::vector<PsdCheck> psdChecks(const DocFile& doc);
bool verifyPsd(const std::string& file, uint32_t w, uint32_t h, const std::vector<PsdCheck>& expect, std::string& err);
bool commitFileReplace(const std::string& tmp, const std::string& path, std::string& err);

// In-memory PNG (system clipboard): encode straight RGBA, decode any stb-supported image.
bool encodePngMemory(const ImageRGBA& img, std::vector<uint8_t>& out);
bool decodeImageMemory(const void* data, size_t size, ImageRGBA& out, std::string& err);
void premultiply(std::vector<uint8_t>& rgba);
void unpremultiply(std::vector<uint8_t>& rgba);
