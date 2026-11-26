#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace umbriel {

  // All coordinates are logical layout coordinates, including origins on other outputs or virtual workspaces.
  struct SpreadBox {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
  };

  struct SpreadLayout {
    // Available content cells, indexed by window. Each window fits inside its cell without changing its aspect.
    std::vector<SpreadBox> cells;
    // Window indices in visual row order, independent of workspace enumeration and stacking order.
    std::vector<size_t> order;
    size_t columns = 0;
  };

  struct SpreadLayoutOptions {
    double spacing = 32;
    double maxScale = 1;
    bool caption = true;
  };

  SpreadBox spreadWallpaperBox(SpreadBox output, SpreadBox usable, std::span<const SpreadBox> windows, double progress);

  SpreadBox spreadOrigin(SpreadBox window, SpreadBox output, double workspaceDelta, bool horizontal);
  SpreadBox spreadFit(SpreadBox window, SpreadBox cell, double maxScale = 1.0);
  SpreadBox spreadInterpolate(SpreadBox origin, SpreadBox target, double progress);
  SpreadLayout spreadLayout(SpreadBox usable, std::span<const SpreadBox> origins, SpreadLayoutOptions options = {});

  // Distribute windows to their owner output, or gather them onto the invoking output.
  SpreadLayout spreadLayoutOnOutputs(
      std::span<const SpreadBox> outputs, std::span<const SpreadBox> origins, std::span<const size_t> owners,
      size_t invoking, bool gather, SpreadLayoutOptions options = {}
  );
  size_t spreadNeighbor(std::span<const SpreadBox> cells, size_t current, int dx, int dy);

} // namespace umbriel
