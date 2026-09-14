#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "core/UiCalibration.h"

using namespace sidecar;

namespace {

// A flat grey frame, with a block painted white between two corners.
std::vector<uint8_t> Frame(uint32_t w, uint32_t h) {
  return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 96);
}

void Paint(std::vector<uint8_t>& img, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1,
           uint32_t y1, uint8_t value) {
  for (uint32_t y = y0; y < y1; ++y) {
    for (uint32_t x = x0; x < x1; ++x) {
      uint8_t* p = img.data() + (static_cast<size_t>(y) * w + x) * 3;
      p[0] = p[1] = p[2] = value;
    }
  }
}

bool Covers(const std::vector<UiRect>& rects, int32_t x, int32_t y) {
  for (const auto& r : rects) {
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("a block that appears only with the UI becomes one rectangle", "[unit]") {
  const uint32_t w = 512, h = 256;
  auto with = Frame(w, h);
  auto without = Frame(w, h);
  // An "action bar": 256x64 at (128, 160).
  Paint(with, w, 128, 160, 384, 224, 240);

  UiCalibrationParams params;
  params.cell = 32;
  params.dilateCells = 1;
  const auto rects = RectsFromDiff(with, without, w, h, params);

  REQUIRE(rects.size() == 1);
  // Covers the bar, plus one cell of dilation on every side that fits.
  REQUIRE(Covers(rects, 128, 160));
  REQUIRE(Covers(rects, 383, 223));
  REQUIRE(Covers(rects, 100, 140));      // dilated
  REQUIRE_FALSE(Covers(rects, 60, 100)); // well outside
  REQUIRE(rects[0].bottom <= static_cast<int32_t>(h));
}

TEST_CASE("scattered world motion is not mistaken for interface", "[unit]") {
  const uint32_t w = 512, h = 256;
  auto with = Frame(w, h);
  auto without = Frame(w, h);
  // A few flickering pixels here and there -- a torch, a bird.
  Paint(with, w, 10, 10, 14, 14, 255);
  Paint(with, w, 300, 40, 306, 46, 255);
  Paint(with, w, 450, 200, 452, 210, 255);

  const auto rects = RectsFromDiff(with, without, w, h, UiCalibrationParams{});
  REQUIRE(rects.empty());
}

TEST_CASE("an isolated single differing cell is dropped as noise", "[unit]") {
  const uint32_t w = 512, h = 256;
  auto with = Frame(w, h);
  auto without = Frame(w, h);
  // One whole cell's worth of change, alone.
  Paint(with, w, 256, 128, 288, 160, 255);

  UiCalibrationParams params;
  params.minComponentCells = 3;
  REQUIRE(RectsFromDiff(with, without, w, h, params).empty());

  // But a component of three cells survives.
  Paint(with, w, 288, 128, 352, 160, 255);
  REQUIRE(RectsFromDiff(with, without, w, h, params).size() == 1);
}

TEST_CASE("two separate interface elements become two rectangles", "[unit]") {
  const uint32_t w = 512, h = 256;
  auto with = Frame(w, h);
  auto without = Frame(w, h);
  Paint(with, w, 0, 0, 128, 64, 255);        // top-left frame
  Paint(with, w, 384, 192, 512, 256, 255);   // bottom-right frame

  const auto rects = RectsFromDiff(with, without, w, h, UiCalibrationParams{});
  REQUIRE(rects.size() == 2);
  REQUIRE(Covers(rects, 5, 5));
  REQUIRE(Covers(rects, 500, 250));
  REQUIRE_FALSE(Covers(rects, 256, 128));
}

TEST_CASE("the diff mask marks exactly the differing pixels", "[unit]") {
  const uint32_t w = 64, h = 64;
  auto with = Frame(w, h);
  auto without = Frame(w, h);
  Paint(with, w, 16, 16, 32, 32, 200);

  std::vector<uint8_t> mask;
  RectsFromDiff(with, without, w, h, UiCalibrationParams{}, &mask);
  REQUIRE(mask.size() == static_cast<size_t>(w) * h);
  REQUIRE(mask[16 * w + 16] == 255);
  REQUIRE(mask[31 * w + 31] == 255);
  REQUIRE(mask[0] == 0);
  REQUIRE(mask[40 * w + 40] == 0);
}
