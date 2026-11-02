#include "config/resolve.h"
#include "input/cursor.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "view/view_internal.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {
  bool View::canUseRegionGeometry() const {
    const ScratchpadManager* scratchpad = m_server->scratchpadManager();
    return m_mapped
        && !m_tiled
        && !attached()
        && !m_pinned
        && !scheduledFullscreen()
        && !currentFullscreen()
        && (scratchpad == nullptr || !scratchpad->contains(this));
  }

  bool View::canSetRegion(const Workspace* destination) const {
    const Workspace* workspace = destination != nullptr ? destination : m_workspace;
    return canUseRegionGeometry() && workspace != nullptr && workspace->layoutMode() == LayoutMode::Stacking;
  }

  std::optional<wlr_box>
  View::floatingRegionBox(WindowRegion region, const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    const auto resolved = layout != nullptr ? *layout
        : m_workspace != nullptr            ? m_workspace->layoutConfig()
                                            : resolveGlobalLayout(config());
    const wlr_box tile = windowRegionBox(floatingMarginArea(usable, &resolved), region, resolved.totalGap);
    // Region placement leaves maximize-to-edges, which makes the title bar visible again.
    const int bar = wantsTitleBar(m_titleBarMode.value_or(config().appearance.titleBar.mode))
        ? config().appearance.titleBar.height
        : 0;
    if (tile.width <= 0 || tile.height <= bar) {
      return std::nullopt;
    }
    const wlr_box content = contentBoxInTile(tile, bar);
    const SizeHints hints = sizeHints();
    if (hints.minWidth > content.width || hints.minHeight > content.height) {
      return std::nullopt;
    }
    const int width = clampWidth(content.width, hints);
    const int height = clampHeight(content.height, hints);
    return wlr_box{
        .x = content.x + (content.width - width) / 2,
        .y = content.y + (content.height - height) / 2,
        .width = width,
        .height = height,
    };
  }

  bool View::setRegion(WindowRegion region, std::optional<FloatingPoint> restoreOrigin) {
    if (!canSetRegion()) {
      return false;
    }
    const auto box = floatingRegionBox(region, floatingUsableArea());
    if (!box) {
      return false;
    }
    const auto [width, height] = floatingSize();
    // Pending configures and animation targets already describe this placement. A repeated set must not restart
    // either transition. Maximize can retain the region while presenting a different box, so it still needs a set.
    if (m_region == region
        && !scheduledMaximized()
        && !m_maximizedToEdges
        && !m_floatingMaximized
        && box->x == m_regionBox.x
        && box->y == m_regionBox.y
        && box->width == m_regionBox.width
        && box->height == m_regionBox.height
        && layoutTargetX() == box->x
        && layoutTargetY() == box->y
        && width == box->width
        && height == box->height) {
      return true;
    }
    if (!m_region) {
      m_regionRestoreBox = m_hasMaximizeRestoreBox
          ? m_maximizeRestoreBox
          : wlr_box{.x = layoutTargetX(), .y = layoutTargetY(), .width = width, .height = height};
      if (restoreOrigin) {
        m_regionRestoreBox.x = restoreOrigin->x;
        m_regionRestoreBox.y = restoreOrigin->y;
      }
      m_hasRegionRestoreBox = m_regionRestoreBox.width > 0 && m_regionRestoreBox.height > 0;
    }
    dropMaximizedForResize();
    m_region = region;
    m_regionBox = *box;
    requestFloatingSize(box->width, box->height);
    beginResizeAnimation(box->width, box->height);
    animateTo(box->x, box->y);
    syncSsdFrame();
    m_server->scheduleIpcWindowsEvent();
    return true;
  }

  void View::clearRegionState() {
    if (m_region) {
      m_server->scheduleIpcWindowsEvent();
    }
    m_region.reset();
    m_hasRegionRestoreBox = false;
    m_regionRestoreBox = {};
    m_regionBox = {};
  }

  void View::clearRegion(bool animate) {
    if (!m_region || !canUseRegionGeometry()) {
      return;
    }
    const auto restore = m_hasRegionRestoreBox ? std::optional{m_regionRestoreBox} : std::nullopt;
    clearRegionState();
    dropMaximizedForResize();
    if (!restore) {
      return;
    }
    requestFloatingSize(restore->width, restore->height);
    if (animate) {
      beginResizeAnimation(restore->width, restore->height);
      animateTo(restore->x, restore->y);
    } else {
      cancelSizeAnimation();
      setPosition(restore->x, restore->y);
    }
    rememberFloatingPosition();
  }

  void View::refitFloatingRegion() {
    if (!m_region) {
      return;
    }
    if (m_workspace == nullptr || m_workspace->layoutMode() != LayoutMode::Stacking) {
      clearRegionState();
      return;
    }
    if (!canUseRegionGeometry() || scheduledMaximized() || m_server->cursor()->isDraggingView(this)) {
      return;
    }
    const auto box = floatingRegionBox(*m_region, floatingUsableArea());
    if (!box) {
      clearRegion();
      return;
    }
    if (box->x == m_regionBox.x
        && box->y == m_regionBox.y
        && box->width == m_regionBox.width
        && box->height == m_regionBox.height) {
      return;
    }
    m_regionBox = *box;
    requestFloatingSize(box->width, box->height);
    setPosition(box->x, box->y);
    syncFloatingSurfaceClip();
  }
} // namespace umbriel
