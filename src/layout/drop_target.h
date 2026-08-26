#pragma once

#include <cstdint>
#include <optional>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class Server;
  class View;
  class Workspace;

  struct DropColumnWidth {
    double fraction = 0.5;
    bool fullWidth = false;
  };

  struct DropTargetOptions {
    bool clipHintToUsable = true;
    bool reserveScrollingViewportEdges = true;
    bool endpointGapsOutsideColumns = false;
  };
  // Captures the width owned by a single-view scrolling column before a drag
  // detaches it. Stacked views do not own their destination column width.
  [[nodiscard]] std::optional<DropColumnWidth> captureDropColumnWidth(const Workspace& source, const View* view);

  // Where a dragged tile would land, plus the world-space hint rectangle. Scrolling: row >= 0 inserts into column
  // `column` at that row; row < 0 opens a new column at gap index `column`. Dwindle: `view`/`edge` name a directional
  // split; when there is no splittable leaf they are null/0 and `column` falls back to the append index. A zero-sized
  // hintBox means "draw nothing".
  struct DropTarget {
    Workspace* workspace = nullptr;
    int column = -1;
    int row = -1;
    View* view = nullptr;
    uint32_t edge = 0;
    wlr_box hintBox{};
  };

  // `worldX`/`worldY` are layout coordinates. The normal cursor path clips the hint to the output's usable area and
  // reserves its viewport edges. A projected overview path can preserve the hint until after applying its own output
  // bounds and use only the scrolling strip's content-space targets.
  [[nodiscard]] DropTarget computeDropTarget(
      Workspace& workspace, double worldX, double worldY, const View* excludedView, const DropTargetOptions& options
  );

  // Inserts `view` at `drop` and focuses it. `columnWidth` restores a detached scrolling column when the drop opens a
  // new column; stack drops retain the destination column's width. The caller has already detached `view`.
  void applyDrop(
      Server& server, View& view, Workspace& target, const DropTarget& drop, const DropColumnWidth* columnWidth,
      bool animate
  );

} // namespace umbriel
