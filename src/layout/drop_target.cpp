#include "layout/drop_target.h"

#include "config/config.h"
#include "layout/dwindle.h"
#include "layout/layout.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr int kColumnHintWidth = 300;
    constexpr int kRowHintEdgeHeight = 150;
    constexpr int kRowHintMidHeight = 300;
    constexpr int kScrollingEdgeDropWidth = 32;

    struct ScrollingTarget {
      int column = 0;
      int row = -1;
    };

    struct DwindleTarget {
      View* view = nullptr;
      uint32_t edge = 0;
      int leaf = -1;
      wlr_box hint{};
    };

    struct MasterTarget {
      int column = 0;
      int row = 0;
      wlr_box hint{};
    };

    wlr_box columnHintBox(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, int gapIndex, double scroll
    ) {
      const bool v = workspace.scrollingVertical();
      const int edgePad = workspace.layoutConfig().edgePad;
      const int gap = workspace.layoutConfig().totalGap;
      const int viewportPrimary = workspace.scrollViewportExtent();
      const int columnCount = static_cast<int>(workspace.layout().columns().size());
      const int clampedGap = std::clamp(gapIndex, 0, columnCount);

      int hintPrimary = 0;
      if (columnCount == 0) {
        hintPrimary = 0;
      } else if (clampedGap <= 0) {
        hintPrimary = layout.columnX(0, viewportPrimary) - gap - kColumnHintWidth;
      } else if (clampedGap >= columnCount) {
        hintPrimary = layout.columnX(columnCount - 1, viewportPrimary)
            + layout.columnWidth(columnCount - 1, viewportPrimary)
            + gap;
      } else {
        hintPrimary = layout.columnX(clampedGap, viewportPrimary) - gap / 2 - kColumnHintWidth / 2;
      }

      const int primaryPosition =
          (v ? usable.y : usable.x) + edgePad + hintPrimary - static_cast<int>(std::lround(scroll));
      const int crossPosition = (v ? usable.x : usable.y) + edgePad;
      const int crossExtent = std::max(1, (v ? usable.width : usable.height) - 2 * edgePad);
      if (v) {
        return {
            .x = crossPosition,
            .y = primaryPosition,
            .width = crossExtent,
            .height = kColumnHintWidth,
        };
      }
      return {
          .x = primaryPosition,
          .y = crossPosition,
          .width = kColumnHintWidth,
          .height = crossExtent,
      };
    }

    wlr_box stackHintBox(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, int columnIndex, int rowIndex,
        double scroll
    ) {
      if (columnIndex < 0 || columnIndex >= static_cast<int>(workspace.layout().columns().size())) {
        return {};
      }
      const bool v = workspace.scrollingVertical();
      const int edgePad = workspace.layoutConfig().edgePad;
      const int viewportPrimary = workspace.scrollViewportExtent();
      const Column& column = workspace.layout().columns()[static_cast<size_t>(columnIndex)];
      const int rowCount = static_cast<int>(column.views.size());
      const int row = std::clamp(rowIndex, 0, rowCount);
      const int crossOrigin = v ? usable.x : usable.y;
      const int crossExtent = v ? usable.width : usable.height;

      int hintCross = 0;
      int hintCrossExtent = 0;
      if (row == 0) {
        hintCross = crossOrigin + edgePad;
        hintCrossExtent = kRowHintEdgeHeight;
      } else if (row >= rowCount) {
        hintCross = crossOrigin + crossExtent - edgePad - kRowHintEdgeHeight;
        hintCrossExtent = kRowHintEdgeHeight;
      } else {
        const wlr_box target = workspace.layout().targetBox(column.views[static_cast<size_t>(row)]);
        const int boundary = (v ? target.x : target.y) - workspace.layoutConfig().totalGap / 2;
        hintCross = boundary - kRowHintMidHeight / 2;
        hintCrossExtent = kRowHintMidHeight;
      }

      const int primaryPosition = (v ? usable.y : usable.x)
          + edgePad
          + layout.columnX(columnIndex, viewportPrimary)
          - static_cast<int>(std::lround(scroll));
      const int primaryExtent = layout.columnWidth(columnIndex, viewportPrimary);
      if (v) {
        return {
            .x = hintCross,
            .y = primaryPosition,
            .width = hintCrossExtent,
            .height = primaryExtent,
        };
      }
      return {
          .x = primaryPosition,
          .y = hintCross,
          .width = primaryExtent,
          .height = hintCrossExtent,
      };
    }

    wlr_box clampHintBox(const wlr_box& box, const wlr_box& usable) {
      const int clampedX = std::max(box.x, usable.x);
      const int clampedY = std::max(box.y, usable.y);
      const int clampedX2 = std::min(box.x + box.width, usable.x + usable.width);
      const int clampedY2 = std::min(box.y + box.height, usable.y + usable.height);
      if (clampedX >= clampedX2 || clampedY >= clampedY2) {
        return {};
      }
      return {
          .x = clampedX,
          .y = clampedY,
          .width = clampedX2 - clampedX,
          .height = clampedY2 - clampedY,
      };
    }

    DwindleTarget
    computeDwindleTarget(const DwindleLayout& layout, double worldX, double worldY, const View* excludedView) {
      DwindleTarget result{};
      result.leaf = layout.leafIndexAt(worldX, worldY);
      if (result.leaf < 0) {
        return result;
      }

      const wlr_box targetBox = layout.targetBoxByIndex(result.leaf);
      const auto& leaves = layout.columns();
      if (targetBox.width <= 0
          || targetBox.height <= 0
          || result.leaf >= static_cast<int>(leaves.size())
          || leaves[static_cast<size_t>(result.leaf)].views.empty()) {
        return result;
      }

      result.view = leaves[static_cast<size_t>(result.leaf)].views.front();
      if (result.view == excludedView) {
        result.view = nullptr;
        return result;
      }

      const double fx = (worldX - targetBox.x) / static_cast<double>(targetBox.width);
      const double fy = (worldY - targetBox.y) / static_cast<double>(targetBox.height);
      result.hint = targetBox;
      if (std::min(fx, 1.0 - fx) <= std::min(fy, 1.0 - fy)) {
        if (fx <= 0.5) {
          result.edge = WLR_EDGE_LEFT;
          result.hint.width = targetBox.width / 2;
        } else {
          result.edge = WLR_EDGE_RIGHT;
          result.hint.x = targetBox.x + targetBox.width / 2;
          result.hint.width = targetBox.width - targetBox.width / 2;
        }
      } else if (fy <= 0.5) {
        result.edge = WLR_EDGE_TOP;
        result.hint.height = targetBox.height / 2;
      } else {
        result.edge = WLR_EDGE_BOTTOM;
        result.hint.y = targetBox.y + targetBox.height / 2;
        result.hint.height = targetBox.height - targetBox.height / 2;
      }
      return result;
    }
    bool atStripStartEdge(double primaryWorld, int primaryOrigin) {
      return primaryWorld >= static_cast<double>(primaryOrigin - kScrollingEdgeDropWidth)
          && primaryWorld <= static_cast<double>(primaryOrigin + kScrollingEdgeDropWidth);
    }

    bool atStripEndEdge(double primaryWorld, int primaryOrigin, int primaryExtent) {
      const int edge = primaryOrigin + primaryExtent;
      return primaryWorld >= static_cast<double>(edge - kScrollingEdgeDropWidth)
          && primaryWorld <= static_cast<double>(edge + kScrollingEdgeDropWidth);
    }

    ScrollingTarget computeScrollingTarget(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, double scroll, double worldX,
        double worldY, const DropTargetOptions& options
    ) {
      const bool v = workspace.scrollingVertical();
      const int edgePad = workspace.layoutConfig().edgePad;
      const int totalGap = workspace.layoutConfig().totalGap;
      const int viewportPrimary = workspace.scrollViewportExtent();
      const int columnCount = static_cast<int>(layout.columns().size());
      const double primaryWorld = v ? worldY : worldX;
      const int primaryOrigin = v ? usable.y : usable.x;
      const int primaryExtent = v ? usable.height : usable.width;
      const double crossWorld = v ? worldX : worldY;
      const int crossOrigin = v ? usable.x : usable.y;
      const int crossExtent = v ? usable.width : usable.height;

      // Reserve a bounded band around the viewport's primary edges as stable
      // prepend and append targets when the strip overflows. Overview cards
      // projected beyond the viewport remain ordinary content targets.
      if (options.reserveScrollingViewportEdges && columnCount > 0 && layout.maxScroll(viewportPrimary) > 0) {
        if (atStripStartEdge(primaryWorld, primaryOrigin)) {
          return {.column = 0, .row = -1};
        }
        if (atStripEndEdge(primaryWorld, primaryOrigin, primaryExtent)) {
          return {.column = columnCount, .row = -1};
        }
      }

      const double layoutPrimary = primaryWorld - primaryOrigin - edgePad + scroll;

      for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        const int columnPrimary = layout.columnX(columnIndex, viewportPrimary);
        const int columnExtent = layout.columnWidth(columnIndex, viewportPrimary);
        const double leadingFraction = options.endpointGapsOutsideColumns && columnIndex == 0 ? 0.0 : 0.2;
        const double trailingFraction =
            options.endpointGapsOutsideColumns && columnIndex == columnCount - 1 ? 1.0 : 0.8;
        if (layoutPrimary < columnPrimary + columnExtent * leadingFraction
            || layoutPrimary > columnPrimary + columnExtent * trailingFraction) {
          continue;
        }
        const Column& column = layout.columns()[static_cast<size_t>(columnIndex)];
        int nearestRow = 0;
        double rowDistance = std::abs(crossWorld - (crossOrigin + edgePad));
        for (int row = 1; row <= static_cast<int>(column.views.size()); ++row) {
          const wlr_box target =
              row == static_cast<int>(column.views.size()) ? wlr_box{} : layout.targetBox(column.views[row]);
          const int boundary = row == static_cast<int>(column.views.size()) ? crossOrigin + crossExtent - edgePad
                                                                            : (v ? target.x : target.y) - totalGap / 2;
          const double distance = std::abs(crossWorld - boundary);
          if (distance < rowDistance) {
            nearestRow = row;
            rowDistance = distance;
          }
        }
        return {.column = columnIndex, .row = nearestRow};
      }

      int nearestGap = 0;
      double nearestDistance = std::abs(layoutPrimary);
      for (int gap = 1; gap <= columnCount; ++gap) {
        const int boundary = gap == columnCount ? layout.columnX(gap, viewportPrimary) - totalGap
                                                : layout.columnX(gap, viewportPrimary) - totalGap / 2;
        const double distance = std::abs(layoutPrimary - boundary);
        if (distance < nearestDistance) {
          nearestGap = gap;
          nearestDistance = distance;
        }
      }
      return {.column = nearestGap, .row = -1};
    }

    MasterTarget computeMasterTarget(
        const Workspace& workspace, const Layout& layout, const wlr_box& /*usable*/, double worldX, double worldY
    ) {
      if (layout.columns().empty()) {
        return {};
      }

      int selectedColumn = 0;
      wlr_box selectedArea{};
      double selectedDistance = 0.0;
      bool haveSelection = false;
      for (int columnIndex = 0; columnIndex < static_cast<int>(layout.columns().size()); ++columnIndex) {
        const Column& column = layout.columns()[static_cast<size_t>(columnIndex)];
        wlr_box area{};
        bool haveArea = false;
        for (const View* view : column.views) {
          const wlr_box box = layout.targetBox(view);
          if (box.width <= 0 || box.height <= 0) {
            continue;
          }
          if (!haveArea) {
            area = box;
            haveArea = true;
          } else {
            const int right = std::max(area.x + area.width, box.x + box.width);
            const int bottom = std::max(area.y + area.height, box.y + box.height);
            area.x = std::min(area.x, box.x);
            area.y = std::min(area.y, box.y);
            area.width = right - area.x;
            area.height = bottom - area.y;
          }
        }
        if (!haveArea) {
          continue;
        }

        double distance = 0.0;
        if (worldX < area.x) {
          distance = area.x - worldX;
        } else if (worldX > area.x + area.width) {
          distance = worldX - (area.x + area.width);
        }
        if (!haveSelection || distance < selectedDistance) {
          selectedColumn = columnIndex;
          selectedArea = area;
          selectedDistance = distance;
          haveSelection = true;
        }
      }
      if (!haveSelection) {
        return {};
      }

      const Column& column = layout.columns()[static_cast<size_t>(selectedColumn)];
      int nearestRow = 0;
      double nearestDistance = std::abs(worldY - selectedArea.y);
      const int rowCount = static_cast<int>(column.views.size());
      for (int row = 1; row <= rowCount; ++row) {
        const int boundary = row == rowCount
            ? selectedArea.y + selectedArea.height
            : layout.targetBox(column.views[static_cast<size_t>(row)]).y - workspace.layoutConfig().totalGap / 2;
        const double distance = std::abs(worldY - boundary);
        if (distance < nearestDistance) {
          nearestRow = row;
          nearestDistance = distance;
        }
      }

      wlr_box hint{
          .x = selectedArea.x,
          .y = selectedArea.y,
          .width = selectedArea.width,
          .height = kRowHintEdgeHeight,
      };
      if (nearestRow == rowCount) {
        hint.y = selectedArea.y + selectedArea.height - kRowHintEdgeHeight;
      } else if (nearestRow > 0) {
        const int boundary =
            layout.targetBox(column.views[static_cast<size_t>(nearestRow)]).y - workspace.layoutConfig().totalGap / 2;
        hint.y = boundary - kRowHintMidHeight / 2;
        hint.height = kRowHintMidHeight;
      }
      return {.column = selectedColumn, .row = nearestRow, .hint = hint};
    }
  } // namespace
  std::optional<DropColumnWidth> captureDropColumnWidth(const Workspace& source, const View* view) {
    const ScrollingLayout* scrolling = source.scrollingLayout();
    if (scrolling == nullptr || view == nullptr) {
      return std::nullopt;
    }
    const int columnIndex = scrolling->columnOf(view);
    const auto& columns = scrolling->columns();
    if (columnIndex < 0 || columnIndex >= static_cast<int>(columns.size())) {
      return std::nullopt;
    }
    const Column& column = columns[static_cast<size_t>(columnIndex)];
    if (column.views.size() != 1) {
      return std::nullopt;
    }
    return DropColumnWidth{
        .fraction = column.savedWidthFrac > 0.0 ? column.savedWidthFrac : column.widthFrac,
        .fullWidth = column.savedWidthFrac > 0.0,
    };
  }

  DropTarget computeDropTarget(
      Workspace& workspace, double worldX, double worldY, const View* excludedView, const DropTargetOptions& options
  ) {
    DropTarget result{.workspace = &workspace};
    if (workspace.group() == nullptr || workspace.group()->output() == nullptr) {
      return result;
    }
    const wlr_box usable = workspace.group()->output()->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return result;
    }

    // Layout mode is a genuine policy fork here. Each layout wants different
    // drop targets and hint shapes.
    if (DwindleLayout* dwindle = workspace.dwindleLayout()) {
      const DwindleTarget target = computeDwindleTarget(*dwindle, worldX, worldY, excludedView);
      result.column = target.leaf >= 0 ? target.leaf : static_cast<int>(workspace.layout().columns().size());
      result.view = target.view;
      result.edge = target.edge;
      if (target.view != nullptr && target.edge != 0) {
        result.hintBox = target.hint;
      }
    } else if (workspace.layoutMode() == LayoutMode::Master) {
      const MasterTarget target = computeMasterTarget(workspace, workspace.layout(), usable, worldX, worldY);
      result.column = target.column;
      result.row = target.row;
      result.hintBox = target.hint;
    } else if (const ScrollingLayout* scrolling = workspace.scrollingLayout()) {
      const ScrollingLayout& layout = *scrolling;
      const double scroll = layout.scroll();
      const ScrollingTarget target = computeScrollingTarget(workspace, layout, usable, scroll, worldX, worldY, options);
      result.column = target.column;
      result.row = target.row;
      if (target.row >= 0) {
        result.hintBox = stackHintBox(workspace, layout, usable, target.column, target.row, scroll);
      } else {
        const bool v = workspace.scrollingVertical();
        const double primaryWorld = v ? worldY : worldX;
        const int primaryOrigin = v ? usable.y : usable.x;
        const int primaryExtent = v ? usable.height : usable.width;
        const int edgePad = workspace.layoutConfig().edgePad;
        if (options.reserveScrollingViewportEdges
            && target.column == 0
            && !layout.columns().empty()
            && atStripStartEdge(primaryWorld, primaryOrigin)) {
          result.hintBox = v
              ? wlr_box{
                    .x = usable.x + edgePad,
                    .y = usable.y,
                    .width = std::max(1, usable.width - 2 * edgePad),
                    .height = kColumnHintWidth,
                }
              : wlr_box{
                    .x = usable.x,
                    .y = usable.y + edgePad,
                    .width = kColumnHintWidth,
                    .height = std::max(1, usable.height - 2 * edgePad),
                };
        } else if (
            options.reserveScrollingViewportEdges
            && target.column == static_cast<int>(layout.columns().size())
            && atStripEndEdge(primaryWorld, primaryOrigin, primaryExtent)
        ) {
          result.hintBox = v
              ? wlr_box{
                    .x = usable.x + edgePad,
                    .y = usable.y + usable.height - kColumnHintWidth,
                    .width = std::max(1, usable.width - 2 * edgePad),
                    .height = kColumnHintWidth,
                }
              : wlr_box{
                    .x = usable.x + usable.width - kColumnHintWidth,
                    .y = usable.y + edgePad,
                    .width = kColumnHintWidth,
                    .height = std::max(1, usable.height - 2 * edgePad),
                };
        } else {
          result.hintBox = columnHintBox(workspace, layout, usable, target.column, scroll);
        }
      }
    } else {
      result.column = static_cast<int>(workspace.layout().columns().size());
    }

    if (options.clipHintToUsable) {
      result.hintBox = clampHintBox(result.hintBox, usable);
    }
    return result;
  }

  void applyDrop(
      Server& server, View& view, Workspace& target, const DropTarget& drop, const DropColumnWidth* columnWidth,
      bool animate
  ) {
    if (server.scratchpadManager() != nullptr && server.scratchpadManager()->contains(&view)) {
      return;
    }
    const auto restoreSceneParent = [&view, &target]() {
      const bool fullscreen = view.toplevel()->current.fullscreen || view.toplevel()->scheduled.fullscreen;
      wlr_scene_node_reparent(&view.sceneTree()->node, fullscreen ? target.fullscreenTree() : target.viewLayer(true));
    };
    // Policy fork again: a dwindle drop splits a leaf, while other layouts use
    // the generic column and row insertion interface.
    if (DwindleLayout* dwindle = target.dwindleLayout()) {
      const bool splitDrop =
          drop.view != nullptr && drop.view != &view && drop.edge != 0 && dwindle->columnOf(drop.view) >= 0;
      if (view.workspace() != &target) {
        // Auto-attach would split the focused leaf and send a stale configure
        // before the explicit placement below.
        view.moveToWorkspace(&target, /*attachToLayout=*/false);
      } else {
        dwindle->removeView(&view);
      }
      if (splitDrop) {
        dwindle->insertViewSplitOnView(&view, drop.view, drop.edge);
      } else {
        dwindle->insertView(&view, static_cast<int>(dwindle->columns().size()));
      }
      restoreSceneParent();
      target.markArrange(animate);
      server.focusView(&view, FocusReason::DragDrop);
      return;
    }

    if (view.workspace() != &target) {
      view.moveToWorkspace(&target, /*attachToLayout=*/false);
    } else {
      target.layout().removeView(&view);
    }
    if (drop.row >= 0) {
      target.layout().insertViewIntoColumn(&view, std::max(0, drop.column), drop.row);
    } else {
      target.layout().insertView(&view, std::max(0, drop.column));
    }
    if (columnWidth != nullptr && drop.row < 0) {
      const int column = target.layout().columnOf(&view);
      target.layout().setWidthFraction(column, columnWidth->fraction);
      if (columnWidth->fullWidth) {
        target.layout().toggleFullWidth(column);
      }
      wlr_xdg_toplevel_set_maximized(view.toplevel(), columnWidth->fullWidth);
    }
    restoreSceneParent();
    target.markArrange(animate);
    server.focusView(&view, FocusReason::DragDrop);
  }

} // namespace umbriel
