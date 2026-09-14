#include "core/ImageDump.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace sidecar {

bool WriteBmp(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& bgr) {
  if (width == 0 || height == 0 || bgr.size() < static_cast<size_t>(width) * height * 3) {
    return false;
  }
  const uint32_t rowBytes = width * 3;
  const uint32_t padded = (rowBytes + 3) & ~3u;
  const uint32_t imageBytes = padded * height;
  const uint32_t fileBytes = 14 + 40 + imageBytes;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };

  // BITMAPFILEHEADER
  out.write("BM", 2);
  put32(fileBytes);
  put16(0);
  put16(0);
  put32(14 + 40);
  // BITMAPINFOHEADER. Negative height means top-down, which is the order the
  // caller supplies rows in.
  put32(40);
  put32(width);
  put32(static_cast<uint32_t>(-static_cast<int32_t>(height)));
  put16(1);
  put16(24);
  put32(0);
  put32(imageBytes);
  put32(2835);
  put32(2835);
  put32(0);
  put32(0);

  const char pad[3] = {0, 0, 0};
  for (uint32_t y = 0; y < height; ++y) {
    out.write(reinterpret_cast<const char*>(bgr.data() + static_cast<size_t>(y) * rowBytes),
              rowBytes);
    out.write(pad, padded - rowBytes);
  }
  return out.good();
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign << 31;
    } else {
      // Subnormal: normalise.
      uint32_t e = 127 - 15 + 1;
      uint32_t m = mant;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --e;
      }
      m &= 0x3FFu;
      bits = (sign << 31) | (e << 23) | (m << 13);
    }
  } else if (exp == 31) {
    bits = (sign << 31) | 0x7F800000u | (mant << 13);
  } else {
    bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

}  // namespace sidecar
