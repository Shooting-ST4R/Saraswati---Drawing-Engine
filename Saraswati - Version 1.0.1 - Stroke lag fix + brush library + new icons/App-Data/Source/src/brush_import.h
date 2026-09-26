// Importing brushes from Photoshop (.abr) and Clip Studio Paint (.sut).
#pragma once
#include "brush.h"
#include <cstdint>
#include <string>
#include <vector>

struct ImportedImage {  // a sampled tip or a pattern texture (alpha: 255 = paint)
  std::string name;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> alpha;
};

struct TipRef {  // which images a brush uses (indexes into BrushImport::images)
  size_t brush;
  std::vector<size_t> tips;
  size_t texture, dual;  // SIZE_MAX = none
};

struct BrushImport {
  std::vector<BrushSettings> brushes;  // brush.notes lists what could not be reproduced
  std::vector<ImportedImage> images;
  std::vector<TipRef> tipRefs;
  std::vector<std::string> fileNotes;
};

bool importAbr(const std::string& path, BrushImport& out, std::string& err);
bool importSut(const std::string& path, BrushImport& out, std::string& err);
