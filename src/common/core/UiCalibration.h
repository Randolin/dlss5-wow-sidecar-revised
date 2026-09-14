#pragma once
#include <cstdint>
#include <vector>

#include "core/Config.h"

namespace sidecar {

// Turns two captures of the same view -- one with the game's interface drawn,
// one with it hidden (Alt+Z in WoW) -- into the rectangles the neural pass
// must leave alone.
//
// Pure, so it can be tested against synthetic images. The world moves a
// little between the two captures (idle animations, flames, clouds), so the
// comparison works on a coarse cell grid with a fraction threshold, drops
// small isolated components, and then dilates by a cell so text edges are
// safely inside the mask.
struct UiCalibrationParams {
  uint32_t cell = 32;            // grid cell size in pixels
  int pixelThreshold = 12;       // per-channel difference that counts, of 255
  float cellFraction = 0.20f;    // fraction of a cell's pixels that must differ
  uint32_t minComponentCells = 3;   // smaller connected groups are noise
  uint32_t dilateCells = 1;      // grown by this many cells on every side
  size_t maxRects = 256;
};

// `withUi` and `withoutUi` are row-major BGR, three bytes per pixel, the same
// size. `diffMask`, if given, receives one byte per pixel: 255 where the pixel
// differed, so the operator can look at what was detected.
std::vector<UiRect> RectsFromDiff(const std::vector<uint8_t>& withUi,
                                  const std::vector<uint8_t>& withoutUi,
                                  uint32_t width, uint32_t height,
                                  const UiCalibrationParams& params,
                                  std::vector<uint8_t>* diffMask = nullptr);

}  // namespace sidecar
