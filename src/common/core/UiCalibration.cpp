#include "core/UiCalibration.h"

#include <algorithm>
#include <cstdlib>

namespace sidecar {

std::vector<UiRect> RectsFromDiff(const std::vector<uint8_t>& withUi,
                                  const std::vector<uint8_t>& withoutUi,
                                  uint32_t width, uint32_t height,
                                  const UiCalibrationParams& params,
                                  std::vector<uint8_t>* diffMask) {
  std::vector<UiRect> rects;
  const size_t pixels = static_cast<size_t>(width) * height;
  if (width == 0 || height == 0 || params.cell == 0 || withUi.size() < pixels * 3 ||
      withoutUi.size() < pixels * 3) {
    return rects;
  }

  const uint32_t cell = params.cell;
  const uint32_t cw = (width + cell - 1) / cell;
  const uint32_t ch = (height + cell - 1) / cell;

  // 1. Per-pixel difference, counted per cell.
  std::vector<uint32_t> counts(static_cast<size_t>(cw) * ch, 0);
  if (diffMask) diffMask->assign(pixels, 0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t p = (static_cast<size_t>(y) * width + x) * 3;
      int d = 0;
      for (int c = 0; c < 3; ++c) {
        d = std::max(d, std::abs(static_cast<int>(withUi[p + c]) -
                                 static_cast<int>(withoutUi[p + c])));
      }
      if (d > params.pixelThreshold) {
        ++counts[static_cast<size_t>(y / cell) * cw + x / cell];
        if (diffMask) (*diffMask)[static_cast<size_t>(y) * width + x] = 255;
      }
    }
  }

  // 2. A cell counts when enough of it differed. Edge cells are smaller, so
  //    the fraction is against the cell's real pixel count.
  std::vector<uint8_t> marked(counts.size(), 0);
  for (uint32_t cy = 0; cy < ch; ++cy) {
    for (uint32_t cx = 0; cx < cw; ++cx) {
      const uint32_t w = std::min(cell, width - cx * cell);
      const uint32_t h = std::min(cell, height - cy * cell);
      const float fraction = static_cast<float>(counts[static_cast<size_t>(cy) * cw + cx]) /
                             static_cast<float>(w * h);
      marked[static_cast<size_t>(cy) * cw + cx] = fraction >= params.cellFraction ? 1 : 0;
    }
  }

  // 3. Drop small connected components: a flickering torch or a bird is a
  //    cell or two; an action bar is a row of them.
  {
    std::vector<int32_t> label(marked.size(), -1);
    std::vector<uint32_t> stack;
    int32_t next = 0;
    std::vector<uint32_t> sizes;
    for (size_t i = 0; i < marked.size(); ++i) {
      if (!marked[i] || label[i] >= 0) continue;
      const int32_t id = next++;
      uint32_t size = 0;
      stack.clear();
      stack.push_back(static_cast<uint32_t>(i));
      label[i] = id;
      while (!stack.empty()) {
        const uint32_t at = stack.back();
        stack.pop_back();
        ++size;
        const uint32_t x = at % cw;
        const uint32_t y = at / cw;
        const int32_t dx[4] = {1, -1, 0, 0};
        const int32_t dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          const int32_t nx = static_cast<int32_t>(x) + dx[k];
          const int32_t ny = static_cast<int32_t>(y) + dy[k];
          if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(cw) || ny >= static_cast<int32_t>(ch)) {
            continue;
          }
          const size_t n = static_cast<size_t>(ny) * cw + static_cast<size_t>(nx);
          if (marked[n] && label[n] < 0) {
            label[n] = id;
            stack.push_back(static_cast<uint32_t>(n));
          }
        }
      }
      sizes.push_back(size);
    }
    for (size_t i = 0; i < marked.size(); ++i) {
      if (marked[i] && sizes[static_cast<size_t>(label[i])] < params.minComponentCells) {
        marked[i] = 0;
      }
    }
  }

  // 4. Dilate, so the ramp of the blend's feather sits outside the text.
  if (params.dilateCells > 0) {
    std::vector<uint8_t> grown(marked.size(), 0);
    const int32_t r = static_cast<int32_t>(params.dilateCells);
    for (uint32_t cy = 0; cy < ch; ++cy) {
      for (uint32_t cx = 0; cx < cw; ++cx) {
        if (!marked[static_cast<size_t>(cy) * cw + cx]) continue;
        for (int32_t dy = -r; dy <= r; ++dy) {
          for (int32_t dx = -r; dx <= r; ++dx) {
            const int32_t nx = static_cast<int32_t>(cx) + dx;
            const int32_t ny = static_cast<int32_t>(cy) + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(cw) ||
                ny >= static_cast<int32_t>(ch)) {
              continue;
            }
            grown[static_cast<size_t>(ny) * cw + static_cast<size_t>(nx)] = 1;
          }
        }
      }
    }
    marked.swap(grown);
  }

  // 5. Rectangles: each maximal horizontal run of cells, merged with the run
  //    directly above it when they share the same columns. Simple, and it
  //    yields one rectangle per action bar, unit frame, or chat box.
  struct Run {
    uint32_t x0, x1, y0, y1;   // cells, half-open
  };
  std::vector<Run> runs;
  std::vector<size_t> previousRow;   // indices into runs for the row above
  for (uint32_t cy = 0; cy < ch; ++cy) {
    std::vector<size_t> thisRow;
    uint32_t cx = 0;
    while (cx < cw) {
      if (!marked[static_cast<size_t>(cy) * cw + cx]) {
        ++cx;
        continue;
      }
      const uint32_t x0 = cx;
      while (cx < cw && marked[static_cast<size_t>(cy) * cw + cx]) ++cx;
      const uint32_t x1 = cx;
      bool merged = false;
      for (const size_t above : previousRow) {
        if (runs[above].x0 == x0 && runs[above].x1 == x1 && runs[above].y1 == cy) {
          runs[above].y1 = cy + 1;
          thisRow.push_back(above);
          merged = true;
          break;
        }
      }
      if (!merged) {
        runs.push_back(Run{x0, x1, cy, cy + 1});
        thisRow.push_back(runs.size() - 1);
      }
    }
    previousRow.swap(thisRow);
  }

  rects.reserve(runs.size());
  for (const auto& run : runs) {
    UiRect r;
    r.left = static_cast<int32_t>(run.x0 * cell);
    r.top = static_cast<int32_t>(run.y0 * cell);
    r.right = static_cast<int32_t>(std::min(run.x1 * cell, width));
    r.bottom = static_cast<int32_t>(std::min(run.y1 * cell, height));
    rects.push_back(r);
  }

  if (rects.size() > params.maxRects) {
    std::sort(rects.begin(), rects.end(), [](const UiRect& a, const UiRect& b) {
      const int64_t aa = static_cast<int64_t>(a.right - a.left) * (a.bottom - a.top);
      const int64_t bb = static_cast<int64_t>(b.right - b.left) * (b.bottom - b.top);
      return aa > bb;
    });
    rects.resize(params.maxRects);
  }
  return rects;
}

}  // namespace sidecar
