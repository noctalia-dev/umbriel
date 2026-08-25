#include "workspace/workspace.h"

#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "input/cursor.h"
#include "layout/dwindle.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <utility>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr Logger kLog("workspace");

    constexpr uint32_t kWorkspaceCaps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE
        | EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

    constexpr uint32_t kGroupCaps = EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE;

    // The bridge between the layout's opaque View identity and the client state it needs to size that view. Workspace
    // owns both sides, so it owns the lookup; layout/ stays free of view/ and its geometry stays testable.
    LayoutConstraints viewLayoutConstraints(const View* view) {
      const wlr_xdg_toplevel* toplevel = view != nullptr ? view->toplevel() : nullptr;
      const XdgSizeHints hints = xdgSizeHints(toplevel);
      return {
          .minWidth = hints.minWidth,
          .minHeight = hints.minHeight,
          .maxWidth = hints.maxWidth,
          .maxHeight = hints.maxHeight,
          .fullscreen = view != nullptr && view->layoutFullscreen(),
          .maximizedToEdges = view != nullptr && view->maximizedToEdges(),
      };
    }
  } // namespace

  Workspace::Workspace(
      WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string id, std::string name, size_t index,
      ResolvedLayoutConfig layoutConfig
  )
      : m_group(&group), m_handle(handle), m_id(std::move(id)), m_name(std::move(name)), m_index(index),
        m_layout(createLayout(layoutConfig.mode)), m_layoutConfig(std::move(layoutConfig)),
        m_layoutMode(m_layoutConfig.mode) {
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    m_handle->data = this;
    wlr_ext_workspace_handle_v1_set_group(m_handle, m_group->handle());
    wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
    wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    m_tree = wlr_scene_tree_create(m_group->output()->viewRoot());
    // Focus raises only within a layer: floating views can never fall below tiles.
    m_shadowLayer = wlr_scene_tree_create(m_tree);
    m_tiledLayer = wlr_scene_tree_create(m_tree);
    m_floatingLayer = wlr_scene_tree_create(m_tree);
    m_fullscreenTree = wlr_scene_tree_create(m_group->output()->fullscreenRoot());
  }

  Workspace::~Workspace() {
    for (View* view : m_views) {
      view->cancelPositionAnimation();
      const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
      wlr_scene_node_reparent(
          &view->sceneTree()->node, fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree()
      );
      view->detachWorkspace();
    }
    m_views.clear();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_shadowLayer = nullptr;
      m_tiledLayer = nullptr;
      m_floatingLayer = nullptr;
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_node_destroy(&m_fullscreenTree->node);
      m_fullscreenTree = nullptr;
    }
    if (m_handle != nullptr) {
      if (m_handle->data == this) {
        m_handle->data = nullptr;
      }
      wlr_ext_workspace_handle_v1_destroy(m_handle);
      m_handle = nullptr;
    }
  }

  ScrollingLayout* Workspace::scrollingLayout() {
    return m_layoutMode == LayoutMode::Scrolling ? static_cast<ScrollingLayout*>(m_layout.get()) : nullptr;
  }

  DwindleLayout* Workspace::dwindleLayout() {
    return m_layoutMode == LayoutMode::Dwindle ? static_cast<DwindleLayout*>(m_layout.get()) : nullptr;
  }

  const ScrollingLayout* Workspace::scrollingLayout() const {
    return m_layoutMode == LayoutMode::Scrolling ? static_cast<const ScrollingLayout*>(m_layout.get()) : nullptr;
  }

  bool Workspace::scrollingVertical() const {
    return scrollingLayout() != nullptr && m_layoutConfig.scrolling.direction == ScrollingDirection::Vertical;
  }

  void Workspace::setActive(bool active) {
    if (m_active == active) {
      return;
    }
    m_active = active;
    wlr_ext_workspace_handle_v1_set_active(m_handle, active);
    applyVisibility();
    if (active) {
      markArrange(false);
      m_group->output()->updateVrr();
      m_group->output()->updateHdr();
    }
  }

  void Workspace::updateUrgent() {
    const bool urgent = std::ranges::any_of(m_views, [](const View* view) { return view->urgent(); });
    wlr_ext_workspace_handle_v1_set_urgent(m_handle, urgent);
  }

  void Workspace::setFocusedView(View* view) {
    if (view == nullptr || view->workspace() == this) {
      m_focusedView = view;
      if (view != nullptr && view->floating()) {
        std::erase(m_floatingStack, view);
        m_floatingStack.push_back(view);
        restackFloatingViews();
      }
    }
  }

  void Workspace::syncFloatingStack(View* view) {
    if (view == nullptr || view->workspace() != this) {
      return;
    }
    if (view->floating()) {
      if (std::ranges::find(m_floatingStack, view) == m_floatingStack.end()) {
        m_floatingStack.push_back(view);
      }
    } else {
      std::erase(m_floatingStack, view);
    }
    restackFloatingViews();
  }

  void Workspace::restackFloatingViews() {
    for (View* view : m_floatingStack) {
      if (view != nullptr && view->workspace() == this && view->floating()) {
        view->raiseToTop();
      }
    }
  }

  void Workspace::addView(View* view, bool attachToLayout) {
    if (view == nullptr || std::ranges::find(m_views, view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    updateUrgent();
    const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
    if (view->pinned()) {
      // Cross-output moves have to rehome the pinned view onto the new output's clipped roots.
      view->restorePinnedSceneParent();
    } else {
      wlr_scene_node_reparent(&view->sceneTree()->node, fs ? m_fullscreenTree : viewLayer(view->tiled()));
      view->reparentShadow(m_shadowLayer);
    }
    syncFloatingStack(view);
    applyVisibility();
    if (attachToLayout) {
      layoutAttach(view);
    }
    m_group->reconcileDynamic();
  }

  View* Workspace::removeView(View* view, bool reconcile) {
    if (view == nullptr) {
      return nullptr;
    }
    if (!view->pinned()) {
      const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
      wlr_scene_node_reparent(
          &view->sceneTree()->node, fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree()
      );
      view->reparentShadow(nullptr);
    }
    View* replacement = m_focusedView == view ? focusReplacementForRemoval(view) : nullptr;
    detachFromLayout(view);
    std::erase(m_views, view);
    updateUrgent();
    std::erase(m_floatingStack, view);
    std::erase(m_switchViews, view);

    if (m_focusedView == view) {
      m_focusedView = replacement;
    }
    // Re-anchor the strip on whatever is focused now, the way every other focus-moving operation does. Activating an
    // adjacent column and fitting the view prevents the old scroll offset from leaving a survivor cut off at the left
    // edge while empty space opens on the right.
    ensureFocusedVisible();
    markArrange();
    if (reconcile) {
      m_group->reconcileDynamic();
    }
    return replacement;
  }

  void Workspace::layoutAttach(View* view, std::optional<double> initialWidth) {
    if (view == nullptr || !view->mapped() || !view->tiled() || m_layout->columnOf(view) >= 0) {
      return;
    }
    const int focusedColumn = m_layout->columnOf(m_focusedView);
    const int index = focusedColumn >= 0 ? focusedColumn + 1 : static_cast<int>(m_layout->columns().size());
    m_layout->insertView(view, index);
    // default_width is a viewport fraction: scrolling only. Dwindle ignores it.
    if (ScrollingLayout* scrolling = scrollingLayout(); initialWidth && scrolling != nullptr) {
      scrolling->setWidthFraction(scrolling->columnOf(view), *initialWidth);
    }
    markArrange(true);
  }

  void Workspace::layoutDetach(View* view, bool animate) {
    detachFromLayout(view);
    // The column just left the strip, so the old offset can now point past the end: a survivor stays cut off at the
    // left edge while empty space opens on the right. Clamping re-anchors the remaining columns after removal while
    // leaving the offset alone if the strip is still longer than the viewport. Deliberately not inside arrange(): a
    // touchpad swipe overscrolls on purpose, and it arranges on every frame of the gesture.
    clampScrollToRange();
    markArrange(animate);
  }

  int Workspace::scrollViewportExtent() const {
    if (m_group == nullptr || m_group->output() == nullptr) {
      return 1;
    }
    const wlr_box usable = m_group->output()->usableArea();
    const int extent = scrollingVertical() ? usable.height : usable.width;
    return std::max(1, extent - 2 * m_layoutConfig.edgePad);
  }

  void Workspace::detachFromLayout(View* view) {
    ScrollingLayout* scrolling = scrollingLayout();
    // Measured before the removal, because it depends on where the column is.
    const double shift = scrolling != nullptr
        ? scrolling->scrollShiftForColumnRemoval(scrolling->columnOf(view), scrollViewportExtent())
        : 0.0;
    m_layout->removeView(view);
    if (scrolling != nullptr && shift != 0.0) {
      scrolling->setScroll(scrolling->scroll() - shift);
    }
  }

  void Workspace::clampScrollToRange() {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr) {
      return;
    }
    const auto maxScroll = static_cast<double>(scrolling->maxScroll(scrollViewportExtent()));
    scrolling->setScroll(std::clamp(scrolling->scroll(), 0.0, maxScroll));
  }

  void Workspace::markArrange(bool animate) {
    // Last mark wins. The pairing that settles this is a touchpad scroll: every motion marks unanimated, and the
    // release that snaps to the nearest column marks animated, often in the same frame as the last motion. Letting the
    // unanimated mark win would teleport the strip at the end of every swipe. The opposite mistake, an animated mark
    // landing mid-drag, costs one tween on a frame where something unrelated also changed the layout.
    m_arrangeAnimate = animate;
    m_arrangePending = true;
    if (m_group != nullptr && m_group->output() != nullptr) {
      m_group->output()->markDirty(Dirty::Layout);
    }
  }

  void Workspace::flushArrange() {
    if (m_arrangePending) {
      arrange(m_arrangeAnimate);
    }
  }

  void Workspace::arrange(bool animate) {
    // Clearing here, rather than only in flushArrange, is what makes mixing the two safe: a direct arrange() satisfies
    // whatever was marked earlier in the frame, so the flush does not repeat it.
    m_arrangePending = false;
    // Layout math and client configures must run even for hidden workspaces: clients (games especially) change
    // fullscreen state while another workspace is active, and skipping the configure here leaves them with a stale size
    // (fullscreen at tile size, windowed at output size, ...).
    if (m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &usable);
    }
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }

    m_layout->arrange(usable);
    // The map-time IPC event can fire before this arrange runs, leaving the previous window positions in the listing.
    // Re-emit now that the layout boxes are settled; the event coalescer caps this at one per frame.
    m_group->server()->scheduleIpcWindowsEvent();
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped() || !view->tiled()) {
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        continue;
      }
      if (view->toplevel()->scheduled.fullscreen) {
        wlr_box fullArea{};
        wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &fullArea);
        if (fullArea.width > 0
            && fullArea.height > 0
            && (view->toplevel()->scheduled.width != fullArea.width
                || view->toplevel()->scheduled.height != fullArea.height)) {
          wlr_xdg_toplevel_set_size(view->toplevel(), fullArea.width, fullArea.height);
        }
        if (animate) {
          view->beginResizeAnimation(fullArea.width, fullArea.height, true);
        }
        continue;
      }
      // An unfullscreen configure with client-chosen size is in flight; the
      // column size waits for the ack (View::handleCommit re-arranges).
      if (view->awaitingUnfullscreenSize()) {
        continue;
      }
      const wlr_box target = tiledTargetBox(view, usable);
      const XdgSizeHints hints = xdgSizeHints(view->toplevel());
      const int width = view->maximizedToEdges() ? target.width : clampXdgWidth(target.width, hints);
      const int height = view->maximizedToEdges() ? target.height : clampXdgHeight(target.height, hints);
      const auto& scheduled = view->toplevel()->scheduled;
      if (scheduled.width != width || scheduled.height != height) {
        wlr_xdg_toplevel_set_size(view->toplevel(), width, height);
        // Start the presentation animation when the compositor changes the assigned size. Client geometry can differ
        // from a stable configure, notably with Chromium CSD, and must not replay the resize on focus.
        if (animate) {
          view->beginResizeAnimation(width, height, view->toplevel()->current.fullscreen);
        }
      }
    }

    // Visual state below (scroll, positions) only applies while visible.
    Overview* overview = m_group->server()->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    if (!m_active && !m_inSwitchTransition && !overviewActive) {
      return;
    }

    // One positioning path for both layouts: the layout owns the scroll, the
    // targets below already include it, and each view animates or snaps itself.
    applyPositions(animate);
    if (overviewActive) {
      overview->onWorkspaceArranged(this);
    }
  }

  void Workspace::syncViewPresentation(View* view) {
    if (view == nullptr || !view->mapped() || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    // A window under an interactive move spans outputs unclipped; leave it as presentGrabbedViewSpanning set it
    // (re-deriving presentation mid-drag flickers A<->B).
    if (Cursor* cursor = m_group->server()->cursor(); cursor != nullptr && cursor->isDraggingView(view)) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      usable = outputBox;
    }

    // Position from the node's CURRENT position, not the layout target: during position animations (column swaps, drag
    // drops) the node lags the target, and presentation derived from the target lands displaced on screen (cut-off
    // borders that reappear as the window settles).
    const wlr_scene_node& node = view->sceneTree()->node;

    // Each case only decides the region the view occupies; everything after is common. Keeping that region off the
    // neighbouring output is the job of the output's clipped scene roots, not of this function.
    wlr_box target{};
    const Overview* overview = m_group->server()->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    const bool normallyVisible = view->pinned() || m_active || m_inSwitchTransition;

    if (view->pinned()) {
      // Pinned views sit outside the workspace, so no slide offset applies, and
      // they are sized from committed geometry rather than the presented size.
      const wlr_box& geometry = view->toplevel()->base->geometry;
      target = {node.x, node.y, geometry.width, geometry.height};
    } else {
      if (!normallyVisible && !overviewActive) {
        return;
      }

      if (view->layoutFullscreen()) {
        if (m_layout->columnOf(view) < 0) {
          view->applyFullscreenLayout();
          view->setNodeEnabled(normallyVisible);
          return;
        }
        // Fullscreen covers the output and draws no decorations.
        target = {node.x, node.y + m_slideOffsetY, outputBox.width, outputBox.height};
      } else {
        // Floating views follow committed geometry; tiled ones follow the box
        // the layout assigned them.
        const wlr_box sized =
            m_layout->columnOf(view) < 0 ? view->toplevel()->base->geometry : tiledTargetBox(view, usable);
        target = {node.x, node.y + m_slideOffsetY, sized.width, sized.height};
      }
    }

    view->setNodeEnabled(normallyVisible);
    view->applyPresentation(target);
  }

  void Workspace::applyPositions(bool animate) {
    const Overview* overview = m_group != nullptr ? m_group->server()->overview() : nullptr;
    const bool overviewActive = overview != nullptr && overview->active();
    if ((!m_active && !m_inSwitchTransition && !overviewActive) || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      usable = outputBox;
    }
    const int viewportPrimary = scrollViewportExtent();

    // Position first, then let syncViewPresentation derive enable + clip from
    // the node's current position so animated and resting views share one path.
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped()) {
        continue;
      }
      if (view->layoutFullscreen()) {
        const int col = m_layout->columnOf(view);
        wlr_box target = outputBox;
        if (col >= 0) {
          if (const ScrollingLayout* scrolling = scrollingLayout()) {
            const int position = (scrollingVertical() ? outputBox.y : outputBox.x)
                + scrolling->columnX(col, viewportPrimary)
                - static_cast<int>(std::lround(scrolling->scroll()));
            if (scrollingVertical()) {
              target.y = position;
            } else {
              target.x = position;
            }
          }
        }
        if (animate) {
          view->animateTo(target.x, target.y);
        } else {
          view->setPosition(target.x, target.y);
        }
        syncViewPresentation(view);
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        // Floating (non-fullscreen): clip + enable against the home output.
        view->clampFloatingPosition();
        syncViewPresentation(view);
        continue;
      }
      wlr_box target = tiledTargetBox(view, usable);
      if (animate) {
        view->animateTo(target.x, target.y);
      } else {
        view->setPosition(target.x, target.y);
      }
      syncViewPresentation(view);
    }
  }

  wlr_box Workspace::tiledTargetBox(const View* view, const wlr_box& usable) const {
    wlr_box target = m_layout->targetBox(view);
    if (view == nullptr || !view->maximizedToEdges()) {
      return target;
    }
    if (scrollingLayout() != nullptr) {
      if (scrollingVertical()) {
        target.y -= m_layoutConfig.edgePad;
        target.x = usable.x;
      } else {
        target.x -= m_layoutConfig.edgePad;
        target.y = usable.y;
      }
    } else {
      target.x = usable.x;
      target.y = usable.y;
    }
    target.width = usable.width;
    target.height = usable.height;
    return target;
  }

  View* Workspace::focusAlongStrip(int direction) const {
    const int current = m_layout->columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return nullptr;
    }
    const Column& column = m_layout->columns()[static_cast<size_t>(target)];
    return column.views.empty() ? nullptr : column.views.front();
  }

  View* Workspace::focusWithinLane(int direction) const {
    if (View* vertical = m_layout->focusVerticalLeaf(m_focusedView, direction)) {
      return vertical;
    }
    const int column = m_layout->columnOf(m_focusedView);
    const int row = m_layout->rowOf(m_focusedView);
    if (column < 0 || row < 0) {
      return nullptr;
    }
    const auto& views = m_layout->columns()[static_cast<size_t>(column)].views;
    const int target = row + direction;
    return target < 0 || target >= static_cast<int>(views.size()) ? nullptr : views[static_cast<size_t>(target)];
  }

  View* Workspace::focusAdjacent(int direction) const {
    return scrollingVertical() ? focusWithinLane(direction) : focusAlongStrip(direction);
  }

  View* Workspace::focusVertical(int direction) const {
    return scrollingVertical() ? focusAlongStrip(direction) : focusWithinLane(direction);
  }

  View* Workspace::focusReplacementForRemoval(const View* view) const {
    if (view == nullptr) {
      return nullptr;
    }

    const int columnIndex = m_layout->columnOf(view);
    const int rowIndex = m_layout->rowOf(view);
    const auto& columns = m_layout->columns();
    const auto mappedCandidate = [view](View* candidate) {
      return candidate != nullptr && candidate != view && candidate->mapped();
    };

    if (columnIndex >= 0 && columnIndex < static_cast<int>(columns.size())) {
      const auto& column = columns[static_cast<size_t>(columnIndex)].views;
      for (int row = rowIndex - 1; row >= 0; --row) {
        if (View* candidate = column[static_cast<size_t>(row)]; mappedCandidate(candidate)) {
          return candidate;
        }
      }
      for (int row = rowIndex + 1; row < static_cast<int>(column.size()); ++row) {
        if (View* candidate = column[static_cast<size_t>(row)]; mappedCandidate(candidate)) {
          return candidate;
        }
      }
      for (int targetColumn = columnIndex - 1; targetColumn >= 0; --targetColumn) {
        for (View* candidate : columns[static_cast<size_t>(targetColumn)].views) {
          if (mappedCandidate(candidate)) {
            return candidate;
          }
        }
      }
      for (int targetColumn = columnIndex + 1; targetColumn < static_cast<int>(columns.size()); ++targetColumn) {
        for (View* candidate : columns[static_cast<size_t>(targetColumn)].views) {
          if (mappedCandidate(candidate)) {
            return candidate;
          }
        }
      }
    }

    const auto current = std::ranges::find(m_views, view);
    if (current != m_views.end()) {
      for (auto candidate = std::make_reverse_iterator(current); candidate != m_views.rend(); ++candidate) {
        if (mappedCandidate(*candidate)) {
          return *candidate;
        }
      }
      for (auto candidate = std::next(current); candidate != m_views.end(); ++candidate) {
        if (mappedCandidate(*candidate)) {
          return *candidate;
        }
      }
    }
    return nullptr;
  }

  bool Workspace::moveLaneAlongStrip(int direction) {
    const int current = m_layout->columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return false;
    }
    m_layout->moveColumn(current, target);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::consumeFocusedLeft() {
    if (!m_layout->consumeLeft(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::expelFocusedRight() {
    if (!m_layout->expelRight(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::moveWithinLane(int direction) {
    if (!m_layout->moveViewVertical(m_focusedView, direction)) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::moveFocusedColumn(int direction) {
    return scrollingVertical() ? moveWithinLane(direction) : moveLaneAlongStrip(direction);
  }

  bool Workspace::moveFocusedVertical(int direction) {
    return scrollingVertical() ? moveLaneAlongStrip(direction) : moveWithinLane(direction);
  }

  bool Workspace::cycleFocusedWidth(int direction) {
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->cycleWidth(column, direction)) {
      return false;
    }
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), false);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::setFocusedWidth(double fraction) {
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->setWidthFraction(column, fraction)) {
      return false;
    }
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), false);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::modifyFocusedWidth(double delta) {
    const int column = m_layout->columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    // Clamp here, not in the layouts: ScrollingLayout clamps internally but
    // DwindleLayout does not, and both must land in [0.1, 1.0].
    return setFocusedWidth(std::clamp(m_layout->widthFraction(column) + delta, 0.1, 1.0));
  }

  bool Workspace::toggleFocusedFullWidth() {
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    const bool fullWidth = m_layout->toggleFullWidth(column);
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), fullWidth);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::toggleFocusedMaximizedToEdges() {
    if (m_focusedView == nullptr || !m_focusedView->mapped()) {
      return false;
    }
    m_focusedView->toggleMaximizedToEdges();
    return true;
  }

  bool Workspace::toggleFocusedFullscreen() {
    if (m_focusedView == nullptr || !m_focusedView->mapped()) {
      return false;
    }
    m_focusedView->toggleFullscreen();
    return true;
  }

  bool Workspace::toggleFocusedFloating() {
    if (m_focusedView == nullptr || !m_focusedView->mapped()) {
      return false;
    }
    m_focusedView->toggleFloating();
    return true;
  }

  void Workspace::ensureFocusedVisible() {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    scrolling->ensureVisible(scrolling->columnOf(m_focusedView), scrollViewportExtent());
  }

  double Workspace::scrollFractionToReveal(const View* view) const {
    const ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return 0.0;
    }
    const int column = scrolling->columnOf(view);
    return scrolling->scrollAmountToEnsureVisible(column, scrollViewportExtent());
  }

  void Workspace::applyVisibility() {
    for (View* view : m_views) {
      if (view->pinned()) {
        continue;
      }
      view->setOnActiveWorkspace(m_active);
      // Persistent resting state: an inactive workspace keeps its nodes disabled so the shared scene never renders them
      // on any output. Active (and in-transition) views are enabled + clipped to their home output by
      // syncViewPresentation (arrange / slide), which replaces the old per-render-pass enable/disable.
      if (!m_active && !m_inSwitchTransition) {
        view->setNodeEnabled(false);
      }
    }
  }

  bool Workspace::isSwitchTransitionView(const View* view) const {
    return std::ranges::find(m_switchViews, view) != m_switchViews.end();
  }

  void Workspace::beginSwitchTransition() {
    m_inSwitchTransition = true;
    wlr_box clip{};
    if (m_group != nullptr && m_group->output() != nullptr) {
      wlr_output_layout_get_box(m_group->server()->outputLayout(), m_group->output()->wlr(), &clip);
    }
    const wlr_box* usedClip = clip.width > 0 && clip.height > 0 ? &clip : nullptr;
    if (m_tree != nullptr) {
      wlr_scene_tree_set_clip(m_tree, usedClip);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_tree_set_clip(m_fullscreenTree, usedClip);
    }
    m_switchViews.clear();
    for (View* view : m_views) {
      if (!view->pinned() && view->mapped() && (view->sceneTree()->node.enabled || !m_active)) {
        m_switchViews.push_back(view);
      }
    }
  }

  void Workspace::showSwitchViews() {
    for (View* view : m_switchViews) {
      if (!m_active) {
        view->setNodeEnabled(true);
      }
    }
  }

  void Workspace::setSlideOffset(double y) {
    m_slideOffsetY = static_cast<int>(std::lround(y));
    if (m_tree != nullptr) {
      wlr_scene_node_set_position(&m_tree->node, 0, m_slideOffsetY);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_node_set_position(&m_fullscreenTree->node, 0, m_slideOffsetY);
    }
    for (View* view : m_views) {
      if (!view->pinned() && view->mapped()) {
        syncViewPresentation(view);
      }
    }
  }

  void Workspace::endSwitchTransition() {
    // Put every view back at its resting position while transition visibility is still active: an inactive workspace
    // deliberately skips presentation sync once m_inSwitchTransition is cleared.
    setSlideOffset(0);
    if (m_tree != nullptr) {
      wlr_scene_tree_set_clip(m_tree, nullptr);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_tree_set_clip(m_fullscreenTree, nullptr);
    }
    m_inSwitchTransition = false;
    for (View* view : m_switchViews) {
      view->setFadeAlpha(1.0F);
      if (!m_active) {
        view->setNodeEnabled(false);
      }
    }
    m_switchViews.clear();
    // setSlideOffset() refreshes visibility and clips, but it does not move tiled scene nodes to their
    // authoritative horizontal strip positions. Reconcile after an interrupted switch so a fullscreen column cannot
    // remain off-screen.
    if (m_active) {
      markArrange(false);
    }
  }

  void Workspace::overrideLayoutMode(LayoutMode mode) {
    m_layoutModeOverride = mode;
    ResolvedLayoutConfig copy = m_layoutConfig;
    copy.mode = mode;
    applyLayoutConfig(std::move(copy));
  }

  void Workspace::rename(std::string name, size_t index) {
    if (m_name != name) {
      m_name = std::move(name);
      wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    }
    if (m_index != index) {
      m_index = index;
      const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
      wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    }
  }

  void Workspace::applyLayoutConfig(ResolvedLayoutConfig layoutConfig) {
    m_layoutConfig = std::move(layoutConfig);
    if (m_layout != nullptr && m_layout->mode() == m_layoutConfig.mode) {
      m_layout->setConfig(&m_layoutConfig);
      m_layout->setConstraints(&viewLayoutConstraints);
      markArrange(true);
      return;
    }
    std::vector<View*> tiledViews;
    for (View* view : m_views) {
      if (m_layout != nullptr && m_layout->columnOf(view) >= 0) {
        m_layout->removeView(view);
        tiledViews.push_back(view);
      }
    }
    m_layoutMode = m_layoutConfig.mode;
    m_layout = createLayout(m_layoutMode);
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    for (View* view : tiledViews) {
      m_layout->insertView(view, static_cast<int>(m_layout->columns().size()));
    }
    markArrange();
  }

  WorkspaceGroup::WorkspaceGroup(Server& server, Output& output) : m_server(&server), m_output(&output) {
    m_server->registerAnimatable(this);
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    m_handle = wlr_ext_workspace_group_handle_v1_create(manager, kGroupCaps);
    m_handle->data = this;
    wlr_ext_workspace_group_handle_v1_output_enter(m_handle, m_output->wlr());

    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    auto resolved = resolveWorkspacesForOutput(config(), outputName);
    m_dynamic = resolved.dynamic;
    const size_t count = resolved.workspaces.size();
    m_workspaces.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      m_workspaces.push_back(createConfiguredWorkspace(std::move(resolved.workspaces[i]), i));
    }

    activate(m_workspaces.front().get());
    kLog.info("workspace group for {} with {} workspaces", outputName, count);
  }

  WorkspaceGroup::~WorkspaceGroup() {
    m_server->unregisterAnimatable(this);
    slideFinish();
    m_active = nullptr;
    m_previous = nullptr;
    if (m_handle != nullptr && m_output != nullptr && m_output->wlr() != nullptr) {
      wlr_ext_workspace_group_handle_v1_output_leave(m_handle, m_output->wlr());
    }
    m_workspaces.clear();
    if (m_handle != nullptr) {
      if (m_handle->data == this) {
        m_handle->data = nullptr;
      }
      wlr_ext_workspace_group_handle_v1_destroy(m_handle);
      m_handle = nullptr;
    }
  }
  std::unique_ptr<Workspace> WorkspaceGroup::createConfiguredWorkspace(ResolvedWorkspace workspace, size_t index) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%u", outputName, m_nextHandleSerial++);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    return std::make_unique<Workspace>(
        *this, handle, id, std::move(workspace.name), index, std::move(workspace.layout)
    );
  }

  Workspace* WorkspaceGroup::appendDynamicWorkspace() {
    const size_t index = m_workspaces.size();
    std::string name = std::to_string(index + 1);
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), outputName, name, index);
    auto workspace = createConfiguredWorkspace({std::move(name), std::move(layout)}, index);
    Workspace* result = workspace.get();
    m_workspaces.push_back(std::move(workspace));
    return result;
  }

  void WorkspaceGroup::refreshDynamicWorkspaceMetadata() {
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    for (size_t index = 0; index < m_workspaces.size(); ++index) {
      const std::string name = std::to_string(index + 1);
      ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), outputName, name, index);
      Workspace* workspace = m_workspaces[index].get();
      // Keep a runtime layout switch across structural changes to a dynamic
      // group, while allowing numeric workspace rules to follow the new index.
      if (const std::optional<LayoutMode> overrideMode = workspace->layoutModeOverride()) {
        layout.mode = *overrideMode;
      }
      if (workspace->layoutConfig() != layout) {
        workspace->applyLayoutConfig(std::move(layout));
      }
      if (workspace->name() != name || workspace->index() != index) {
        workspace->rename(name, index);
      }
    }
    if (m_previous == m_active) {
      m_previous = nullptr;
    }
  }

  Workspace* WorkspaceGroup::insertDynamicWorkspace(size_t index) {
    if (!m_dynamic || m_output == nullptr || m_output->wlr() == nullptr) {
      return nullptr;
    }
    index = std::min(index, m_workspaces.size());
    const std::string name = std::to_string(index + 1);
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), outputName, name, index);
    auto workspace = createConfiguredWorkspace({name, std::move(layout)}, index);
    Workspace* result = workspace.get();
    m_workspaces.insert(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index), std::move(workspace));
    refreshDynamicWorkspaceMetadata();
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }
    return result;
  }

  bool WorkspaceGroup::moveActiveWorkspace(int direction) {
    if (m_active == nullptr || m_workspaces.size() < 2 || direction == 0 || m_output == nullptr) {
      return false;
    }
    const size_t index = m_active->index();
    const auto target = static_cast<std::ptrdiff_t>(index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(m_workspaces.size())) {
      return false;
    }
    if (m_dynamic && direction > 0) {
      const bool targetIsTrailingEmpty = static_cast<size_t>(target) == m_workspaces.size() - 1
          && !m_workspaces[static_cast<size_t>(target)]->hasViews();
      if (targetIsTrailingEmpty) {
        return false;
      }
    }
    slideFinish();
    std::swap(m_workspaces[index], m_workspaces[static_cast<size_t>(target)]);
    if (m_dynamic) {
      refreshDynamicWorkspaceMetadata();
      if (m_workspaces.back()->hasViews()) {
        appendDynamicWorkspace();
      }
    } else {
      for (const size_t slot : {index, static_cast<size_t>(target)}) {
        Workspace* moved = m_workspaces[slot].get();
        moved->rename(moved->name(), slot);
      }
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }
    return true;
  }

  void WorkspaceGroup::reconcileInventory() {
    slideFinish();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    auto resolvedSet = resolveWorkspacesForOutput(config(), outputName);
    m_dynamic = resolvedSet.dynamic;

    if (m_dynamic) {
      auto old = std::move(m_workspaces);
      m_workspaces.clear();
      m_workspaces.reserve(old.size() + 1);
      for (auto& workspace : old) {
        if (workspace != nullptr && (workspace->hasViews() || workspace.get() == m_active)) {
          m_workspaces.push_back(std::move(workspace));
        } else if (workspace.get() == m_previous) {
          m_previous = nullptr;
        }
      }
      old.clear();

      if (m_workspaces.empty() || m_workspaces.back()->hasViews()) {
        appendDynamicWorkspace();
      }
      for (size_t index = 0; index < m_workspaces.size(); ++index) {
        m_workspaces[index]->rename(std::to_string(index + 1), index);
      }
      if (m_previous == m_active) {
        m_previous = nullptr;
      }
      kLog.info("reconciled {} to {} workspaces (0 windows relocated)", outputName, m_workspaces.size());
      return;
    }

    auto& resolved = resolvedSet.workspaces;
    auto old = std::move(m_workspaces);
    std::vector<std::unique_ptr<Workspace>> next(resolved.size());

    // Preserve workspace identity by name before using position as a fallback.
    for (size_t i = 0; i < resolved.size(); ++i) {
      const auto match = std::ranges::find_if(old, [&](const auto& workspace) {
        return workspace != nullptr && workspace->name() == resolved[i].name;
      });
      if (match != old.end()) {
        next[i] = std::move(*match);
      }
    }
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (next[i] == nullptr && i < old.size() && old[i] != nullptr) {
        next[i] = std::move(old[i]);
      }
      if (next[i] != nullptr) {
        next[i]->rename(resolved[i].name, i);
      } else {
        next[i] = createConfiguredWorkspace(std::move(resolved[i]), i);
      }
    }

    const auto survives = [&](const Workspace* workspace) {
      return workspace != nullptr
          && std::ranges::any_of(next, [&](const auto& candidate) { return candidate.get() == workspace; });
    };
    const bool activeSurvives = survives(m_active);
    const bool previousSurvives = survives(m_previous);
    Workspace* replacementActive = activeSurvives ? m_active : nullptr;
    size_t relocatedViews = 0;

    m_workspaces = std::move(next);
    for (const auto& removed : old) {
      if (removed == nullptr) {
        continue;
      }
      Workspace* fallback = m_workspaces[std::min(removed->index(), m_workspaces.size() - 1)].get();
      if (removed.get() == m_active) {
        removed->setActive(false);
        replacementActive = fallback;
      }
      for (View* view : removed->allViews()) {
        view->setWorkspace(fallback);
        ++relocatedViews;
      }
    }

    if (!previousSurvives) {
      m_previous = nullptr;
    }
    if (!activeSurvives) {
      m_active = replacementActive;
      m_active->setActive(true);
    }
    if (m_previous == m_active) {
      m_previous = nullptr;
    }
    old.clear();

    if (relocatedViews > 0 || !activeSurvives) {
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
    }
    kLog.info("reconciled {} to {} workspaces ({} windows relocated)", outputName, m_workspaces.size(), relocatedViews);
  }

  void WorkspaceGroup::refreshLayouts() {
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    for (const auto& workspace : m_workspaces) {
      // A config reload reasserts the configured mode, dropping any runtime
      // workspace-set-layout override.
      workspace->clearLayoutModeOverride();
      ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), outputName, workspace->name(), workspace->index());
      if (workspace->layoutConfig() != layout) {
        workspace->applyLayoutConfig(std::move(layout));
      }
    }
  }

  void WorkspaceGroup::flushArrange() {
    // Indexed, and the bound re-read every step: arrange() reaches the overview and the view animations, and a
    // workspace list that grows or shrinks under an iterator would be a use-after-free rather than a missed arrange.
    for (size_t index = 0; index < m_workspaces.size(); ++index) { // NOLINT(modernize-loop-convert)
      m_workspaces[index]->flushArrange();
    }
  }

  void WorkspaceGroup::reconcileDynamic() {
    if (!m_dynamic || slideActive()) {
      return;
    }

    // Dynamic groups keep exactly one empty workspace. Prefer an empty active workspace so closing its last window does
    // not destroy the workspace the user is currently viewing; otherwise retain the last existing empty one to avoid
    // replacing its protocol identity on every reconciliation.
    Workspace* emptyKeeper = m_active != nullptr && !m_active->hasViews() ? m_active : nullptr;
    if (emptyKeeper == nullptr && !m_workspaces.empty() && !m_workspaces.back()->hasViews()) {
      emptyKeeper = m_workspaces.back().get();
    }
    for (size_t index = m_workspaces.size(); index-- > 0;) {
      Workspace* workspace = m_workspaces[index].get();
      if (!workspace->hasViews() && workspace != emptyKeeper) {
        if (m_previous == workspace) {
          m_previous = nullptr;
        }
        m_workspaces.erase(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index));
      }
    }
    if (emptyKeeper == nullptr) {
      appendDynamicWorkspace();
    }

    refreshDynamicWorkspaceMetadata();
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }
  }

  Workspace* WorkspaceGroup::workspaceAt(size_t index) const {
    if (index >= m_workspaces.size()) {
      return nullptr;
    }
    return m_workspaces[index].get();
  }

  Workspace* WorkspaceGroup::workspaceAtClamped(size_t index) const {
    Workspace* workspace = workspaceAt(index);
    if (workspace == nullptr && m_dynamic && !m_workspaces.empty()) {
      return m_workspaces.back().get();
    }
    return workspace;
  }

  Workspace* WorkspaceGroup::workspaceNamed(std::string_view name) const {
    for (const auto& entry : m_workspaces) {
      if (entry->name() == name) {
        return entry.get();
      }
    }
    return nullptr;
  }

  Workspace* WorkspaceGroup::workspaceForSelector(std::string_view name) const {
    if (Workspace* workspace = workspaceNamed(name)) {
      return workspace;
    }
    if (name.empty() || !std::ranges::all_of(name, [](char value) { return value >= '0' && value <= '9'; })) {
      return nullptr;
    }
    size_t index = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), index);
    if (error != std::errc{} || end != name.data() + name.size() || index < 1 || m_workspaces.empty()) {
      return nullptr;
    }
    if (!m_dynamic) {
      return index <= m_workspaces.size() ? m_workspaces[index - 1].get() : nullptr;
    }
    return m_workspaces.back().get();
  }

  Workspace* WorkspaceGroup::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    for (const auto& entry : m_workspaces) {
      if (entry->handle() == handle) {
        return entry.get();
      }
    }
    return nullptr;
  }

  void WorkspaceGroup::slideFinish() {
    m_slideAnim.snap(0.0);
    if (m_slide.base != nullptr) {
      m_slide.base->endSwitchTransition();
    }
    if (m_slide.up != nullptr) {
      m_slide.up->endSwitchTransition();
    }
    if (m_slide.down != nullptr) {
      m_slide.down->endSwitchTransition();
    }
    if (m_active != nullptr && m_active->switchTransitionActive()) {
      m_active->endSwitchTransition();
    }
    m_slide = {};
  }

  bool WorkspaceGroup::slideBegin(bool includePrev, bool includeNext) {
    if (m_active == nullptr) {
      return false;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_output->wlr(), &box);
    if (box.height <= 0) {
      return false;
    }
    slideFinish();
    m_slide.base = m_active;
    m_slide.height = box.height;
    m_slide.progress = 0;
    const size_t idx = m_active->index();
    m_slide.up = (includePrev && idx > 0) ? workspaceAt(idx - 1) : nullptr;
    m_slide.down = includeNext ? workspaceAt(idx + 1) : nullptr;
    m_slide.base->beginSwitchTransition();
    if (m_slide.up != nullptr) {
      m_slide.up->beginSwitchTransition();
      m_slide.up->showSwitchViews();
      m_slide.up->arrange(false);
    }
    if (m_slide.down != nullptr) {
      m_slide.down->beginSwitchTransition();
      m_slide.down->showSwitchViews();
      m_slide.down->arrange(false);
    }
    slideApply(0.0);
    return true;
  }

  void WorkspaceGroup::slideApply(double progress) {
    m_slide.progress = progress;
    const double h = m_slide.height;
    m_slide.base->setSlideOffset(-progress * h);
    if (m_slide.down != nullptr) {
      m_slide.down->setSlideOffset((1.0 - progress) * h);
    }
    if (m_slide.up != nullptr) {
      m_slide.up->setSlideOffset((-1.0 - progress) * h);
    }
    wlr_output_schedule_frame(m_output->wlr());
  }

  void WorkspaceGroup::slideSettle(int delta) {
    Workspace* target = nullptr;
    if (delta < 0) {
      target = m_slide.up;
    } else if (delta > 0) {
      target = m_slide.down;
    }
    if (target == nullptr) {
      delta = 0;
      target = m_slide.base;
    }
    if (target != m_active) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = target;
      m_active->setActive(true);
      if (m_previous != nullptr) {
        m_previous->showSwitchViews();
      }
      Workspace* unused = (delta > 0) ? m_slide.up : m_slide.down;
      if (unused != nullptr) {
        unused->endSwitchTransition();
      }
    }
    kLog.debug("slide workspace {} → {} on {}", m_slide.base->name(), target->name(), m_output->wlr()->name);
    m_slideAnim.snap(m_slide.progress);
    m_slideAnim.retarget(static_cast<double>(delta), config().appearance.animationMs, Easing::EaseOutCubic);
    wlr_output_schedule_frame(m_output->wlr());
  }

  bool WorkspaceGroup::tickAnimations(uint64_t nowMsec) {
    if (!m_slideAnim.tick(nowMsec)) {
      return false;
    }
    slideApply(m_slideAnim.current());
    if (!m_slideAnim.animating()) {
      slideFinish();
      reconcileDynamic();
      return false;
    }
    return true;
  }

  void WorkspaceGroup::activate(Workspace* workspace, bool animate) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    if (m_active == workspace) {
      return;
    }
    slideFinish();
    Overview* overview = m_server->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    // The real trees are hidden while overview runs: the filmstrip scroll is
    // the transition, so never start a slide underneath it.
    const bool doAnimate = animate && m_active != nullptr && !m_server->sessionLocked() && !overviewActive;
    if (!doAnimate) {
      if (m_active != nullptr) {
        m_previous = m_active;
        m_active->setActive(false);
      }
      m_active = workspace;
      m_active->setActive(true);
      kLog.debug("activate workspace {} on {}", m_active->name(), m_output->wlr()->name);
      if (overviewActive) {
        overview->onWorkspaceActivated(this);
      }
      reconcileDynamic();
      return;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_output->wlr(), &box);
    if (box.height <= 0) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = workspace;
      m_active->setActive(true);
      reconcileDynamic();
      return;
    }
    const int sign = workspace->index() > m_active->index() ? 1 : -1;
    m_slide.base = m_active;
    m_slide.height = box.height;
    m_slide.progress = 0;
    if (sign > 0) {
      m_slide.down = workspace;
    } else {
      m_slide.up = workspace;
    }
    m_slide.base->beginSwitchTransition();
    workspace->beginSwitchTransition();
    slideApply(0.0);
    slideSettle(sign);
  }

  void WorkspaceGroup::select(Workspace* workspace) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    Workspace* selected = workspace;
    if (m_active == workspace && config().workspaces.backAndForth && m_previous != nullptr && m_previous != m_active) {
      selected = m_previous;
    }
    activate(selected);
    m_server->cursor()->clearConstraint();
    m_server->refocus(m_output);
  }

  void WorkspaceGroup::deactivate(Workspace* workspace) {
    if (workspace == nullptr || m_active != workspace) {
      return;
    }
    Workspace* fallback = workspaceAt(0);
    if (fallback == workspace) {
      fallback = workspaceAt(1);
    }
    if (fallback != nullptr) {
      activate(fallback, false);
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
      return;
    }
    m_active->setActive(false);
    m_active = nullptr;
    m_server->cursor()->clearConstraint();
    m_server->refocus(m_output);
  }

  Workspace* WorkspaceGroup::createWorkspace(const char* name) {
    if (m_dynamic) {
      kLog.debug("using trailing dynamic workspace for create request on {}", m_output->wlr()->name);
      return m_workspaces.empty() ? appendDynamicWorkspace() : m_workspaces.back().get();
    }

    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const size_t index = m_workspaces.size();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%u", outputName, m_nextHandleSerial++);
    std::string wsName = (name != nullptr && name[0] != '\0') ? name : std::to_string(index + 1);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    m_workspaces.push_back(
        std::make_unique<Workspace>(*this, handle, id, std::move(wsName), index, resolveGlobalLayout(config()))
    );
    return m_workspaces.back().get();
  }

} // namespace umbriel
