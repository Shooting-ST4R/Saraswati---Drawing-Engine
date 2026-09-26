#pragma once
#include <cstdint>
#include <string>
// Writes an 8-bit RGB PNG (stored/uncompressed deflate; test helper, not for documents).
bool writePngRgb(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgb);
