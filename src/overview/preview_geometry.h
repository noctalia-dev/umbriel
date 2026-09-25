#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace umbriel {

  // Gap between workspace thumbnails, as a fraction of the scaled row height.
  inline constexpr double kPreviewRowGapFraction = 0.1;

  // Workspace preview placement on one output. Every field is whole layout pixels, so a filmstrip scrolled by whole
  // rows puts every preview on the pixel grid and a settling row cannot round to a neighbouring pixel.
  struct PreviewGrid {
    int width = 0;
    int height = 0;
    int baseX = 0;
    int baseY = 0;
    int gap = 0;

    // Distance between neighbouring previews along the workspace axis.
    [[nodiscard]] int step(bool horizontal) const { return (horizontal ? width : height) + gap; }
  };

  // Previews scaled by zoom and centred on the output box; the gap scales with the workspace axis.
  [[nodiscard]] inline PreviewGrid
  previewGrid(int outputX, int outputY, int outputWidth, int outputHeight, double zoom, bool horizontal) {
    PreviewGrid grid;
    grid.width = std::max(1, static_cast<int>(std::lround(outputWidth * zoom)));
    grid.height = std::max(1, static_cast<int>(std::lround(outputHeight * zoom)));
    grid.baseX = static_cast<int>(std::lround(outputX + (outputWidth - grid.width) / 2.0));
    grid.baseY = static_cast<int>(std::lround(outputY + (outputHeight - grid.height) / 2.0));
    const int axisExtent = horizontal ? outputWidth : outputHeight;
    grid.gap = static_cast<int>(std::lround(kPreviewRowGapFraction * axisExtent * zoom));
    return grid;
  }

  // Origin along the workspace axis of the preview for workspaceIndex while the filmstrip is at workspaceScroll.
  [[nodiscard]] inline int
  previewAxisOrigin(const PreviewGrid& grid, bool horizontal, size_t workspaceIndex, double workspaceScroll) {
    const double offset = (static_cast<double>(workspaceIndex) - workspaceScroll) * grid.step(horizontal);
    return static_cast<int>(std::lround((horizontal ? grid.baseX : grid.baseY) + offset));
  }

} // namespace umbriel
