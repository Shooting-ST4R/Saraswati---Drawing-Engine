#include "png_write.h"
#include <cstdio>
#include <vector>

static uint32_t crcTable[256];
static void initCrc() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
  for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> buf;
  be32(buf, uint32_t(data.size()));
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  be32(buf, crc(buf.data() + 4, buf.size() - 4) ^ 0xffffffffu);
  fwrite(buf.data(), 1, buf.size(), f);
}

bool writePngRgb(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgb) {
  initCrc();
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  be32(ihdr, w); be32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(f, "IHDR", ihdr);
  // raw scanlines (filter byte 0 + RGB)
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (w * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
  }
  // zlib stream with stored blocks
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  size_t pos = 0;
  do {
    size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  be32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  return fclose(f) == 0;
}
