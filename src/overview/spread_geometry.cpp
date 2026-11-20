#include "overview/spread_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace umbriel {

  SpreadBox
  spreadWallpaperBox(SpreadBox output, SpreadBox usable, std::span<const SpreadBox> windows, double progress) {
    if (usable.width <= 0 || usable.height <= 0) {
      usable = output;
    }
    const double cx = usable.x + usable.width / 2;
    const double cy = usable.y + usable.height / 2;
    // Give sparse overviews the breathing room of a workspace preview. Dense
    // grids expand the wallpaper to contain their existing windows and gaps.
    double halfWidth = usable.width * 0.35;
    double halfHeight = usable.height * 0.35;
    constexpr double padding = 16;
    for (const auto& window : windows) {
      halfWidth = std::max({halfWidth, cx - window.x + padding, window.x + window.width - cx + padding});
      halfHeight = std::max({halfHeight, cy - window.y + padding, window.y + window.height - cy + padding});
    }
    halfWidth = std::min(halfWidth, usable.width / 2);
    halfHeight = std::min(halfHeight, usable.height / 2);
    return spreadInterpolate(output, {cx - halfWidth, cy - halfHeight, halfWidth * 2, halfHeight * 2}, progress);
  }

  SpreadBox spreadOrigin(SpreadBox window, SpreadBox output, double workspaceDelta, bool horizontal) {
    if (horizontal) {
      window.x += workspaceDelta * output.width;
    } else {
      window.y += workspaceDelta * output.height;
    }
    return window;
  }

  SpreadBox spreadFit(SpreadBox window, SpreadBox cell, double maxScale) {
    const double scale = std::max(
        0.0,
        std::min(
            {std::clamp(maxScale, 0.1, 1.0), cell.width / std::max(1.0, window.width),
             cell.height / std::max(1.0, window.height)}
        )
    );
    const double width = window.width * scale;
    const double height = window.height * scale;
    return {cell.x + (cell.width - width) / 2, cell.y + (cell.height - height) / 2, width, height};
  }

  SpreadBox spreadInterpolate(SpreadBox origin, SpreadBox target, double progress) {
    // Spring overshoot must not make a card invert, nor carry hidden-workspace windows past their desktop landing.
    const double p = std::clamp(progress, 0.0, 1.0);
    return {
        std::lerp(origin.x, target.x, p), std::lerp(origin.y, target.y, p), std::lerp(origin.width, target.width, p),
        std::lerp(origin.height, target.height, p)
    };
  }

  SpreadLayout spreadLayout(SpreadBox usable, std::span<const SpreadBox> origins, SpreadLayoutOptions options) {
    SpreadLayout layout;
    const size_t count = origins.size();
    if (count == 0 || usable.width <= 0 || usable.height <= 0) {
      return layout;
    }
    usable.x += usable.width * 0.06;
    usable.y += usable.height * 0.09;
    usable.width *= 0.88;
    usable.height *= 0.82;
    const double density = std::min(1.0, std::sqrt(usable.width * usable.height / (count * 240.0 * 180.0)));
    const double gap = std::clamp(options.spacing, 0.0, 128.0) * density;
    const double caption = options.caption ? 40 * density : 0.0;
    double bestArea = -1;
    double cellWidth = 0;
    double cellHeight = 0;
    for (size_t columns = 1; columns <= count; ++columns) {
      const size_t candidateRows = (count + columns - 1) / columns;
      const double width = (usable.width - gap * (columns - 1)) / columns;
      const double height = (usable.height - gap * (candidateRows - 1)) / candidateRows;
      double area = 0;
      for (const SpreadBox& origin : origins) {
        const SpreadBox fit = spreadFit(origin, {0, 0, width, height - caption}, options.maxScale);
        area += fit.width * fit.height;
      }
      if (area > bestArea) {
        bestArea = area;
        layout.columns = columns;
        cellWidth = width;
        cellHeight = height;
      }
    }
    std::vector<SpreadBox> slots;
    for (size_t slot = 0; slot < count; ++slot) {
      const size_t row = slot / layout.columns;
      const size_t rowColumns = std::min(layout.columns, count - row * layout.columns);
      slots.push_back({
          usable.x
              + (layout.columns - rowColumns) * (cellWidth + gap) / 2
              + (slot % layout.columns) * (cellWidth + gap),
          usable.y + row * (cellHeight + gap),
          cellWidth,
          std::max(1.0, cellHeight - caption),
      });
    }
    // Minimize total Euclidean travel. Uncrossing two intersecting center paths shortens their total length, so
    // this assignment naturally keeps windows from opposite workspace directions on the corresponding grid side.
    std::vector<std::vector<double>> costs(count, std::vector<double>(count));
    for (size_t i = 0; i < count; ++i) {
      for (size_t j = 0; j < count; ++j) {
        costs[i][j] = std::hypot(
            origins[i].x + origins[i].width / 2 - slots[j].x - slots[j].width / 2,
            origins[i].y + origins[i].height / 2 - slots[j].y - slots[j].height / 2
        );
      }
    }
    // Hungarian assignment, O(n^3), with stable enumeration to break equal-distance ties.
    std::vector<double> rowPotential(count + 1), columnPotential(count + 1);
    std::vector<size_t> matchedRow(count + 1), previousColumn(count + 1);
    for (size_t row = 1; row <= count; ++row) {
      matchedRow[0] = row;
      size_t column = 0;
      std::vector<double> slack(count + 1, std::numeric_limits<double>::infinity());
      std::vector<bool> used(count + 1, false);
      do {
        used[column] = true;
        const size_t currentRow = matchedRow[column];
        double delta = std::numeric_limits<double>::infinity();
        size_t nextColumn = 0;
        for (size_t candidate = 1; candidate <= count; ++candidate) {
          if (used[candidate]) {
            continue;
          }
          const double cost =
              costs[currentRow - 1][candidate - 1] - rowPotential[currentRow] - columnPotential[candidate];
          if (cost < slack[candidate]) {
            slack[candidate] = cost;
            previousColumn[candidate] = column;
          }
          if (slack[candidate] < delta) {
            delta = slack[candidate];
            nextColumn = candidate;
          }
        }
        for (size_t candidate = 0; candidate <= count; ++candidate) {
          if (used[candidate]) {
            rowPotential[matchedRow[candidate]] += delta;
            columnPotential[candidate] -= delta;
          } else {
            slack[candidate] -= delta;
          }
        }
        column = nextColumn;
      } while (matchedRow[column] != 0);
      do {
        const size_t previous = previousColumn[column];
        matchedRow[column] = matchedRow[previous];
        column = previous;
      } while (column != 0);
    }
    layout.cells.resize(count);
    layout.order.resize(count);
    for (size_t slot = 0; slot < count; ++slot) {
      const size_t index = matchedRow[slot + 1] - 1;
      layout.order[slot] = index;
      layout.cells[index] = slots[slot];
    }
    return layout;
  }

  SpreadLayout spreadLayoutOnOutputs(
      std::span<const SpreadBox> outputs, std::span<const SpreadBox> origins, std::span<const size_t> owners,
      size_t invoking, bool gather, SpreadLayoutOptions options
  ) {
    SpreadLayout result;
    if (owners.size() != origins.size() || (gather && invoking >= outputs.size())) {
      return result;
    }
    result.cells.resize(origins.size());
    result.order.reserve(origins.size());
    for (size_t output = 0; output < outputs.size(); ++output) {
      std::vector<SpreadBox> group;
      std::vector<size_t> indices;
      group.reserve(origins.size());
      indices.reserve(origins.size());
      for (size_t window = 0; window < origins.size(); ++window) {
        if ((gather ? invoking : owners[window]) == output) {
          group.push_back(origins[window]);
          indices.push_back(window);
        }
      }
      const auto layout = spreadLayout(outputs[output], group, options);
      for (size_t i = 0; i < layout.cells.size(); ++i) {
        result.cells[indices[i]] = layout.cells[i];
      }
      for (const size_t index : layout.order) {
        result.order.push_back(indices[index]);
      }
      if (!layout.order.empty()) {
        result.columns = layout.columns;
      }
    }
    return result;
  }

  size_t spreadNeighbor(std::span<const SpreadBox> cells, size_t current, int dx, int dy) {
    if (current >= cells.size() || (dx == 0 && dy == 0)) {
      return current;
    }
    const auto& origin = cells[current];
    size_t next = current;
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < cells.size(); ++i) {
      const double x = cells[i].x + cells[i].width / 2 - origin.x - origin.width / 2;
      const double y = cells[i].y + cells[i].height / 2 - origin.y - origin.height / 2;
      const double forward = x * dx + y * dy;
      if (forward <= 0.5) {
        continue;
      }
      const double perpendicular = std::abs(x * dy - y * dx);
      const double distance = forward + 3 * perpendicular;
      if (distance < best) {
        best = distance;
        next = i;
      }
    }
    return next;
  }

} // namespace umbriel
