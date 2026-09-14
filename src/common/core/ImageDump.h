#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace sidecar {

// Writes an 8-bit BGR image as a Windows BMP. The dullest possible format:
// no dependencies, and every image viewer on the planet opens it. `bgr` is
// row-major, top row first, three bytes per pixel.
bool WriteBmp(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& bgr);

// IEEE half to float. RGBA16F readbacks arrive as halves.
float HalfToFloat(uint16_t half);

}  // namespace sidecar
