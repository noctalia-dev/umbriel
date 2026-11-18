#pragma once

#include "core/animation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  // Gap between workspace thumbnails, as a fraction of the scaled row height.
  inline constexpr double kPreviewRowGapFraction = 0.1;

  // Workspace preview placement in logical coordinates. Keep the grid fractional
  // through centering and filmstrip motion. The renderer snaps physical edges.
  struct PreviewGrid {
    double width = 0;
    double height = 0;
    double baseX = 0;
    double baseY = 0;
    double gap = 0;

    // Distance between neighbouring previews along the workspace axis.
    double step(bool horizontal) const { return (horizontal ? width : height) + gap; }
  };

  // Previews scaled by zoom and centred on the output box; the gap scales with the workspace axis.
  inline PreviewGrid
  previewGrid(int outputX, int outputY, int outputWidth, int outputHeight, double zoom, bool horizontal) {
    PreviewGrid grid;
    grid.width = std::max(1.0, outputWidth * zoom);
    grid.height = std::max(1.0, outputHeight * zoom);
    grid.baseX = outputX + (outputWidth - grid.width) / 2.0;
    grid.baseY = outputY + (outputHeight - grid.height) / 2.0;
    const int axisExtent = horizontal ? outputWidth : outputHeight;
    grid.gap = kPreviewRowGapFraction * axisExtent * zoom;
    return grid;
  }

  // Origin along the workspace axis of the preview for workspaceIndex while the filmstrip is at workspaceScroll.
  inline double
  previewAxisOrigin(const PreviewGrid& grid, bool horizontal, size_t workspaceIndex, double workspaceScroll) {
    const double offset = (static_cast<double>(workspaceIndex) - workspaceScroll) * grid.step(horizontal);
    return (horizontal ? grid.baseX : grid.baseY) + offset;
  }

  // Project an output-local edge between pixel-aligned desktop and overview
  // endpoints. A half-pixel endpoint must be reached during the spring's travel,
  // rather than crossing its rounding boundary only when the spring snaps to rest.
  inline double previewZoomEdge(
      double position, double extent, double zoom, double overviewZoom, double outputScale, double rowOffset = 0.0,
      double outputPixels = 0.0
  ) {
    const double desktop = position + rowOffset;
    const double overview = extent / 2.0 + (desktop - extent / 2.0) * overviewZoom;
    const double progress = (1.0 - zoom) / (1.0 - overviewZoom);

    // The logical resolution can truncate the output's final physical pixel.
    const double desktopPixels = position == extent && outputPixels > 0.0
        ? std::round(outputPixels + rowOffset * outputScale)
        : std::round(desktop * outputScale);
    return std::lerp(desktopPixels, std::round(overview * outputScale), progress) / outputScale;
  }

  struct PreviewProjection {
    wlr_box outputBox{};
    double zoom = 1.0;
    double overviewZoom = 0.5;
    double outputScale = 1.0;
    int outputPixelWidth = 0;
    int outputPixelHeight = 0;
    bool horizontal = false;
  };

  // Project backgrounds and cards through the same output-local physical grid.
  inline wlr_fbox projectPreviewPresentationBox(
      const PreviewProjection& projection, const wlr_fbox& world, double workspaceScroll, size_t workspaceIndex
  ) {
    const wlr_box& output = projection.outputBox;
    const double row = static_cast<double>(workspaceIndex) - workspaceScroll;
    const double rowOffset =
        row * (projection.horizontal ? output.width : output.height) * (1.0 + kPreviewRowGapFraction);

    const auto x = [&](double position) {
      return output.x
          + previewZoomEdge(
                 position - output.x, output.width, projection.zoom, projection.overviewZoom, projection.outputScale,
                 projection.horizontal ? rowOffset : 0.0, projection.outputPixelWidth
          );
    };

    const auto y = [&](double position) {
      return output.y
          + previewZoomEdge(
                 position - output.y, output.height, projection.zoom, projection.overviewZoom, projection.outputScale,
                 projection.horizontal ? 0.0 : rowOffset, projection.outputPixelHeight
          );
    };

    const double left = x(world.x);
    const double top = y(world.y);
    return {left, top, std::max(1.0, x(world.x + world.width) - left), std::max(1.0, y(world.y + world.height) - top)};
  }

  inline bool previewRowEdgesSettled(
      const PreviewProjection& projection, const wlr_fbox& world, size_t workspaceIndex, double target, double remaining
  ) {
    const double origin = projection.horizontal ? projection.outputBox.x : projection.outputBox.y;
    const auto pixels = [&](double scroll) {
      const wlr_fbox box = projectPreviewPresentationBox(projection, world, scroll, workspaceIndex);
      const double start = projection.horizontal ? box.x : box.y;
      const double extent = projection.horizontal ? box.width : box.height;
      return std::array{
          std::round((start - origin) * projection.outputScale),
          std::round((start + extent - origin) * projection.outputScale),
      };
    };

    const auto endpoint = pixels(target);
    return pixels(target - remaining) == endpoint && pixels(target + remaining) == endpoint;
  }

  // The caller checks every participating background and card, without allocating
  // a separate list. A zoom in flight owns the edges and prevents an early finish.
  template <typename EdgesSettled>
  bool finishPreviewRowScrollSpringTail(
      AnimatedValue& scroll, const PreviewProjection& projection, bool zoomAnimating, const EdgesSettled& edgesSettled
  ) {
    if (zoomAnimating || projection.zoom < projection.overviewZoom || projection.zoom > 1.0) {
      return false;
    }

    const double target = scroll.target();
    const double remaining =
        springDisplacementBound(scroll.current(), target, scroll.velocity(), scroll.curve().spring);
    if (!std::isfinite(remaining) || !edgesSettled(target, remaining)) {
      return false;
    }

    const wlr_box& output = projection.outputBox;
    const PreviewGrid grid =
        previewGrid(output.x, output.y, output.width, output.height, projection.zoom, projection.horizontal);

    return scroll.finishSpringTail(grid.step(projection.horizontal) * projection.outputScale);
  }

} // namespace umbriel
