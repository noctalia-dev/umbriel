#include "workspace/workspace.h"

#include "config/config.h"
#include "config/resolve.h"
#include "config/store.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/dwindle.h"
#include "layout/master.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "server/server.h"
#include "view/floating.h"
#include "view/registry.h"
#include "view/size_hints.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>
#include "wlr.h"
// clang-format on

extern "C" {
#include <umbrielfx/render/effect.h>
}

namespace umbriel {

  namespace {
    constexpr Logger kLog("workspace");

    constexpr uint32_t kWorkspaceCaps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE
        | EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

    constexpr uint32_t kGroupCaps = EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE;

    // The bridge between the layout's opaque View identity and the client state it needs to size that view. Workspace
    // owns both sides, so it owns the lookup; layout/ stays free of view/ and its geometry stays testable.
    LayoutConstraints viewLayoutConstraints(const View* view) {
      const SizeHints hints = view != nullptr ? view->sizeHints() : SizeHints{};
      const int titleBar = view != nullptr ? view->titleBarHeight() : 0;
      return {
          .minWidth = hints.minWidth,
          .minHeight = hints.minHeight + titleBar,
          .maxWidth = hints.maxWidth,
          .maxHeight = hints.maxHeight > 0 ? hints.maxHeight + titleBar : 0,
          .fullscreen = view != nullptr && view->layoutFullscreen(),
          .maximizedToEdges = view != nullptr && view->maximizedToEdges(),
      };
    }

    struct NamedScrollingColumnPlacement {
      size_t column = 0;
      int row = 0;
    };

    std::optional<NamedScrollingColumnPlacement> namedScrollingColumnPlacement(
        const ScrollingLayout& layout, const View* joining, std::string_view name, std::optional<int> order
    ) {
      if (name.empty()) {
        return std::nullopt;
      }
      const auto& columns = layout.columns();
      for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
        const Column& column = columns[columnIndex];
        const auto anchor = std::ranges::find_if(column.views, [&](const View* existing) {
          return existing != joining
              && existing->namedScrollingColumnName()
              && *existing->namedScrollingColumnName() == name;
        });
        if (anchor == column.views.end()) {
          continue;
        }

        const auto before = order
            ? std::ranges::find_if(
                  column.views,
                  [&](const View* existing) {
                    const std::optional<int> existingOrder = existing->namedScrollingColumnOrder();
                    return existing != joining
                        && existing->namedScrollingColumnName()
                        && *existing->namedScrollingColumnName() == name
                        && (!existingOrder || *existingOrder > *order);
                  }
              )
            : column.views.end();
        return NamedScrollingColumnPlacement{
            .column = columnIndex,
            .row = static_cast<int>(std::distance(column.views.begin(), before)),
        };
      }
      return std::nullopt;
    }

    // Ordered members join the tab group at their insertion row, including either boundary.
    void joinNamedScrollingColumn(ScrollingLayout& layout, View* view, const NamedScrollingColumnPlacement& placement) {
      const auto column = static_cast<int>(placement.column);
      const Column& lane = layout.columns()[placement.column];
      const auto row = static_cast<size_t>(placement.row);
      const TabGroup* group = lane.tabs.groupAt(row);
      if (group == nullptr && row > 0) {
        group = lane.tabs.groupAt(row - 1);
      }
      const View* member = group != nullptr ? lane.views[group->active] : nullptr;
      if (member == nullptr || !layout.insertTab(view, column, placement.row, member)) {
        layout.insertViewIntoColumn(view, column, placement.row);
      }
    }

    // Whether a window shares its workspace with the others, which is what `match.is_only_window` measures. A pinned
    // window floats above the workspace rather than on it, and a scratchpad window has already left it, so neither
    // counts as company.
    bool sharesWorkspace(const View* view) { return view != nullptr && view->mapped() && !view->pinned(); }

    ResolvedLayoutConfig displacedWorkspaceLayout(const DisplacedWorkspace& origin) {
      ResolvedLayoutConfig layout = origin.workspaceNamed
          ? resolveWorkspaceLayout(config(), origin.identity(), origin.workspaceName, origin.workspaceIndex)
          : resolveUnnamedWorkspaceLayout(config(), origin.identity(), origin.workspaceIndex);
      if (origin.configGeneration == configStore().generation() && origin.layoutModeOverride) {
        layout.mode = *origin.layoutModeOverride;
      }
      return layout;
    }

    ResolvedLayoutConfig workspaceLayout(const Workspace& workspace, size_t index) {
      if (const auto& origin = workspace.displacedWorkspace()) {
        return displacedWorkspaceLayout(*origin);
      }
      const OutputIdentity identity = workspace.group()->output()->identity();
      return workspace.named() ? resolveWorkspaceLayout(config(), identity, workspace.name(), index)
                               : resolveUnnamedWorkspaceLayout(config(), identity, index);
    }
  } // namespace

  Workspace::Workspace(
      WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string id, std::string name, size_t index,
      bool named, ResolvedLayoutConfig layoutConfig
  )
      : m_group(&group), m_handle(handle), m_id(std::move(id)), m_name(std::move(name)), m_index(index), m_named(named),
        m_layout(createLayout(layoutConfig.mode)), m_layoutConfig(std::move(layoutConfig)),
        m_layoutMode(m_layoutConfig.mode) {
    m_launchAnchor->workspace = this;
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    m_handle->data = this;
    wlr_ext_workspace_handle_v1_set_group(m_handle, m_group->handle());
    wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
    wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    m_tree = wlr_scene_tree_create(m_group->output()->viewRoot());
    // Focus raises only within a layer: floating views can never fall below tiles. Tiles lend their shadows to the
    // layer under all of them, so a tile never shadows the tile beside it; every other window keeps its own.
    m_tileShadowLayer = wlr_scene_tree_create(m_tree);
    m_tiledLayer = wlr_scene_tree_create(m_tree);
    m_floatingLayer = wlr_scene_tree_create(m_tree);
    m_fullscreenTree = wlr_scene_tree_create(m_group->output()->fullscreenRoot());
  }

  Workspace::~Workspace() {
    m_launchAnchor->workspace = nullptr;
    // Windows leaving with the workspace are no longer anyone's tabs.
    m_tabs.releaseAll();
    discardCloseSnapshots();
    endLayoutMotion();
    for (View* view : m_views) {
      view->cancelPositionAnimation();
      const bool fs = view->currentFullscreen() || view->scheduledFullscreen();
      view->setSceneParent(fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree());
      view->detachWorkspace();
    }
    m_views.clear();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_tileShadowLayer = nullptr;
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

  void Workspace::invalidateLaunchReservations() {
    m_launchAnchor->workspace = nullptr;
    m_launchAnchor = std::make_shared<WorkspaceLaunchAnchor>();
    m_launchAnchor->workspace = this;
  }

  ScrollingLayout* Workspace::scrollingLayout() {
    return m_layoutMode == LayoutMode::Scrolling ? static_cast<ScrollingLayout*>(m_layout.get()) : nullptr;
  }

  DwindleLayout* Workspace::dwindleLayout() {
    return m_layoutMode == LayoutMode::Dwindle ? static_cast<DwindleLayout*>(m_layout.get()) : nullptr;
  }

  MasterStackLayout* Workspace::masterLayout() {
    return m_layoutMode == LayoutMode::Master ? dynamic_cast<MasterStackLayout*>(m_layout.get()) : nullptr;
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
    syncCloseSnapshots();
    if (active) {
      markArrange(false);
      m_group->output()->updateVrr();
      m_group->output()->updateHdr();
    }
    m_group->server()->scheduleIpcWorkspacesEvent();
  }

  void Workspace::updateUrgent() {
    const bool urgent = std::ranges::any_of(m_views, [](const View* view) { return view->urgent(); });
    wlr_ext_workspace_handle_v1_set_urgent(m_handle, urgent);
    m_group->server()->scheduleIpcWorkspacesEvent();
  }

  void Workspace::setFocusedView(View* view, bool raise, bool settleLayers) {
    if (view == nullptr || view->workspace() == this) {
      if (m_focusedView != view) {
        m_focusedView = view;
        m_group->server()->scheduleIpcWorkspacesEvent();
      }
      if (view == nullptr) {
        m_floatingLayerActive = false;
      } else if (!view->pinned()) {
        if (view->floating() && !view->scheduledFullscreen()) {
          m_floatingLayerActive = true;
        } else if (settleLayers) {
          m_floatingLayerActive = false;
        }
      }
      if (view != nullptr && view->floating() && raise) {
        std::erase(m_floatingStack, view);
        m_floatingStack.push_back(view);
      }
      if (raise) {
        restackFloatingViews();
      } else {
        // Focus without raise must not reorder the scene, but the floating layer still follows the layer state
        // above, or a hover would hide floats a fullscreen window is sitting under.
        syncFloatingLayer();
        raiseFloatingLayer();
      }
      // Whatever focused a tab, a click on its bar, a keybind, activation, or IPC, puts that tab on show.
      m_tabs.focusChanged(view);
    }
  }

  void Workspace::syncFloatingStack(View* view) {
    if (view == nullptr || view->workspace() != this) {
      return;
    }
    if (view == m_focusedView && !view->pinned()) {
      m_floatingLayerActive = view->floating() && !view->scheduledFullscreen();
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

  bool Workspace::floatingLayerRaised() const {
    return m_floatingLayer != nullptr && m_floatingLayer->node.parent == m_fullscreenTree;
  }

  void Workspace::syncFloatingLayer() {
    if (m_floatingLayerActive && std::ranges::none_of(m_views, [](const View* view) {
          return view->mapped() && view->floating() && !view->pinned() && !view->scheduledFullscreen();
        })) {
      // A hover may leave the floating layer raised after tiled focus returns. Once it empties, a later arrival
      // must establish floating focus itself rather than inherit the previous floating episode.
      m_floatingLayerActive = false;
    }
    const bool fullscreen = std::ranges::any_of(m_views, [](const View* view) {
      return view->mapped() && !view->pinned() && view->scheduledFullscreen();
    });
    wlr_scene_tree* target = m_floatingLayerActive && fullscreen ? m_fullscreenTree : m_tree;
    if (m_floatingLayer->node.parent != target) {
      // Both workspace roots carry the same slide offset and output clip.
      wlr_scene_node_reparent(&m_floatingLayer->node, target);
    }
  }

  void Workspace::raiseFloatingLayer() {
    if (floatingLayerRaised()) {
      wlr_scene_node_raise_to_top(&m_floatingLayer->node);
    }
  }

  void Workspace::restackFloatingViews() {
    syncFloatingLayer();
    for (View* view : m_floatingStack) {
      if (view != nullptr && view->workspace() == this && view->floating()) {
        view->raiseToTop();
      }
    }
    raiseFloatingLayer();
  }

  void Workspace::addView(View* view, bool attachToLayout, LayoutAttachOrigin origin) {
    if (view == nullptr || std::ranges::find(m_views, view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    // Occupancy and the window count are part of the IPC workspace listing, so every membership change pushes an event,
    // even on a static output, where reconciliation never adds or removes a workspace.
    m_group->server()->scheduleIpcWorkspacesEvent();
    updateUrgent();
    const bool fs = view->currentFullscreen() || view->scheduledFullscreen();
    if (view->pinned()) {
      // Cross-output moves have to rehome the pinned view onto the new output's clipped roots.
      view->restorePinnedSceneParent();
    } else {
      view->setSceneParent(fs ? m_fullscreenTree : viewLayer(view->tiled()));
    }
    syncFloatingStack(view);
    applyVisibility();
    if (attachToLayout || m_layoutMode == LayoutMode::Floating) {
      layoutAttach(view, std::nullopt, std::nullopt, origin);
    }
    if (attachToLayout
        && m_layoutMode != LayoutMode::Floating
        && view->layoutFloated()
        && view->floating()
        && !view->pinned()) {
      view->setFloating(false, false);
    }
    // A float joins no layout, so nothing below would mark the workspace for a pass; occupancy questions need one.
    markArrange();
    m_group->reconcileDynamic();
  }

  View* Workspace::removeView(View* view, bool reconcile) {
    if (view == nullptr) {
      return nullptr;
    }
    if (!view->pinned()) {
      const bool fs = view->currentFullscreen() || view->scheduledFullscreen();
      view->setSceneParent(fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree());
    }
    const bool focusedHere = m_focusedView == view;
    View* replacement = focusedHere ? focusReplacementForRemoval(view) : nullptr;
    if (focusedHere) {
      // detachFromLayout() judges the survivor once the column is gone, so the note and the new focus land first.
      noteRemovalOfFocusedColumn(m_layout->columnOf(view));
      m_focusedView = replacement;
      m_group->server()->scheduleIpcWorkspacesEvent();
    }
    detachFromLayout(view);
    if (std::erase(m_views, view) > 0) {
      m_group->server()->scheduleIpcWorkspacesEvent();
    }
    if (view == m_lastAloneSoleView) {
      // The window we remembered as the only one is gone: forget it, so another window that replaces it is still
      // noticed as new.
      m_lastAloneSoleView = nullptr;
    }
    if (view == m_lastOnlyWindowView) {
      m_lastOnlyWindowView = nullptr;
    }
    forgetShowDesktopFocus(view);
    updateUrgent();
    std::erase(m_floatingStack, view);
    std::erase(m_switchViews, view);
    if (focusedHere) {
      setFocusedView(replacement);
    } else {
      restackFloatingViews();
    }

    // Re-anchor the strip on whatever is focused now, the way every other focus-moving operation does. Fitting the
    // survivor prevents the old scroll offset from leaving it cut off at the left edge while empty space opens on the
    // right.
    ensureFocusedVisible();
    markArrange();
    if (reconcile) {
      m_group->reconcileDynamic();
    }
    return replacement;
  }

  int Workspace::layoutAttachIndex(const View* view) const {
    int focusedColumn = m_layout->columnOf(m_focusedView);
    if (focusedColumn < 0 && m_group != nullptr) {
      for (const auto& entry : m_group->server()->registry().placementRoots()) {
        View* candidate = entry.get();
        if (candidate != view && candidate->mapped() && candidate->tiled() && candidate->workspace() == this) {
          focusedColumn = m_layout->columnOf(candidate);
          if (focusedColumn >= 0) {
            break;
          }
        }
      }
    }
    return focusedColumn >= 0 ? focusedColumn + 1 : static_cast<int>(m_layout->columns().size());
  }

  bool Workspace::isOnlyTiledView(const View* view) const {
    for (const Column& column : m_layout->columns()) {
      for (View* member : column.views) {
        if (member != view) {
          return false;
        }
      }
    }
    return true;
  }

  bool Workspace::isOnlyVisibleView(const View* view) const {
    // A window that does not count as company cannot be the only one either.
    if (!sharesWorkspace(view)) {
      return false;
    }
    for (View* other : m_views) {
      if (other != view && sharesWorkspace(other)) {
        return false;
      }
    }
    return true;
  }

  void Workspace::exitFullscreenForIncomingView(const View* joining) {
    const FullscreenExitScope scope = m_layoutConfig.newExitsFullscreen;
    // The scope picks which kinds of arriving window are allowed to displace a fullscreen one; a joining window is
    // classified the way it will live on the workspace (pinned wins over floating over tiled).
    const auto joiningBit = static_cast<uint8_t>(
        joining->pinned()      ? FullscreenExitScope::Pinned
            : joining->tiled() ? FullscreenExitScope::Tiled
                               : FullscreenExitScope::Floating
    );
    // A tiled arrival on a scrolling workspace opens beside the fullscreen column and the strip scrolls to it.
    if ((static_cast<uint8_t>(scope) & joiningBit) == 0
        || (m_layoutMode == LayoutMode::Scrolling && joiningBit == static_cast<uint8_t>(FullscreenExitScope::Tiled))) {
      return;
    }
    for (View* other : m_views) {
      if (other != joining && other->layoutFullscreen()) {
        other->setFullscreen(false);
      }
    }
  }

  void Workspace::layoutAttach(
      View* view, std::optional<double> initialExtent, std::optional<int> initialExtentPx, LayoutAttachOrigin origin
  ) {
    if (view == nullptr || !view->mapped() || !view->tiled() || m_layout->columnOf(view) >= 0) {
      return;
    }
    if (m_layoutMode == LayoutMode::Floating) {
      if (origin == LayoutAttachOrigin::OpeningView) {
        view->adoptLayoutFloat();
      } else {
        view->floatForLayout();
      }
      if (origin == LayoutAttachOrigin::OpeningView || origin == LayoutAttachOrigin::MovedView) {
        exitFullscreenForIncomingView(view);
      }
      return;
    }
    if (origin == LayoutAttachOrigin::OpeningView || origin == LayoutAttachOrigin::MovedView) {
      exitFullscreenForIncomingView(view);
    }
    ScrollingLayout* scrolling = scrollingLayout();
    const std::optional<std::string>& name = view->namedScrollingColumnName();
    const std::optional<NamedScrollingColumnPlacement> placement = scrolling != nullptr && name
        ? namedScrollingColumnPlacement(*scrolling, view, *name, view->namedScrollingColumnOrder())
        : std::nullopt;
    view->m_ownsNamedScrollingColumnExtent = scrolling != nullptr && name.has_value() && !placement;
    if (placement) {
      joinNamedScrollingColumn(*scrolling, view, *placement);
    } else if (DwindleLayout* dwindle = dwindleLayout(); dwindle != nullptr && m_layoutConfig.dwindle.newTowardCursor) {
      // The pointer only means something here while it is over this workspace as shown on its output.
      Server* server = m_group != nullptr ? m_group->server() : nullptr;
      Output* output = m_group != nullptr ? m_group->output() : nullptr;
      const Overview* overview = server != nullptr ? server->overview() : nullptr;
      const Cursor* cursor = server != nullptr ? server->cursor() : nullptr;
      const wlr_cursor* pointer = cursor != nullptr ? cursor->wlr() : nullptr;
      if (m_active
          && output != nullptr
          && pointer != nullptr
          && (overview == nullptr || !overview->active())
          && wlr_output_layout_output_at(server->outputLayout(), pointer->x, pointer->y) == output->wlr()) {
        dwindle->insertViewToward(view, layoutAttachIndex(view), pointer->x, pointer->y);
      } else {
        dwindle->insertView(view, layoutAttachIndex(view));
      }
    } else {
      m_layout->insertView(view, layoutAttachIndex(view));
    }
    // An opening window's rules can set how the column it opens in shows its windows.
    if (TabbedContainers* containers = m_layout->tabbedContainers();
        containers != nullptr && origin == LayoutAttachOrigin::OpeningView && view->m_ruleColumnDisplay) {
      containers->setTabbed(view, *view->m_ruleColumnDisplay == ColumnDisplay::Tabbed);
    }

    if (scrolling != nullptr && !placement) {
      const int column = scrolling->columnOf(view);
      if (initialExtentPx) {
        scrolling->setWidthFromPixels(column, scrollViewportExtent(), *initialExtentPx);
      } else if (initialExtent) {
        scrolling->setWidthFraction(column, *initialExtent);
      } else if (m_layoutConfig.scrolling.defaultExtentFraction) {
        scrolling->setWidthFraction(column, *m_layoutConfig.scrolling.defaultExtentFraction);
      } else {
        const wlr_box& geometry = view->geometryBox();
        const int primary = scrollingVertical() ? geometry.height : geometry.width;
        if (primary > 0) {
          scrolling->setWidthFromPixels(column, scrollViewportExtent(), primary);
        }
      }
    }
    markArrange(true);
  }

  std::unique_ptr<Layout> Workspace::previewLayout() const {
    LayoutCapture capture = m_layout->captureState();
    std::unique_ptr<Layout> preview = createLayout(m_layout->mode());
    preview->setConfig(&m_layoutConfig);
    preview->setConstraints(&viewLayoutConstraints);
    if (capture.snapshot == nullptr || !preview->restoreState(*capture.snapshot, capture.members)) {
      kLog.error("failed to restore layout preview");
      return nullptr;
    }
    return preview;
  }

  Layout::InitialSize Workspace::initialMaximizedSize(View* view, const wlr_box& usable) const {
    std::unique_ptr<Layout> preview = previewLayout();
    if (preview == nullptr) {
      return m_layout->initialSize(usable, true, std::nullopt, std::nullopt, m_focusedView);
    }
    preview->insertView(view, layoutAttachIndex(view));
    const int column = preview->columnOf(view);
    if (column >= 0 && !preview->isFullWidth(column)) {
      preview->toggleFullWidth(column);
    }
    preview->arrange(usable);
    const wlr_box target = preview->targetBox(view);
    return {.width = target.width, .height = target.height};
  }

  std::optional<Layout::InitialSize> Workspace::initialNamedScrollingColumnSize(
      View* view, const wlr_box& usable, std::string_view group, std::optional<int> order, bool maximized
  ) const {
    const ScrollingLayout* scrolling = scrollingLayout();
    if (view == nullptr || scrolling == nullptr) {
      return std::nullopt;
    }
    const std::optional<NamedScrollingColumnPlacement> placement =
        namedScrollingColumnPlacement(*scrolling, view, group, order);
    if (!placement) {
      return std::nullopt;
    }

    std::unique_ptr<Layout> basePreview = previewLayout();
    auto* preview = dynamic_cast<ScrollingLayout*>(basePreview.get());
    if (preview == nullptr) {
      return std::nullopt;
    }
    if (placement->column >= preview->columns().size()) {
      return std::nullopt;
    }
    const int column = static_cast<int>(placement->column);
    joinNamedScrollingColumn(*preview, view, *placement);
    if (maximized && !preview->isFullWidth(column)) {
      preview->toggleFullWidth(column);
    }
    preview->arrange(usable);
    const wlr_box target = preview->targetBox(view);
    return Layout::InitialSize{.width = target.width, .height = target.height};
  }

  void Workspace::applyNamedScrollingColumnRule(
      View* view, std::optional<double> initialExtent, std::optional<int> initialExtentPx,
      NamedScrollingColumnChange change
  ) {
    ScrollingLayout* scrolling = scrollingLayout();
    if (view == nullptr
        || !view->mapped()
        || !view->tiled()
        || scrolling == nullptr
        || !view->namedScrollingColumnName()) {
      return;
    }
    const std::string& name = *view->namedScrollingColumnName();
    const auto restoreMaximizedColumn = [&] {
      const int column = scrolling->columnOf(view);
      if (column >= 0 && view->scheduledMaximized() && !view->maximizedToEdges() && !scrolling->isFullWidth(column)) {
        scrolling->toggleFullWidth(column);
      }
    };
    std::optional<NamedScrollingColumnPlacement> placement =
        namedScrollingColumnPlacement(*scrolling, view, name, view->namedScrollingColumnOrder());
    switch (change) {
    case NamedScrollingColumnChange::Name:
      view->m_ownsNamedScrollingColumnExtent = !placement;
      break;
    case NamedScrollingColumnChange::Order:
      if (placement && static_cast<int>(placement->column) != scrolling->columnOf(view)) {
        return;
      }
      break;
    }
    if (!placement) {
      if (change == NamedScrollingColumnChange::Order) {
        return;
      }
      const int previousColumn = scrolling->columnOf(view);
      if (previousColumn < 0 || scrolling->columns()[static_cast<size_t>(previousColumn)].views.size() == 1) {
        return;
      }

      // A late identity change can move a member from an established group to
      // a new one. Start that group in its own adjacent column.
      detachFromLayout(view);
      scrolling->insertView(view, previousColumn + 1);
      if (initialExtentPx) {
        scrolling->setWidthFromPixels(scrolling->columnOf(view), scrollViewportExtent(), *initialExtentPx);
      } else if (initialExtent) {
        scrolling->setWidthFraction(scrolling->columnOf(view), *initialExtent);
      }
      restoreMaximizedColumn();
      clampScrollToRange();
      ensureFocusedVisible();
      markArrange(true);
      return;
    }

    const int previousColumn = scrolling->columnOf(view);
    detachFromLayout(view);
    placement = namedScrollingColumnPlacement(*scrolling, view, name, view->namedScrollingColumnOrder());
    if (!placement) {
      // The earlier lookup found another member. Preserve a valid layout if a
      // future detach path ever removes more than the joining view.
      scrolling->insertView(view, std::clamp(previousColumn, 0, static_cast<int>(scrolling->columns().size())));
      kLog.error("named scrolling column '{}' disappeared while moving a view", name);
    } else {
      joinNamedScrollingColumn(*scrolling, view, *placement);
    }
    restoreMaximizedColumn();
    clampScrollToRange();
    ensureFocusedVisible();
    markArrange(true);
  }

  void Workspace::layoutDetach(View* view, bool animate) {
    detachFromLayout(view);
    // Removing the column can leave the offset past the end of the strip; clamp it now. Not done in arrange(), since a
    // touchpad swipe overscrolls on purpose and arranges every frame.
    clampScrollToRange();
    markArrange(animate);
  }

  int Workspace::scrollViewportExtent() const {
    const wlr_box usable = tiledArea();
    const int extent = scrollingVertical() ? usable.height : usable.width;
    return std::max(1, extent - 2 * m_layoutConfig.edgePad);
  }

  wlr_box Workspace::usableArea() const {
    if (m_group == nullptr || m_group->output() == nullptr) {
      return {};
    }
    Output* output = m_group->output();
    wlr_box area = output->usableArea();
    if (area.width <= 0 || area.height <= 0) {
      area = output->layoutBox();
    }
    return area;
  }

  wlr_box Workspace::tiledArea() const { return applyLayoutStruts(usableArea(), m_layoutConfig.struts); }

  wlr_box Workspace::presentedTiledBox(const View* view, const Layout* layout) const {
    return tiledTargetBox(view, usableArea(), layout);
  }

  void Workspace::detachFromLayout(View* view) {
    ScrollingLayout* scrolling = scrollingLayout();
    // Measured before the removal, because it depends on where the column is.
    const double shift = scrolling != nullptr
        ? scrolling->scrollShiftForColumnRemoval(scrolling->columnOf(view), scrollViewportExtent())
        : 0.0;
    m_layout->removeView(view);
    // Outside the layout a window is never a tab; whatever presents it next finds it showing.
    m_tabs.released(view);
    releaseLayoutMotion(view);
    view->endLayoutMotion();
    if (scrolling != nullptr && shift != 0.0) {
      scrolling->setScroll(scrolling->scroll() - shift);
    }
    if (scrolling != nullptr) {
      // The column that just left may have been the focused one. Its survivor is judged now, against the column that
      // took its place, which is the first moment that pair can be measured.
      scrolling->reevaluateAfterRemoval(scrolling->columnOf(m_focusedView), scrollViewportExtent());
    }
  }

  void Workspace::clampScrollToRange() {
    if (ScrollingLayout* scrolling = scrollingLayout()) {
      scrolling->clampScroll(scrollViewportExtent());
    }
  }

  void Workspace::markArrange(bool animate) {
    // Last mark wins, so a swipe's animated snap on release is not overridden by an unanimated motion mark in the same
    // frame.
    m_arrangeAnimate = animate;
    m_arrangePending = true;
    if (m_group != nullptr && m_group->output() != nullptr) {
      m_group->output()->markDirty(Dirty::Layout);
    }
  }

  // Tells every window whether it is alone, and whether it is the workspace's only window, but only when one of those
  // answers changed. It compares each answer's count and holder, and the config version, so every change is noticed
  // (even one window replaced by another at the same time). The guard stops the window handlers from triggering another
  // pass.
  void Workspace::refreshAloneRuleStates() {
    if (m_refreshingAloneRules) {
      return;
    }
    View* soleTiled = nullptr;
    size_t tiledViewCount = 0;
    for (const Column& column : m_layout->columns()) {
      for (View* view : column.views) {
        ++tiledViewCount;
        if (tiledViewCount == 1) {
          soleTiled = view;
        }
      }
    }
    if (tiledViewCount != 1) {
      soleTiled = nullptr;
    }
    View* soleVisible = nullptr;
    size_t visibleViewCount = 0;
    for (View* other : m_views) {
      if (!sharesWorkspace(other)) {
        continue;
      }
      ++visibleViewCount;
      if (visibleViewCount == 1) {
        soleVisible = other;
      }
    }
    if (visibleViewCount != 1) {
      soleVisible = nullptr;
    }
    const uint64_t generation = configStore().generation();
    if (tiledViewCount == m_lastAloneViewCount
        && soleTiled == m_lastAloneSoleView
        && visibleViewCount == m_lastOnlyWindowCount
        && soleVisible == m_lastOnlyWindowView
        && generation == m_lastAloneGeneration) {
      return;
    }
    m_lastAloneViewCount = tiledViewCount;
    m_lastAloneSoleView = soleTiled;
    m_lastOnlyWindowCount = visibleViewCount;
    m_lastOnlyWindowView = soleVisible;
    m_lastAloneGeneration = generation;
    m_refreshingAloneRules = true;
    for (View* view : m_views) {
      if (view != nullptr && view->mapped()) {
        view->notifyAloneStateChanged();
      }
    }
    m_refreshingAloneRules = false;
  }

  void Workspace::flushArrange() {
    if (m_arrangePending) {
      arrange(m_arrangeAnimate);
    }
  }

  std::unique_ptr<Layout> Workspace::previewArrangedLayout() const {
    auto preview = previewLayout();
    if (preview != nullptr) {
      preview->arrange(tiledArea());
    }
    return preview;
  }

  void Workspace::arrange(bool animate) {
    // Clearing here, rather than only in flushArrange, is what makes mixing the two safe: a direct arrange() satisfies
    // whatever was marked earlier in the frame, so the flush does not repeat it.
    m_arrangePending = false;
    refreshAloneRuleStates();
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

    m_layout->arrange(applyLayoutStruts(usable, m_layoutConfig.struts));
    // Tab visibility follows the layout on hidden workspaces too, so a workspace always comes back showing the right
    // tabs.
    m_tabs.sync();
    // A maximized float keeps the margin a maximized tile keeps, so it follows every change an arrange follows: gap,
    // border, struts, and the usable area.
    for (View* view : m_views) {
      if (view != nullptr && !view->tiled()) {
        view->refitFloatingMaximized();
        view->refitFloatingSnapped();
      }
    }
    // The map-time IPC event can fire before this arrange runs, leaving the previous window positions in the listing.
    // Re-emit now that the layout boxes are settled; the event coalescer caps this at one per frame.
    m_group->server()->scheduleIpcWindowsEvent();
    struct ResizeRequest {
      View* view;
      int width;
      int height;
    };
    // Plan size requests before presenting the new layout. A close barrier may need to retain both the old box and the
    // old client buffer until windows_out finishes. A client whose committed geometry never matches a stable configure
    // (Chromium CSD) must not replay its resize on every focus arrange.
    std::vector<View*> resized;
    std::vector<ResizeRequest> resizeRequests;
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped() || !view->tiled()) {
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        continue;
      }
      if (view->scheduledFullscreen()) {
        wlr_box fullArea{};
        wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &fullArea);
        if (fullArea.width > 0
            && fullArea.height > 0
            && (view->scheduledSize().width != fullArea.width || view->scheduledSize().height != fullArea.height)) {
          view->configureSize(fullArea.width, fullArea.height);
        }
        if (animate) {
          view->beginResizeAnimation(fullArea.width, fullArea.height, true);
        }
        continue;
      }
      const wlr_box target = tiledTargetBox(view, usable);
      const SizeHints hints = view->sizeHints();
      const int width = view->maximizedToEdges() ? target.width : clampWidth(target.width, hints);
      const int height = view->maximizedToEdges() ? target.height : clampHeight(target.height, hints);
      const View::ClientSize scheduled = view->scheduledSize();
      if (scheduled.width != width || scheduled.height != height) {
        resized.push_back(view);
        resizeRequests.push_back({.view = view, .width = width, .height = height});
      }
    }
    // Visual state below (scroll, positions) only applies while visible.
    Overview* overview = m_group->server()->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    if (!m_active && !m_inSwitchTransition && !overviewActive) {
      for (const ResizeRequest& request : resizeRequests) {
        request.view->requestTiledSize(request.width, request.height);
      }
      return;
    }

    // One positioning path for every layout: targets already include any
    // layout-specific offset, and each view animates or snaps itself.
    applyPositions(animate, resized);
    for (const ResizeRequest& request : resizeRequests) {
      request.view->requestTiledSize(request.width, request.height);
    }
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
    const int nodeX = view->layoutX();
    const int nodeY = view->layoutY();

    // Each case only decides the region the view occupies; everything after is common. Keeping that region off the
    // neighbouring output is the job of the output's clipped scene roots, not of this function.
    wlr_box target{};
    const Overview* overview = m_group->server()->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    const bool normallyVisible = view->pinned() || m_active || m_inSwitchTransition;

    if (view->pinned()) {
      // Pinned views sit outside the workspace, so no slide offset applies, and
      // they are sized from committed geometry rather than the presented size.
      const wlr_box& geometry = view->geometryBox();
      target = {nodeX, nodeY, geometry.width, geometry.height};
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
        target = {nodeX + m_slideOffsetX, nodeY + m_slideOffsetY, outputBox.width, outputBox.height};
      } else {
        // Floating views keep a pending requested size after a drag ends, until the client redraws. Tiled views
        // follow the box the layout assigned them.
        const bool inLayout = m_layout->columnOf(view) >= 0;
        const auto [floatingWidth, floatingHeight] = view->floatingSize();
        const wlr_box sized = inLayout ? tiledTargetBox(view, usable)
                                       : wlr_box{.x = 0, .y = 0, .width = floatingWidth, .height = floatingHeight};
        // A view that just joined the layout has no slot until the pending arrange. Presenting the empty slot would
        // erase the box it still shows, and that arrange would then place it like an opener instead of moving it.
        if (inLayout && (sized.width <= 0 || sized.height <= 0)) {
          view->setNodeEnabled(normallyVisible);
          return;
        }
        target = {nodeX + m_slideOffsetX, nodeY + m_slideOffsetY, sized.width, sized.height};
      }
    }

    view->setNodeEnabled(normallyVisible);
    view->applyPresentation(target);
  }

  void Workspace::applyPositions(bool animate, std::span<View* const> resized) {
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

    // Fullscreen and floating views position themselves. Established tiled members share windows_move below, while an
    // opening tiled lifecycle view is presented directly at its final slot.
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped()) {
        continue;
      }
      if (view->layoutFullscreen()) {
        const wlr_box target = fullscreenTargetBox(view);
        if (view->fullscreenOpeningActive()) {
          // windows_in carries the node while the opener scales inside this box; the layout keeps only the target.
          view->setLayoutTarget(target.x, target.y);
          view->presentBox(target);
          continue;
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
      }
    }
    applyTiledMotion(usable, animate, resized);
  }

  namespace {

    bool sameBox(const wlr_box& a, const wlr_box& b) {
      return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    float motionDirection(const wlr_box& from, const wlr_box& to) {
      for (const int delta : {to.x - from.x, to.y - from.y, to.width - from.width}) {
        if (delta != 0) {
          return delta < 0 ? -1.0F : 1.0F;
        }
      }
      return 1.0F;
    }

    // Squared distance a member's centre travels, in quarter pixels so no rounding is needed.
    long travelSquared(const wlr_box& from, const wlr_box& to) {
      const long dx = (2L * to.x + to.width) - (2L * from.x + from.width);
      const long dy = (2L * to.y + to.height) - (2L * from.y + from.height);
      return dx * dx + dy * dy;
    }

    // The progress speed, per second, at which a restarted motion keeps its members moving as fast as they were: the
    // least-squares fit of every member's current edge velocity onto its new span. Only members present in both
    // motions count; 0 when nothing carries over.
    template <typename Entries>
    double carriedProgressSpeed(const Entries& previous, const Entries& next, double previousSpeed, bool size) {
      if (previousSpeed == 0.0 || !std::isfinite(previousSpeed)) {
        return 0.0;
      }
      double along = 0.0;
      double squared = 0.0;
      for (const auto& entry : next) {
        const auto old =
            std::ranges::find_if(previous, [&entry](const auto& candidate) { return candidate.view == entry.view; });
        if (old == previous.end()) {
          continue;
        }
        const std::array<double, 2> oldSpan = size
            ? std::array<double, 2>{static_cast<double>(old->to.width - old->from.width),
                                    static_cast<double>(old->to.height - old->from.height)}
            : std::array<double, 2>{static_cast<double>(old->to.x - old->from.x),
                                    static_cast<double>(old->to.y - old->from.y)};
        const std::array<double, 2> newSpan = size
            ? std::array<double, 2>{static_cast<double>(entry.to.width - entry.from.width),
                                    static_cast<double>(entry.to.height - entry.from.height)}
            : std::array<double, 2>{static_cast<double>(entry.to.x - entry.from.x),
                                    static_cast<double>(entry.to.y - entry.from.y)};
        for (size_t axis = 0; axis < 2; ++axis) {
          along += oldSpan[axis] * previousSpeed * newSpan[axis];
          squared += newSpan[axis] * newSpan[axis];
        }
      }
      return squared > 0.0 ? along / squared : 0.0;
    }

    // Starts a 0 -> 1 motion timeline, from the carried speed when its curve is a spring that can use one.
    void startTimeline(AnimatedValue& value, bool& physics, int durationMs, const AnimationCurve& curve, double speed) {
      value.snap(0.0);
      physics = curve.easing == Easing::Spring && speed > 0.0;
      if (physics) {
        value.settleSpring(1.0, curve.spring, speed);
      } else {
        value.retarget(1.0, durationMs, curve);
      }
    }

  } // namespace

  void Workspace::applyTiledMotion(const wlr_box& usable, bool animate, std::span<View* const> resized) {
    const Overview* overview = m_group->server()->overview();
    const bool overviewActive = overview != nullptr && overview->active();

    struct Member {
      View* view;
      wlr_box from;
      wlr_box to;
      bool opening;
    };
    std::vector<Member> members;
    for (View* view : m_views) {
      if (view == nullptr
          || !view->mapped()
          || !view->tiled()
          || view->layoutFullscreen()
          || m_layout->columnOf(view) < 0) {
        continue;
      }
      const wlr_box slot = tiledTargetBox(view, usable);
      // Read before setLayoutTarget: an opener has no placement yet, whatever box its map commit presented at the
      // node's default origin.
      const wlr_box& presented = view->presentedBox();
      const bool positioned = view->positioned() && presented.width > 0 && presented.height > 0;
      const bool opening = !positioned || view->tiledOpeningDeferred();
      const std::optional<wlr_box> openingLayoutBox = view->openingLayoutBox();
      view->setLayoutTarget(slot.x, slot.y);
      if ((!view->onActiveWorkspace() && !m_inSwitchTransition && !overviewActive)
          || slot.width <= 0
          || slot.height <= 0) {
        releaseLayoutMotion(view);
        view->endLayoutMotion();
        view->presentTiledBox(slot);
        continue;
      }
      const wlr_box from = openingLayoutBox.value_or(
          wlr_box{.x = view->layoutX(), .y = view->layoutY(), .width = presented.width, .height = presented.height}
      );
      // A positioned member animates its size only when this pass reconfigured it or a motion already carries it;
      // otherwise it keeps the size it presents, which stays inside the slot.
      wlr_box to = slot;
      if (positioned
          && !opening
          && !openingLayoutBox.has_value()
          && !std::ranges::contains(resized, view)
          && std::ranges::none_of(m_motion.views, [view](const LayoutMotion::ViewEntry& entry) {
               return entry.view == view;
             })) {
        to.width = std::min(presented.width, slot.width);
        to.height = std::min(presented.height, slot.height);
      }
      members.push_back({
          .view = view,
          .from = positioned ? from : wlr_box{},
          .to = to,
          .opening = opening,
      });
    }

    const auto& animation = config().animation;
    const auto& move = animation.windowsMove;
    const bool animateMove = animate && animation.enabled && move.enabled && move.durationMs > 0;
    std::vector<LayoutMotion::ViewEntry> views;
    std::vector<Member> stableViews;
    for (const Member& member : members) {
      if (member.opening) {
        continue;
      }
      if (sameBox(member.from, member.to)) {
        stableViews.push_back(member);
        continue;
      }
      views.push_back({
          .view = member.view,
          .from = member.from,
          .to = member.to,
          .direction = motionDirection(member.from, member.to),
      });
    }

    // A fresh map starts windows_in in its final slot on the same tick established members begin windows_move. The
    // opener never joins the geometry motion, so its box is never interpolated, and the reflow it causes does not run
    // ahead of it. View::handleMap only hides it until this arrange places it. A member that merely rejoins the layout,
    // from floating or from another workspace, keeps whatever it is already showing.
    std::vector<View*> openingViews;
    for (const Member& member : members) {
      if (!member.opening) {
        continue;
      }
      member.view->resumeTiledOpening();
      member.view->endLayoutMotion();
      member.view->presentTiledBox(member.to);
      // Lifecycle actors are overlays while established peers reflow underneath. Reassert that ordering on every
      // arrange because an older peer may have been raised for an interrupted rearrangement.
      member.view->raiseToTop();
      openingViews.push_back(member.view);
    }
    for (const Member& member : stableViews) {
      // A client may still owe the buffer for a completed resize. Remove the view from any old motion, then retain the
      // compositor-owned endpoint until that exact configure and size have committed.
      releaseLayoutMotion(member.view);
      member.view->completeLayoutMotion(member.to);
      syncViewPresentation(member.view);
    }

    // A running motion heading for the same layout keeps going; anything else restarts from the current boxes.
    if ((m_motion.progress.animating() || m_motion.size.animating()) && views.size() == m_motion.views.size()) {
      bool same = true;
      for (size_t i = 0; i < views.size() && same; ++i) {
        same = views[i].view == m_motion.views[i].view && sameBox(views[i].to, m_motion.views[i].to);
      }
      if (same) {
        return;
      }
    }
    for (const LayoutMotion::ViewEntry& running : m_motion.views) {
      if (std::ranges::none_of(views, [&](const LayoutMotion::ViewEntry& entry) {
            return entry.view == running.view;
          })) {
        running.view->endLayoutMotion();
      }
    }

    // A pair of positioned members whose side relation changes cannot stay disjoint; the focused one passes over the
    // other, since it is the window the user acted on, and otherwise the one travelling farther.
    std::vector<bool> raise(views.size(), false);
    for (size_t i = 0; i < views.size(); ++i) {
      for (size_t j = i + 1; j < views.size(); ++j) {
        const MotionBox a{views[i].from, views[i].to};
        const MotionBox b{views[j].from, views[j].to};
        if (keepsSeparation(a, b)) {
          continue;
        }
        if (views[i].view == m_focusedView || views[j].view == m_focusedView) {
          raise[views[i].view == m_focusedView ? i : j] = true;
        } else {
          raise[travelSquared(a.from, a.to) > travelSquared(b.from, b.to) ? i : j] = true;
        }
      }
    }

    if (views.empty()) {
      endLayoutMotion();
      return;
    }
    if (!animateMove) {
      endLayoutMotion();
      for (const Member& member : members) {
        member.view->presentTiledBox(member.to);
      }
      return;
    }

    // An interrupted motion hands its speed to the next one, so repeated actions bend the motion instead of stopping
    // it and starting again from rest.
    const auto& resize = animation.windowsResize;
    const double positionSpeed = m_motion.progress.animating()
        ? carriedProgressSpeed(m_motion.views, views, m_motion.progress.velocity(), false)
        : 0.0;
    const double sizeSpeed =
        m_motion.size.animating() ? carriedProgressSpeed(m_motion.views, views, m_motion.size.velocity(), true) : 0.0;
    m_motion.views = std::move(views);
    startTimeline(m_motion.progress, m_motion.progressPhysics, move.durationMs, move.curve, positionSpeed);
    m_motion.geometryCurve.reset(move.curve);
    // Without windows_resize the size lands at once while the position still moves.
    if (resize.enabled) {
      startTimeline(m_motion.size, m_motion.sizePhysics, resize.durationMs, resize.curve, sizeSpeed);
      m_motion.sizeCurve.reset(resize.curve);
    } else {
      m_motion.size.snap(1.0);
      m_motion.sizePhysics = false;
    }

    for (size_t i = 0; i < m_motion.views.size(); ++i) {
      const LayoutMotion::ViewEntry& entry = m_motion.views[i];
      entry.view->beginLayoutMotion(entry.direction);
      entry.view->presentTiledBox(entry.from);
      if (raise[i] && entry.view != m_focusedView) {
        entry.view->raiseToTop();
      }
    }
    // Raised last, so it also passes over every other window raised above.
    for (size_t i = 0; i < m_motion.views.size(); ++i) {
      if (raise[i] && m_motion.views[i].view == m_focusedView) {
        m_motion.views[i].view->raiseToTop();
      }
    }
    for (View* view : openingViews) {
      view->raiseToTop();
    }
    wlr_output_schedule_frame(m_group->output()->wlr());
  }

  bool Workspace::tickLayoutMotion(double nowMsec) {
    const bool positionTicked = m_motion.progress.tick(nowMsec);
    const bool sizeTicked = m_motion.size.tick(nowMsec);
    // A carried-speed spring settles on its own solver; stop it once no member's pixel can change. One progress unit
    // spans each member's whole travel, so the longest travel sets the pixel scale.
    if (m_motion.progressPhysics || m_motion.sizePhysics) {
      int travel = 1;
      int resize = 1;
      for (const LayoutMotion::ViewEntry& entry : m_motion.views) {
        travel = std::max({travel, std::abs(entry.to.x - entry.from.x), std::abs(entry.to.y - entry.from.y)});
        resize = std::max(
            {resize, std::abs(entry.to.width - entry.from.width), std::abs(entry.to.height - entry.from.height)}
        );
      }
      static_cast<void>(m_motion.progress.finishSpringTail(travel));
      static_cast<void>(m_motion.size.finishSpringTail(resize));
    }
    const bool running = m_motion.progress.animating() || m_motion.size.animating();
    const bool needsFinalPresentation = !running && !m_motion.views.empty();
    if (!positionTicked && !sizeTicked && !needsFinalPresentation) {
      return false;
    }

    const auto timeline = [](const AnimatedValue& value, bool physics, const MonotonicEasing& curve) {
      return physics ? std::clamp(value.current(), 0.0, 1.0) : curve.value(value.progress());
    };
    const double position = timeline(m_motion.progress, m_motion.progressPhysics, m_motion.geometryCurve);
    const double size =
        m_motion.size.animating() ? timeline(m_motion.size, m_motion.sizePhysics, m_motion.sizeCurve) : 1.0;
    for (const LayoutMotion::ViewEntry& entry : m_motion.views) {
      if (entry.view->mapped() && m_layout->columnOf(entry.view) >= 0) {
        entry.view->presentTiledBox(interpolateBox(entry.from, entry.to, position, size));
      }
    }
    if (running) {
      return true;
    }

    std::vector<LayoutMotion::ViewEntry> views = std::move(m_motion.views);
    m_motion.views.clear();
    for (const LayoutMotion::ViewEntry& entry : views) {
      if (entry.view->mapped() && m_layout->columnOf(entry.view) >= 0) {
        entry.view->completeLayoutMotion(entry.to);
      } else {
        entry.view->endLayoutMotion();
      }
    }
    return false;
  }

  void Workspace::endLayoutMotion() {
    m_motion.progress.snap(1.0);
    m_motion.size.snap(1.0);
    std::vector<LayoutMotion::ViewEntry> views = std::move(m_motion.views);
    m_motion.views.clear();
    for (const LayoutMotion::ViewEntry& entry : views) {
      entry.view->endLayoutMotion();
    }
  }

  void Workspace::trackCloseSnapshot(CloseSnapshotId id, const wlr_box& outputBox) {
    const wlr_box canvas{outputBox.x - m_slideOffsetX, outputBox.y - m_slideOffsetY, outputBox.width, outputBox.height};
    m_trackedCloseSnapshots.push_back({.id = id, .canvas = canvas});
    syncCloseSnapshots();
  }

  void Workspace::syncCloseSnapshots() {
    if (m_group == nullptr) {
      return;
    }
    Server* server = m_group->server();
    std::erase_if(m_trackedCloseSnapshots, [server](const TrackedCloseSnapshot& snapshot) {
      return !server->closeSnapshotAlive(snapshot.id);
    });

    const bool visible = m_active || m_inSwitchTransition;
    for (const TrackedCloseSnapshot& snapshot : m_trackedCloseSnapshots) {
      server->presentCloseSnapshot(
          snapshot.id, snapshot.canvas.x + m_slideOffsetX, snapshot.canvas.y + m_slideOffsetY, visible
      );
    }
  }

  void Workspace::discardCloseSnapshots() {
    if (m_group != nullptr) {
      Server* server = m_group->server();
      for (const TrackedCloseSnapshot& snapshot : m_trackedCloseSnapshots) {
        // Keep the server-owned animation object alive until the normal post-tick reap. Destroying it while a
        // WorkspaceGroup animation tick is iterating the server's owner snapshot would invalidate that iteration.
        server->presentCloseSnapshot(snapshot.id, snapshot.canvas.x, snapshot.canvas.y, false);
      }
    }
    m_trackedCloseSnapshots.clear();
  }

  void Workspace::releaseLayoutMotion(View* view) {
    std::erase_if(m_motion.views, [view](const LayoutMotion::ViewEntry& entry) { return entry.view == view; });
  }

  const AnimatedValue* Workspace::layoutMotionValue() const {
    return m_motion.progress.animating() ? &m_motion.progress : nullptr;
  }

  bool Workspace::inTabGroup(const View* view) const {
    const int column = m_layout->columnOf(view);
    return column >= 0 && tabGroupOf(m_layout->columns()[static_cast<size_t>(column)], view) != nullptr;
  }

  wlr_box Workspace::tiledTargetBox(const View* view, const wlr_box& usable, const Layout* layout) const {
    wlr_box target = (layout != nullptr ? layout : m_layout.get())->targetBox(view);
    if (view == nullptr || !view->maximizedToEdges()) {
      return contentBoxInTile(target, view != nullptr ? view->titleBarHeight() : 0);
    }
    if (scrollingLayout() != nullptr) {
      const bool vertical = scrollingVertical();
      const wlr_box tiled = applyLayoutStruts(usable, m_layoutConfig.struts);
      if (vertical) {
        target.x = usable.x;
        target.y = usable.y + target.y - tiled.y;
      } else {
        target.y = usable.y;
        target.x = usable.x + target.x - tiled.x;
      }
    } else {
      target.x = usable.x;
      target.y = usable.y;
    }
    target.width = usable.width;
    target.height = usable.height;
    return contentBoxInTile(target, view->titleBarHeight());
  }

  wlr_box Workspace::fullscreenTargetBox(const View* view) const {
    wlr_box outputBox{};
    if (m_group == nullptr || m_group->output() == nullptr) {
      return outputBox;
    }
    wlr_output_layout_get_box(m_group->server()->outputLayout(), m_group->output()->wlr(), &outputBox);
    const ScrollingLayout* scrolling = scrollingLayout();
    const int column = m_layout->columnOf(view);
    if (scrolling == nullptr || column < 0) {
      return outputBox;
    }
    // A fullscreen member of the strip stays anchored to its own column, so scrolling still carries it off-screen.
    const bool vertical = scrollingVertical();
    const int position = (vertical ? outputBox.y : outputBox.x)
        + scrolling->columnX(column, scrollViewportExtent())
        + m_layoutConfig.edgePad
        - static_cast<int>(std::lround(scrolling->scroll()));
    if (vertical) {
      outputBox.y = position;
    } else {
      outputBox.x = position;
    }
    return outputBox;
  }

  View* Workspace::focusAlongStrip(int direction) {
    flushArrange();
    if (const auto horizontal = m_layout->focusHorizontalLeaf(m_focusedView, direction)) {
      return *horizontal;
    }
    const int current = m_layout->columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return nullptr;
    }
    return columnEntry(m_layout->columns()[static_cast<size_t>(target)]);
  }

  View* Workspace::focusWithinLane(int direction) {
    flushArrange();
    if (const auto vertical = m_layout->focusVerticalLeaf(m_focusedView, direction)) {
      return *vertical;
    }
    const int column = m_layout->columnOf(m_focusedView);
    const int row = m_layout->rowOf(m_focusedView);
    if (column < 0 || row < 0) {
      return nullptr;
    }
    // A tab group is one unit of its column: focus steps past its hidden tabs and enters it at the tab it shows.
    const Column& lane = m_layout->columns()[static_cast<size_t>(column)];
    const auto at = static_cast<size_t>(row);
    if (direction > 0) {
      return unitEntry(lane, lane.tabs.unitEnd(at));
    }
    const size_t start = lane.tabs.unitStart(at);
    return direction < 0 && start > 0 ? unitEntry(lane, start - 1) : nullptr;
  }

  View* Workspace::preferRecentPeer(View* target) const {
    if (target == nullptr || m_group == nullptr) {
      return target;
    }
    const std::vector<View*> peers = m_layout->focusPeers(m_focusedView, target);
    // A single peer is the only window the move may land on, such as the tab a tabbed column shows.
    if (peers.size() == 1) {
      return peers.front();
    }
    if (peers.empty()) {
      return target;
    }
    for (const auto& entry : m_group->server()->registry().placementRoots()) {
      View* candidate = entry.get();
      if (candidate->mapped() && candidate->workspace() == this && std::ranges::find(peers, candidate) != peers.end()) {
        return candidate;
      }
    }
    return target;
  }

  View* Workspace::floatingFocusTarget(bool horizontal, int direction) const {
    if (m_focusedView == nullptr || direction == 0) {
      return nullptr;
    }
    std::vector<LayoutTarget> targets;
    for (View* view : m_floatingStack) {
      if (view->mapped()
          && !view->minimized()
          && !view->pinned()
          && !view->currentFullscreen()
          && !view->scheduledFullscreen()) {
        const wlr_box& geo = view->geometryBox();
        targets.push_back({view, view->layoutX(), view->layoutY(), geo.width, geo.height});
      }
    }
    if (View* neighbor = directionalNeighbor(targets, m_focusedView, horizontal, direction)) {
      return neighbor;
    }
    // Cascaded windows overlap. When no edge neighbor exists, use centers to reach overlapping windows too.
    const auto source =
        std::ranges::find_if(targets, [this](const auto& target) { return target.view == m_focusedView; });
    if (source == targets.end()) {
      return nullptr;
    }
    View* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& candidate : targets) {
      const double dx = candidate.x + candidate.width / 2.0 - source->x - source->width / 2.0;
      const double dy = candidate.y + candidate.height / 2.0 - source->y - source->height / 2.0;
      if ((horizontal ? dx : dy) * direction <= 0) {
        continue;
      }
      const double distance = dx * dx + dy * dy;
      if (distance < bestDistance) {
        best = candidate.view;
        bestDistance = distance;
      }
    }
    return best;
  }

  View* Workspace::focusAdjacent(int direction) {
    if (m_layoutMode == LayoutMode::Floating) {
      return floatingFocusTarget(true, direction);
    }
    return preferRecentPeer(scrollingVertical() ? focusWithinLane(direction) : focusAlongStrip(direction));
  }

  View* Workspace::focusVertical(int direction) {
    if (m_layoutMode == LayoutMode::Floating) {
      return floatingFocusTarget(false, direction);
    }
    return preferRecentPeer(scrollingVertical() ? focusAlongStrip(direction) : focusWithinLane(direction));
  }

  View* Workspace::focusFirstColumn() const {
    const auto& columns = m_layout->columns();
    if (columns.empty()) {
      return nullptr;
    }
    return columnEntry(columns.front());
  }

  View* Workspace::focusLastColumn() const {
    const auto& columns = m_layout->columns();
    if (columns.empty()) {
      return nullptr;
    }
    return columnEntry(columns.back());
  }

  View* Workspace::focusReplacementForRemoval(const View* view) const {
    if (view == nullptr) {
      return nullptr;
    }

    const int columnIndex = m_layout->columnOf(view);
    const int rowIndex = m_layout->rowOf(view);
    const auto& columns = m_layout->columns();
    const auto mappedCandidate = [view](View* candidate) {
      return candidate != nullptr && candidate != view && candidate->mapped() && !candidate->minimized();
    };

    if (View* parent = view->shellParent(); parent != nullptr && parent->workspace() == this && !parent->minimized()) {
      return parent;
    }

    // Floating views have no layout successor: hand focus back to the most recently focused mapped view on this
    // workspace, since nothing else refocuses until a destroy-time fallback that unmap-only clients never reach.
    if (columnIndex < 0 || columnIndex >= static_cast<int>(columns.size())) {
      for (const auto& entry : m_group->server()->registry().placementRoots()) {
        if (entry.get() != view && entry->mapped() && !entry->minimized() && entry->workspace() == this) {
          return entry.get();
        }
      }
      return nullptr;
    }

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
    const auto recentInColumn = [&](int targetColumn) -> View* {
      const auto& members = columns[static_cast<size_t>(targetColumn)].views;
      for (const auto& entry : m_group->server()->registry().placementRoots()) {
        View* candidate = entry.get();
        if (mappedCandidate(candidate) && std::ranges::find(members, candidate) != members.end()) {
          return candidate;
        }
      }
      return nullptr;
    };
    // Keep geometric column proximity, but preserve focus memory within that column. Row order is placement, not
    // history: closing a temporary neighboring column should return to the row the user was working in.
    for (int targetColumn = columnIndex - 1; targetColumn >= 0; --targetColumn) {
      if (View* candidate = recentInColumn(targetColumn)) {
        return candidate;
      }
    }
    for (int targetColumn = columnIndex + 1; targetColumn < static_cast<int>(columns.size()); ++targetColumn) {
      if (View* candidate = recentInColumn(targetColumn)) {
        return candidate;
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

  View* Workspace::cycleFocusTarget(int direction) const {
    std::vector<View*> ring;
    for (const Column& column : m_layout->columns()) {
      ring.insert(ring.end(), column.views.begin(), column.views.end());
    }
    std::ranges::copy_if(m_floatingStack, std::back_inserter(ring), [](const View* view) {
      return !view->minimized();
    });
    if (ring.size() < 2) {
      return nullptr;
    }

    const auto focused = std::ranges::find(ring, m_focusedView);
    if (focused == ring.end()) {
      return direction > 0 ? ring.front() : ring.back();
    }
    const auto index = static_cast<std::ptrdiff_t>(focused - ring.begin());
    const auto count = static_cast<std::ptrdiff_t>(ring.size());
    const auto target = (index + direction % count + count) % count;
    return ring[static_cast<size_t>(target)];
  }

  void Workspace::toggleShowDesktop() {
    View* focus = m_focusedView;
    // The focused window goes last, so focus is handed on once instead of through every other window.
    std::vector<View*> views = m_views;
    if (const auto focused = std::ranges::find(views, focus); focused != views.end()) {
      std::rotate(focused, std::next(focused), views.end());
    }
    bool hid = false;
    for (View* view : views) {
      hid = (!view->minimized() && view->requestMinimize()) || hid;
    }
    if (hid) {
      m_showDesktopFocus = focus;
      return;
    }
    // Nothing left to hide: bring back every minimized window here, however it was minimized.
    View* latest = nullptr;
    for (View* view : m_views) {
      if (!view->mapped() || !view->minimized()) {
        continue;
      }
      if (latest == nullptr || view->minimizeSerial() > latest->minimizeSerial()) {
        latest = view;
      }
      view->setMinimized(false);
    }
    focus = std::exchange(m_showDesktopFocus, nullptr);
    if (focus == nullptr || !focus->mapped() || focus->minimized() || focus->workspace() != this) {
      focus = m_focusedView != nullptr ? nullptr : latest;
    }
    if (focus != nullptr) {
      m_group->server()->focusView(focus, FocusReason::Directional);
    }
  }

  void Workspace::forgetShowDesktopFocus(const View* view) {
    if (m_showDesktopFocus == view) {
      m_showDesktopFocus = nullptr;
    }
  }

  bool Workspace::swapFocusedInCycle(int direction) {
    std::vector<View*> ring;
    for (const Column& column : m_layout->columns()) {
      ring.insert(ring.end(), column.views.begin(), column.views.end());
    }
    if (ring.size() < 2) {
      return false;
    }

    const auto focused = std::ranges::find(ring, m_focusedView);
    if (focused == ring.end()) {
      return false;
    }
    const auto index = static_cast<std::ptrdiff_t>(focused - ring.begin());
    const auto count = static_cast<std::ptrdiff_t>(ring.size());
    const auto target = (index + direction % count + count) % count;
    if (!m_layout->swapViews(m_focusedView, ring[static_cast<size_t>(target)])) {
      return false;
    }
    markArrange();
    ensureFocusedVisible();
    return true;
  }

  bool Workspace::swapFocusedInDirection(bool horizontal, int direction) {
    flushArrange();
    // Geometry only: the focus memory directional focus applies could pick a diagonal tile.
    std::vector<LayoutTarget> targets;
    for (const Column& column : m_layout->columns()) {
      for (View* view : column.views) {
        // A hidden tab shares the box of the tab on show, so only the one on show is a neighbor on screen.
        if (hiddenTab(column, view)) {
          continue;
        }
        const wlr_box box = m_layout->targetBox(view);
        targets.push_back({.view = view, .x = box.x, .y = box.y, .width = box.width, .height = box.height});
      }
    }
    View* target = directionalNeighbor(targets, m_focusedView, horizontal, direction);
    if (target == nullptr || !m_layout->swapViews(m_focusedView, target)) {
      return false;
    }
    markArrange();
    ensureFocusedVisible();
    return true;
  }

  bool Workspace::increaseMasterCount() {
    MasterStackLayout* master = masterLayout();
    if (master == nullptr || !master->promoteFromStack()) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::decreaseMasterCount() {
    MasterStackLayout* master = masterLayout();
    if (master == nullptr || !master->demoteToStack()) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::toggleDwindleSplit() {
    DwindleLayout* dwindle = dwindleLayout();
    if (dwindle == nullptr || !dwindle->toggleSplit(m_focusedView)) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::moveLaneAlongStrip(int direction) {
    View* destination = focusAlongStrip(direction);
    if (destination == nullptr) {
      return false;
    }
    const int current = m_layout->columnOf(m_focusedView);
    const int target = m_layout->columnOf(destination);
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return false;
    }
    m_layout->moveColumn(current, target);
    revealMovedFocusedColumn(current);
    markArrange();
    return true;
  }

  bool Workspace::consumeFocused(int direction) {
    flushArrange();
    if (!m_layout->consume(m_focusedView, direction)) {
      return false;
    }
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::consumeFromFocused(int direction) {
    if (!m_layout->consumeFrom(m_focusedView, direction)) {
      return false;
    }
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::expelFocused(int direction) {
    if (!m_layout->expel(m_focusedView, direction)) {
      return false;
    }
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::moveWithinLane(int direction) {
    flushArrange();
    if (!m_layout->moveViewVertical(m_focusedView, direction)) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::moveFocusedColumn(int direction) {
    // Dwindle has no columns to carry, so a column move is the window move.
    if (dwindleLayout() != nullptr) {
      return (m_focusedView == nullptr || !m_focusedView->floating()) && moveFocusedHorizontal(direction);
    }
    return scrollingVertical() ? moveWithinLane(direction) : moveLaneAlongStrip(direction);
  }

  bool Workspace::moveFocusedHorizontal(int direction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      return m_focusedView->moveFloatingToEdge(direction < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT);
    }
    if (DwindleLayout* dwindle = dwindleLayout()) {
      if (!dwindle->moveView(m_focusedView, true, direction)) {
        return false;
      }
      markArrange();
      return true;
    }
    if (scrollingVertical()) {
      return moveWithinLane(direction);
    }
    return expelFocused(direction) || consumeFocused(direction);
  }

  bool Workspace::moveFocusedVertical(int direction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      return m_focusedView->moveFloatingToEdge(direction < 0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
    }
    return scrollingVertical() ? moveLaneAlongStrip(direction) : moveWithinLane(direction);
  }

  bool Workspace::moveFocusedColumnFirst() {
    if (m_focusedView == nullptr) {
      return false;
    }
    const int current = m_layout->columnOf(m_focusedView);
    if (current <= 0) {
      return false;
    }
    m_layout->moveColumn(current, 0);
    revealMovedFocusedColumn(current);
    markArrange();
    return true;
  }

  bool Workspace::moveFocusedColumnLast() {
    if (m_focusedView == nullptr) {
      return false;
    }
    const int current = m_layout->columnOf(m_focusedView);
    const int last = static_cast<int>(m_layout->columns().size()) - 1;
    if (current < 0 || current >= last) {
      return false;
    }
    m_layout->moveColumn(current, last);
    revealMovedFocusedColumn(current);
    markArrange();
    return true;
  }

  std::optional<std::array<int, 2>> Workspace::focusedFloatingAxis(bool width) const {
    View* view = m_focusedView;
    return view != nullptr ? view->floatingAxisBasis(width) : std::nullopt;
  }

  std::optional<double> Workspace::focusedFloatingFraction(bool width) const {
    return m_focusedView != nullptr ? m_focusedView->floatingFraction(width) : std::nullopt;
  }

  bool
  Workspace::resizeFocusedFloating(const std::optional<double>& widthFrac, const std::optional<double>& heightFrac) {
    View* view = m_focusedView;
    return view != nullptr && view->resizeFloatingFractions(widthFrac, heightFrac);
  }

  bool Workspace::cycleFocusedWidth(int direction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      const auto axis = focusedFloatingAxis(true);
      if (!axis) {
        return false;
      }
      const double current = presetSnappedFraction(m_layoutConfig.extentPresets, (*axis)[0], (*axis)[1]);
      return resizeFocusedFloating(nextFractionPreset(m_layoutConfig.extentPresets, current, direction), std::nullopt);
    }
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->cycleWidth(column, direction)) {
      return false;
    }
    m_focusedView->setMaximizedState(false);
    reevaluateFocusedColumn();
    markArrange();
    return true;
  }

  bool Workspace::cycleFocusedHeight(int direction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      const auto axis = focusedFloatingAxis(false);
      if (!axis) {
        return false;
      }
      const double current = presetSnappedFraction(m_layoutConfig.extentPresets, (*axis)[0], (*axis)[1]);
      return resizeFocusedFloating(std::nullopt, nextFractionPreset(m_layoutConfig.extentPresets, current, direction));
    }
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    if (!cycleFractionPreset(
            m_layoutConfig.extentPresets, m_layout->heightFraction(m_focusedView), direction,
            [&](double fraction) { return m_layout->setHeightFraction(m_focusedView, fraction); },
            [&] { return m_layout->heightFraction(m_focusedView); }
        )) {
      return false;
    }
    m_focusedView->setMaximizedState(false);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::setFocusedWidth(double fraction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      return resizeFocusedFloating(std::clamp(fraction, 0.1, 1.0), std::nullopt);
    }
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->setWidthFraction(column, fraction)) {
      return false;
    }
    m_focusedView->setMaximizedState(false);
    reevaluateFocusedColumn();
    markArrange();
    return true;
  }

  bool Workspace::setFocusedHeight(double fraction) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      return resizeFocusedFloating(std::nullopt, std::clamp(fraction, 0.1, 1.0));
    }
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    if (!m_layout->setHeightFraction(m_focusedView, fraction)) {
      return false;
    }
    m_focusedView->setMaximizedState(false);
    ensureFocusedVisible();
    markArrange();
    return true;
  }

  bool Workspace::centerFocusedColumn() {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_focusedView == nullptr) {
      return false;
    }
    const int column = scrolling->columnOf(m_focusedView);
    if (!scrolling->centerColumn(column, scrollViewportExtent())) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::modifyFocusedWidth(double delta) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      if (const auto current = focusedFloatingFraction(true)) {
        return resizeFocusedFloating(std::clamp(*current + delta, 0.1, 1.0), std::nullopt);
      }
      return false;
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    // Clamp here, not in the layouts: ScrollingLayout clamps internally but
    // DwindleLayout does not, and both must land in [0.1, 1.0].
    return setFocusedWidth(std::clamp(m_layout->widthFraction(column) + delta, 0.1, 1.0));
  }

  bool Workspace::modifyFocusedHeight(double delta) {
    if (m_focusedView != nullptr && m_focusedView->floating()) {
      if (const auto current = focusedFloatingFraction(false)) {
        return resizeFocusedFloating(std::nullopt, std::clamp(*current + delta, 0.1, 1.0));
      }
      return false;
    }
    // Clamp here, not in the layouts: DwindleLayout does not clamp the overall
    // fraction, and every layout must land in [0.1, 1.0].
    return setFocusedHeight(std::clamp(m_layout->heightFraction(m_focusedView) + delta, 0.1, 1.0));
  }

  bool Workspace::resizeFocusedEdge(uint32_t edges, double delta) {
    if (m_focusedView == nullptr) {
      return false;
    }
    const bool horizontal = (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0;
    const bool vertical = (edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0;
    if (horizontal == vertical) {
      // One axis exactly: a corner or an empty mask is not something this action
      // can express, so refuse instead of half-applying it.
      return false;
    }
    View* view = m_focusedView;
    if (view->floating()) {
      // No arrange here: it would re-clamp the float against the client's pre-resize committed size.
      view->resizeFloatingEdge(edges, delta);
      return true;
    }
    // Resolve the edges and open the session before anything is mutated: a layout
    // that offers no boundary here, or cannot start the resize, must leave the
    // window exactly as it was, including its maximize-to-edges state.
    const uint32_t resolved = m_layout->sanitizeResizeEdges(view, edges);
    if (resolved == 0) {
      return false;
    }
    const wlr_box usable = tiledArea();
    std::unique_ptr<ResizeGrab> session = m_layout->beginResize(view, resolved, usable);
    if (session == nullptr) {
      return false;
    }
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false);
    }
    // A left or top edge travels against the axis, so growing from there moves in
    // the negative direction. The session applies one total delta from the state
    // it opened with, exactly like a single pointer move during a drag.
    const bool outwardNegative = (resolved & (WLR_EDGE_LEFT | WLR_EDGE_TOP)) != 0;
    const double pixels = delta * (horizontal ? usable.width : usable.height);
    const double travel = outwardNegative ? -pixels : pixels;
    session->applyDelta(horizontal ? travel : 0.0, horizontal ? 0.0 : travel, usable);
    view->setMaximizedState(false);
    reevaluateFocusedColumn();
    markArrange();
    return true;
  }

  bool Workspace::toggleFocusedFullWidth() {
    if (m_focusedView != nullptr && m_focusedView->maximizedToEdges()) {
      m_focusedView->setMaximizedToEdges(false);
    }
    if (m_focusedView != nullptr && m_focusedView->mapped() && m_focusedView->floating()) {
      // A float owns no column, so the full-width analogue is filling the usable
      // area, matching what default_maximize gives a float at map time.
      // Fullscreen already covers the output and owns the geometry: toggling
      // under it would capture a fullscreen-sized restore box and leave the
      // maximized flag inverted once fullscreen is dropped.
      if (m_focusedView->scheduledFullscreen()) {
        return false;
      }
      m_focusedView->toggleMaximized();
      return true;
    }
    const int column = m_layout->columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    const bool fullWidth = m_layout->toggleFullWidth(column);
    m_focusedView->setMaximizedState(fullWidth);
    reevaluateFocusedColumn();
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

    View* target = m_focusedView;
    if (!target->pinned()
        && !target->scheduledFullscreen()
        && !(target->floating() && floatingLayerRaised())
        && m_group != nullptr
        && m_group->output() != nullptr) {
      const wlr_box outputBox = m_group->output()->layoutBox();
      const wlr_box focusedBox = target->presentedBox();
      wlr_box focusedVisible{};
      if (wlr_box_intersection(&focusedVisible, &focusedBox, &outputBox)) {
        // A newly mapped window can own focus while an older fullscreen window
        // still covers it from the higher fullscreen scene layer. In that
        // state the action follows what the user can see: leave the obscuring
        // fullscreen instead of fullscreening the hidden focused window.
        wlr_scene_node* fullscreenNode = nullptr;
        wl_list_for_each_reverse(fullscreenNode, &m_fullscreenTree->children, link) {
          SceneNode* sceneNode = sceneNodeFrom(fullscreenNode->data);
          if (sceneNode == nullptr || sceneNode->kind != SceneNodeKind::View) {
            continue;
          }
          auto* candidate = static_cast<View*>(sceneNode);
          if (candidate == target
              || !candidate->mapped()
              || !candidate->onActiveWorkspace()
              || !candidate->scheduledFullscreen()) {
            continue;
          }
          const wlr_box fullscreenBox{
              .x = candidate->layoutX() + m_slideOffsetX,
              .y = candidate->layoutY() + m_slideOffsetY,
              .width = outputBox.width,
              .height = outputBox.height,
          };
          wlr_box obscured{};
          if (wlr_box_intersection(&obscured, &fullscreenBox, &focusedVisible)
              && obscured.x == focusedVisible.x
              && obscured.y == focusedVisible.y
              && obscured.width == focusedVisible.width
              && obscured.height == focusedVisible.height) {
            target = candidate;
            break;
          }
        }
      }
    }

    target->toggleFullscreen();
    return true;
  }

  bool Workspace::setFocusedColumnTabbed(std::optional<bool> tabbed) {
    TabbedContainers* containers = m_layout->tabbedContainers();
    View* view = m_focusedView;
    const int column = containers != nullptr && view != nullptr && view->tiled() ? m_layout->columnOf(view) : -1;
    if (column < 0) {
      return false;
    }
    const bool grouped = tabGroupOf(m_layout->columns()[static_cast<size_t>(column)], view) != nullptr;
    if (!containers->setTabbed(view, tabbed.value_or(!grouped))) {
      return false;
    }
    reevaluateFocusedColumn();
    markArrange();
    return true;
  }

  bool Workspace::moveFocusedTab(int direction) {
    TabbedContainers* containers = m_layout->tabbedContainers();
    if (containers == nullptr || m_focusedView == nullptr || !m_focusedView->tiled()) {
      return false;
    }
    if (!containers->moveTab(m_focusedView, direction)) {
      return false;
    }
    markArrange();
    return true;
  }

  bool Workspace::setFocusedTabBar(std::optional<bool> shown) {
    TabbedContainers* containers = m_layout->tabbedContainers();
    if (containers == nullptr || m_focusedView == nullptr || !m_focusedView->tiled()) {
      return false;
    }
    if (!containers->setTabBar(m_focusedView, shown)) {
      return false;
    }
    // The tabs grow into the bar's space, or give it back.
    markArrange();
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

  void Workspace::activateFocusedColumn() {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    scrolling->activateColumn(scrolling->columnOf(m_focusedView), scrollViewportExtent());
  }

  void Workspace::reevaluateFocusedColumn() {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    scrolling->reevaluateColumn(scrolling->columnOf(m_focusedView), scrollViewportExtent());
  }

  void Workspace::revealMovedFocusedColumn(int previousColumn) {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    scrolling->activateColumn(scrolling->columnOf(m_focusedView), scrollViewportExtent(), previousColumn);
  }

  void Workspace::noteRemovalOfFocusedColumn(int columnIndex) {
    if (ScrollingLayout* scrolling = scrollingLayout()) {
      scrolling->noteRemovalOfFocusedColumn(columnIndex);
    }
  }

  void Workspace::snapVisible(const View* view) {
    ScrollingLayout* scrolling = scrollingLayout();
    if (scrolling == nullptr || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    scrolling->snapVisible(scrolling->columnOf(view), scrollViewportExtent());
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
      // syncViewPresentation (arrange / slide).
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

  void Workspace::setSlideOffset(double x, double y) {
    m_slideOffsetX = static_cast<int>(std::lround(x));
    m_slideOffsetY = static_cast<int>(std::lround(y));
    if (m_tree != nullptr) {
      wlr_scene_node_set_position(&m_tree->node, m_slideOffsetX, m_slideOffsetY);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_node_set_position(&m_fullscreenTree->node, m_slideOffsetX, m_slideOffsetY);
    }
    for (View* view : m_views) {
      if (!view->pinned() && view->mapped()) {
        syncViewPresentation(view);
      }
    }
    syncCloseSnapshots();
  }

  void Workspace::endSwitchTransition() {
    // Put every view back at its resting position while transition visibility is still active: an inactive workspace
    // deliberately skips presentation sync once m_inSwitchTransition is cleared.
    setSlideOffset(0, 0);
    if (m_tree != nullptr) {
      wlr_scene_tree_set_clip(m_tree, nullptr);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_tree_set_clip(m_fullscreenTree, nullptr);
    }
    m_inSwitchTransition = false;
    for (View* view : m_switchViews) {
      view->setFadeAlpha(1.0F);
    }
    m_switchViews.clear();
    applyVisibility();
    syncCloseSnapshots();
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

  void Workspace::rename(std::string name, size_t index, bool named) {
    bool changed = false;
    if (m_name != name) {
      m_name = std::move(name);
      wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
      changed = true;
    }
    if (m_named != named) {
      m_named = named;
      changed = true;
    }
    if (m_index != index) {
      m_index = index;
      const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
      wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
      changed = true;
    }
    if (changed) {
      m_group->server()->scheduleIpcWorkspacesEvent();
    }
  }

  void Workspace::applyLayoutConfig(ResolvedLayoutConfig layoutConfig) {
    const bool centerFocusedChanged = m_layoutConfig.scrolling.centerFocused != layoutConfig.scrolling.centerFocused;
    const bool strutsChanged = m_layoutConfig.struts != layoutConfig.struts;
    const bool directionChanged = m_layoutConfig.scrolling.direction != layoutConfig.scrolling.direction;
    // When moving to/from scrolling, the current extents are remembered/restored.
    // These flags check whether those operations are required.
    const bool isMovingFromScroll =
        (m_layoutMode == LayoutMode::Scrolling && layoutConfig.mode != LayoutMode::Scrolling);
    const bool isMovingToScroll = (m_layoutMode != LayoutMode::Scrolling && layoutConfig.mode == LayoutMode::Scrolling);
    const bool fromFloating = m_layoutMode == LayoutMode::Floating;
    m_layoutConfig = std::move(layoutConfig);
    // The event payload is built when the idle runs, so scheduling here reports the mode this call installs, whether
    // it reconfigures the existing layout or replaces it below.
    m_group->server()->scheduleIpcWorkspacesEvent();
    if (m_layout != nullptr && m_layout->mode() == m_layoutConfig.mode) {
      m_layout->setConfig(&m_layoutConfig);
      m_layout->setConstraints(&viewLayoutConstraints);
      if (ScrollingLayout* scrolling = scrollingLayout(); scrolling != nullptr) {
        const int focusedColumn = m_focusedView != nullptr ? scrolling->columnOf(m_focusedView) : -1;
        const bool reconcile = centerFocusedChanged || strutsChanged || directionChanged;
        if (reconcile && focusedColumn >= 0) {
          scrolling->reconcileFocusedColumn(focusedColumn, scrollViewportExtent());
        } else if (reconcile) {
          clampScrollToRange();
        }
      }
      markArrange(true);
      return;
    }
    // The members leave one layout and join another; the next arrange carries them there from their current boxes.
    endLayoutMotion();
    // Remember each column's extent on its leading view; the other members join the new layout without one.
    if (isMovingFromScroll) {
      for (const Column& column : scrollingLayout()->columns()) {
        for (View* view : column.views) {
          view->m_savedScrollingExtentPx.reset();
          view->m_savedScrollingExtent.reset();
        }
        if (!column.views.empty()) {
          column.views.front()->m_savedScrollingExtent =
              column.savedWidthFrac > 0 ? column.savedWidthFrac : column.widthFrac;
        }
      }
    }
    std::vector<View*> tiledViews;
    for (View* view : m_views) {
      if (m_layout != nullptr && m_layout->columnOf(view) >= 0) {
        tiledViews.push_back(view);
      }
    }
    if (m_layoutConfig.mode == LayoutMode::Floating) {
      for (View* view : tiledViews) {
        view->floatForLayout();
      }
      tiledViews.clear();
    } else {
      for (View* view : tiledViews) {
        m_layout->removeView(view);
      }
    }
    m_layoutMode = m_layoutConfig.mode;
    m_layout = createLayout(m_layoutMode);
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    for (View* view : tiledViews) {
      m_layout->insertView(view, static_cast<int>(m_layout->columns().size()));
    }
    if (fromFloating) {
      const auto floats = m_floatingStack;
      for (View* view : floats) {
        if (view->layoutFloated() && view->floating() && !view->pinned() && view->mapped()) {
          view->setFloating(false, false);
        }
      }
    }
    // Restore previous extents
    if (isMovingToScroll) {
      ScrollingLayout* scrolling = scrollingLayout();
      for (int i = 0; i < static_cast<int>(scrolling->columns().size()); ++i) {
        View* view = scrolling->columns()[static_cast<size_t>(i)].views.front();
        if (view->m_savedScrollingExtentPx) {
          scrolling->setWidthFromPixels(i, scrollViewportExtent(), *view->m_savedScrollingExtentPx);
        } else if (view->m_savedScrollingExtent) {
          scrolling->setWidthFraction(i, *view->m_savedScrollingExtent);
        }
        view->m_savedScrollingExtentPx.reset();
        view->m_savedScrollingExtent.reset();
      }
      if (m_focusedView != nullptr) {
        scrolling->ensureVisible(scrolling->columnOf(m_focusedView), scrollViewportExtent());
      }
    }
    markArrange();
  }

  WorkspaceGroup::WorkspaceGroup(Server& server, Output& output) : m_server(&server), m_output(&output) {
    m_server->registerAnimatable(this);
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    m_handle = wlr_ext_workspace_group_handle_v1_create(manager, kGroupCaps);
    m_handle->data = this;
    wlr_ext_workspace_group_handle_v1_output_enter(m_handle, m_output->wlr());

    const OutputIdentity identity = m_output->identity();
    m_workspaceAxis = resolveWorkspaceAxis(config(), identity);
    auto resolved = resolveWorkspacesForOutput(config(), identity);
    m_dynamic = resolved.dynamic;
    m_omittedConfiguredNames = resolved.omittedNamed;
    if (m_omittedConfiguredNames > 0) {
      kLog.error(
          "{} named workspace declarations matching {} exceed the output limit and were omitted",
          m_omittedConfiguredNames, identity.connector.empty() ? "output" : identity.connector
      );
    }
    const size_t count = resolved.workspaces.size();
    m_workspaces.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      m_workspaces.push_back(createConfiguredWorkspace(std::move(resolved.workspaces[i]), i));
    }

    activate(m_workspaces.front().get());
    kLog.info(
        "workspace group for {} with {} workspaces", identity.connector.empty() ? "output" : identity.connector, count
    );
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
    m_server->scheduleIpcWorkspacesEvent();
  }
  std::string WorkspaceGroup::nextWorkspaceId() {
    const std::string_view connector = m_output->identity().connector;
    return std::format("{}:{}", connector.empty() ? "output" : connector, m_nextHandleSerial++);
  }

  std::unique_ptr<Workspace> WorkspaceGroup::createConfiguredWorkspace(ResolvedWorkspace workspace, size_t index) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    std::string id = nextWorkspaceId();
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id.c_str(), kWorkspaceCaps);
    // Group construction and the dynamic append/prepend/insert paths all funnel through here, and the payload is read
    // at idle time, after the caller has pushed the workspace into the list.
    m_server->scheduleIpcWorkspacesEvent();
    return std::make_unique<Workspace>(
        *this, handle, std::move(id), std::move(workspace.name), index, workspace.named, std::move(workspace.layout)
    );
  }

  Workspace* WorkspaceGroup::appendDynamicWorkspace() {
    const size_t index = m_workspaces.size();
    std::string name = std::to_string(index + 1);
    const OutputIdentity identity = m_output->identity();
    ResolvedLayoutConfig layout = resolveUnnamedWorkspaceLayout(config(), identity, index);
    auto workspace = createConfiguredWorkspace({std::move(name), false, std::move(layout)}, index);
    Workspace* result = workspace.get();
    m_workspaces.push_back(std::move(workspace));
    return result;
  }

  Workspace* WorkspaceGroup::prependDynamicWorkspace() {
    const std::string name = "1";
    const OutputIdentity identity = m_output->identity();
    ResolvedLayoutConfig layout = resolveUnnamedWorkspaceLayout(config(), identity, 0);
    auto workspace = createConfiguredWorkspace({name, false, std::move(layout)}, 0);
    Workspace* result = workspace.get();
    m_workspaces.insert(m_workspaces.begin(), std::move(workspace));
    return result;
  }

  void WorkspaceGroup::refreshWorkspaceMetadata() {
    for (size_t index = 0; index < m_workspaces.size(); ++index) {
      Workspace* workspace = m_workspaces[index].get();
      const bool named = workspace->named();
      const std::string name = named ? workspace->name() : std::to_string(index + 1);
      ResolvedLayoutConfig layout = workspaceLayout(*workspace, index);
      // Keep a runtime layout switch across structural changes to a dynamic
      // group, while allowing numeric workspace rules to follow the new index.
      if (const std::optional<LayoutMode> overrideMode = workspace->layoutModeOverride()) {
        layout.mode = *overrideMode;
      }
      if (workspace->layoutConfig() != layout) {
        workspace->applyLayoutConfig(std::move(layout));
      }
      if (workspace->name() != name || workspace->index() != index) {
        workspace->rename(name, index, named);
      }
    }
    if (m_previous == m_active) {
      m_previous = nullptr;
    }
  }

  Workspace* WorkspaceGroup::insertDynamicWorkspace(size_t index) {
    if (!m_dynamic || m_output == nullptr || m_output->wlr() == nullptr || m_workspaces.size() >= kMaxWorkspaces) {
      return nullptr;
    }
    index = std::min(index, m_workspaces.size());
    const std::string name = std::to_string(index + 1);
    const OutputIdentity identity = m_output->identity();
    ResolvedLayoutConfig layout = resolveUnnamedWorkspaceLayout(config(), identity, index);
    auto workspace = createConfiguredWorkspace({name, false, std::move(layout)}, index);
    Workspace* result = workspace.get();
    m_workspaces.insert(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index), std::move(workspace));
    refreshWorkspaceMetadata();
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
    if (m_dynamic) {
      if (direction > 0) {
        const bool targetIsTrailingEmpty = static_cast<size_t>(target) == m_workspaces.size() - 1
            && !m_workspaces[static_cast<size_t>(target)]->named()
            && !m_workspaces[static_cast<size_t>(target)]->hasViews();
        if (targetIsTrailingEmpty) {
          return false;
        }
      } else if (config().workspaces.emptyAbove) {
        const bool targetIsLeadingEmpty = target == 0 && !m_workspaces[0]->named() && !m_workspaces[0]->hasViews();
        if (targetIsLeadingEmpty) {
          return false;
        }
      }
    }
    slideFinish();
    std::swap(m_workspaces[index], m_workspaces[static_cast<size_t>(target)]);
    if (m_dynamic) {
      reconcileDynamic();
      return true;
    }
    for (const size_t slot : {index, static_cast<size_t>(target)}) {
      Workspace* moved = m_workspaces[slot].get();
      const bool named = moved->named();
      const std::string name = named ? moved->name() : std::to_string(slot + 1);
      ResolvedLayoutConfig layout = workspaceLayout(*moved, slot);
      if (const std::optional<LayoutMode> overrideMode = moved->layoutModeOverride()) {
        layout.mode = *overrideMode;
      }
      if (moved->layoutConfig() != layout) {
        moved->applyLayoutConfig(std::move(layout));
      }
      moved->rename(name, slot, named);
    }
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }
    return true;
  }

  void WorkspaceGroup::reconcileDynamicNames(const std::vector<ResolvedWorkspace>& resolved) {
    std::vector<std::string> desired;
    desired.reserve(resolved.size());
    for (const ResolvedWorkspace& workspace : resolved) {
      if (workspace.named) {
        desired.push_back(workspace.name);
      }
    }

    std::vector<std::string> claimed;
    claimed.reserve(desired.size());
    for (const auto& workspace : m_workspaces) {
      if (workspace->displacedWorkspace() || !workspace->named()) {
        continue;
      }
      const bool wanted = std::ranges::find(desired, workspace->name()) != desired.end();
      const bool duplicate = std::ranges::find(claimed, workspace->name()) != claimed.end();
      if (wanted && !duplicate) {
        claimed.push_back(workspace->name());
        continue;
      }
      workspace->rename(std::to_string(workspace->index() + 1), workspace->index(), false);
    }

    // Declarations added during a live reload enter at the top of
    // the dynamic list. Preserve declaration order among names added together,
    // and keep the optional leading unnamed sentinel first.
    for (const std::string& entry : std::views::reverse(desired)) {
      if (std::ranges::find(claimed, entry) != claimed.end()) {
        continue;
      }

      if (m_workspaces.size() >= kMaxWorkspaces) {
        Workspace* leadingSentinel = nullptr;
        if (config().workspaces.emptyAbove
            && !m_workspaces.empty()
            && !m_workspaces.front()->named()
            && !m_workspaces.front()->hasViews()) {
          leadingSentinel = m_workspaces.front().get();
        }
        Workspace* trailingSentinel = nullptr;
        if (!m_workspaces.empty() && !m_workspaces.back()->named() && !m_workspaces.back()->hasViews()) {
          trailingSentinel = m_workspaces.back().get();
        }
        auto reusable = std::ranges::find_if(m_workspaces, [&](const auto& workspace) {
          return workspace.get() != leadingSentinel
              && workspace.get() != trailingSentinel
              && !workspace->displacedWorkspace()
              && !workspace->named()
              && !workspace->hasViews()
              && !workspace->launchReserved();
        });
        if (reusable == m_workspaces.end()) {
          // The declaration remains pending until an ordinary anonymous
          // workspace becomes empty. Sentinels are never repurposed as names.
          continue;
        }
        Workspace* workspace = reusable->get();
        ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), m_output->identity(), entry, workspace->index());
        workspace->rename(entry, workspace->index(), true);
        workspace->applyLayoutConfig(std::move(layout));
        claimed.push_back(entry);
        continue;
      }

      size_t index = 0;
      if (config().workspaces.emptyAbove
          && !m_workspaces.empty()
          && !m_workspaces.front()->named()
          && !m_workspaces.front()->hasViews()) {
        index = 1;
      }
      ResolvedLayoutConfig layout = resolveWorkspaceLayout(config(), m_output->identity(), entry, index);
      auto workspace = createConfiguredWorkspace({entry, true, std::move(layout)}, index);
      m_workspaces.insert(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index), std::move(workspace));
      claimed.push_back(entry);
    }
  }

  void WorkspaceGroup::reconcileInventory() {
    slideFinish();
    const OutputIdentity identity = m_output->identity();
    auto resolvedSet = resolveWorkspacesForOutput(config(), identity);
    m_dynamic = resolvedSet.dynamic;

    if (m_dynamic) {
      reconcileDynamic();
      kLog.info(
          "reconciled {} to {} workspaces (0 windows relocated)",
          identity.connector.empty() ? "output" : identity.connector, m_workspaces.size()
      );
      return;
    }
    m_omittedConfiguredNames = 0;

    auto& resolved = resolvedSet.workspaces;
    auto old = std::move(m_workspaces);
    std::vector<std::unique_ptr<Workspace>> temporary;
    for (auto& workspace : old) {
      if (workspace->displacedWorkspace()) {
        temporary.push_back(std::move(workspace));
      }
    }
    std::erase_if(old, [](const auto& workspace) { return workspace == nullptr; });
    std::vector<std::unique_ptr<Workspace>> next(resolved.size());

    // Preserve workspace identity by name before using position as a fallback.
    for (size_t i = 0; i < resolved.size(); ++i) {
      const auto match = std::ranges::find_if(old, [&](const auto& workspace) {
        return resolved[i].named && workspace != nullptr && workspace->named() && workspace->name() == resolved[i].name;
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
        next[i]->rename(resolved[i].name, i, resolved[i].named);
      } else {
        next[i] = createConfiguredWorkspace(std::move(resolved[i]), i);
      }
    }

    size_t excessTemporary = 0;
    for (auto& workspace : temporary) {
      if ((workspace->hasViews() || workspace->launchReserved()) && next.size() < kMaxWorkspaces) {
        const size_t index = next.size();
        const auto& origin = workspace->displacedWorkspace();
        workspace->rename(
            origin->workspaceNamed ? origin->workspaceName : std::to_string(index + 1), index, origin->workspaceNamed
        );
        next.push_back(std::move(workspace));
      } else {
        excessTemporary += workspace->hasViews() ? 1 : 0;
        old.push_back(std::move(workspace));
      }
    }
    if (excessTemporary > 0) {
      kLog.warn(
          "{} temporary workspaces no longer fit on {}; their windows retain their disconnected homes", excessTemporary,
          identity.connector.empty() ? "output" : identity.connector
      );
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
      Workspace* fallback = m_workspaces[std::min(removed->index(), resolved.size() - 1)].get();
      if (removed.get() == m_active) {
        removed->setActive(false);
        replacementActive = fallback;
      }
      // setWorkspace() erases from the source workspace's view list, so relocate from a snapshot: allViews() hands
      // back the live vector and iterating it here would invalidate the iterator on the first move.
      const std::vector<View*> relocating = removed->allViews();
      for (View* view : relocating) {
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
    refreshWorkspaceMetadata();
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }

    if (relocatedViews > 0 || !activeSurvives) {
      m_server->cursor()->clearConstraint();
      m_server->refocus();
    }
    kLog.info(
        "reconciled {} to {} workspaces ({} windows relocated)",
        identity.connector.empty() ? "output" : identity.connector, m_workspaces.size(), relocatedViews
    );
  }

  void WorkspaceGroup::refreshWorkspaceAxis() {
    const WorkspaceAxis axis = resolveWorkspaceAxis(config(), m_output->identity());
    if (axis == m_workspaceAxis) {
      return;
    }
    // Settle any live slide while the old axis still describes the offsets it installed.
    slideFinish();
    m_workspaceAxis = axis;
  }

  void WorkspaceGroup::refreshLayouts() {
    refreshWorkspaceAxis();
    for (const auto& workspace : m_workspaces) {
      // A config reload reasserts the configured mode, dropping any runtime
      // workspace-set-layout override.
      workspace->clearLayoutModeOverride();
      ResolvedLayoutConfig layout = workspaceLayout(*workspace, workspace->index());
      if (workspace->layoutConfig() != layout) {
        workspace->applyLayoutConfig(std::move(layout));
      } else {
        // Bar text such as title_format and max_tabs moves no box, so no arrange redraws the bars that show it.
        workspace->tabs().sync();
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

  bool WorkspaceGroup::pruneDisplacedWorkspaces() {
    bool removed = false;
    for (size_t index = m_workspaces.size(); index-- > 0;) {
      Workspace* workspace = m_workspaces[index].get();
      if (!workspace->displacedWorkspace() || workspace->hasViews() || workspace->launchReserved()) {
        continue;
      }
      if (m_active == workspace) {
        auto replacement =
            std::ranges::find_if(m_workspaces, [](const auto& candidate) { return !candidate->displacedWorkspace(); });
        if (replacement == m_workspaces.end()) {
          replacement = std::ranges::find_if(m_workspaces, [workspace](const auto& candidate) {
            return candidate.get() != workspace && (candidate->hasViews() || candidate->launchReserved());
          });
        }
        workspace->setActive(false);
        m_active = replacement != m_workspaces.end() ? replacement->get() : nullptr;
        if (m_active != nullptr) {
          m_active->setActive(true);
        }
      }
      if (m_previous == workspace) {
        m_previous = nullptr;
      }
      m_workspaces.erase(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index));
      removed = true;
      m_server->scheduleIpcWorkspacesEvent();
    }
    return removed;
  }

  void WorkspaceGroup::reconcileDynamic() {
    if (slideActive()) {
      return;
    }

    const bool removedTemporary = pruneDisplacedWorkspaces();
    const auto finishInventoryChange = [this] {
      refreshWorkspaceMetadata();
      if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
        overview->onWorkspaceInventoryChanged(this);
      }
    };
    if (!m_dynamic) {
      if (removedTemporary) {
        finishInventoryChange();
      }
      return;
    }

    const OutputIdentity identity = m_output->identity();
    ResolvedWorkspaceSet resolved = resolveWorkspacesForOutput(config(), identity);
    // Config is committed before reload side effects run. Overview teardown can therefore reach this method while
    // m_dynamic still reflects the old inventory type; leave the transition to reconcileInventory().
    if (!resolved.dynamic) {
      if (removedTemporary) {
        finishInventoryChange();
      }
      return;
    }
    if (resolved.omittedNamed != m_omittedConfiguredNames) {
      if (resolved.omittedNamed > 0) {
        kLog.error(
            "{} named workspace declarations matching {} exceed the output limit and were omitted",
            resolved.omittedNamed, identity.connector.empty() ? "output" : identity.connector
        );
      } else if (m_omittedConfiguredNames > 0) {
        kLog.info(
            "all named workspace declarations now fit on {}", identity.connector.empty() ? "output" : identity.connector
        );
      }
      m_omittedConfiguredNames = resolved.omittedNamed;
    }
    reconcileDynamicNames(resolved.workspaces);

    // Dynamic groups keep their active empty workspace until the user leaves it. It can serve as the trailing sentinel
    // when every workspace after it is another anonymous empty, but not when a named or occupied workspace follows it.
    const bool emptyAbove = config().workspaces.emptyAbove;
    const size_t minimum = resolveDynamicWorkspaceMinimum(config(), identity);
    Workspace* frontKeeper = nullptr;
    if (emptyAbove && !m_workspaces.empty() && !m_workspaces.front()->named() && !m_workspaces.front()->hasViews()) {
      frontKeeper = m_workspaces.front().get();
    }

    // The optional leading empty and the trailing empty are distinct inventory entries, including before the first
    // view maps. A leading empty therefore cannot also serve as the trailing keeper.
    Workspace* activeKeeper = nullptr;
    if (m_active != nullptr && !m_active->named() && !m_active->hasViews()) {
      activeKeeper = m_active;
    }
    Workspace* backKeeper = nullptr;
    if (activeKeeper != nullptr && activeKeeper != frontKeeper) {
      const auto active =
          std::ranges::find_if(m_workspaces, [&](const auto& workspace) { return workspace.get() == activeKeeper; });
      const bool substantiveWorkspaceFollows = active != m_workspaces.end()
          && std::ranges::any_of(active + 1, m_workspaces.end(),
                                 [](const auto& workspace) { return workspace->named() || workspace->hasViews(); });
      if (!substantiveWorkspaceFollows) {
        backKeeper = activeKeeper;
      }
    }
    if (backKeeper == nullptr
        && !m_workspaces.empty()
        && !m_workspaces.back()->named()
        && !m_workspaces.back()->hasViews()
        && m_workspaces.back().get() != frontKeeper) {
      backKeeper = m_workspaces.back().get();
    }

    for (size_t index = m_workspaces.size(); index-- > 0;) {
      // min_workspaces is a floor on the count, not on a position. Pruning runs from the end, so it stops as soon as
      // the group would shrink past the floor and the surviving empties are the lowest-numbered ones.
      if (m_workspaces.size() <= minimum) {
        break;
      }
      Workspace* workspace = m_workspaces[index].get();
      if (!workspace->named()
          && !workspace->displacedWorkspace()
          && !workspace->hasViews()
          && !workspace->launchReserved()
          && workspace != activeKeeper
          && workspace != backKeeper
          && workspace != frontKeeper) {
        if (m_previous == workspace) {
          m_previous = nullptr;
        }
        m_workspaces.erase(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index));
        m_server->scheduleIpcWorkspacesEvent();
      }
    }
    // Filling the floor appends empty workspaces, so the last of them is the trailing empty this group needs.
    while (m_workspaces.size() < minimum) {
      backKeeper = appendDynamicWorkspace();
    }
    if ((m_workspaces.empty()
         || m_workspaces.back()->named()
         || m_workspaces.back()->hasViews()
         || m_workspaces.back().get() == frontKeeper)
        && m_workspaces.size() < kMaxWorkspaces) {
      appendDynamicWorkspace();
    }
    if (emptyAbove && frontKeeper == nullptr && m_workspaces.size() < kMaxWorkspaces) {
      prependDynamicWorkspace();
    }

    if (m_active == nullptr && !m_workspaces.empty()) {
      m_active = m_workspaces.front().get();
      m_active->setActive(true);
    }
    finishInventoryChange();
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
    Workspace* displaced = nullptr;
    bool ambiguous = false;
    for (const auto& entry : m_workspaces) {
      if (entry->named() && entry->name() == name) {
        if (!entry->displacedWorkspace()) {
          return entry.get();
        }
        if (displaced != nullptr) {
          ambiguous = true;
        }
        displaced = entry.get();
      }
    }
    return ambiguous ? nullptr : displaced;
  }

  Workspace* WorkspaceGroup::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    for (const auto& entry : m_workspaces) {
      if (entry->handle() == handle) {
        return entry.get();
      }
    }
    return nullptr;
  }

  void WorkspaceGroup::beginReveal() {
    if (config().animation.workspaces.style != "reveal") {
      return;
    }
    auto& registry = effectRegistry();
    if (auto* shader = registry.lifecycleEffect(AnimationEvent::Workspaces)) {
      m_slide.reveal = fx_effect_shader_ref(shader);
      m_slide.transition = beginAnimationTransition();
      if (const auto* preset = registry.animationPreset(AnimationEvent::Workspaces)) {
        m_slide.revealPreset = *preset;
      }
    }
  }

  Workspace* WorkspaceGroup::revealIncoming(double progress) const {
    Workspace* incoming = progress < 0 ? m_slide.previous : m_slide.next;
    return progress == 0 && incoming == nullptr ? m_slide.previous : incoming;
  }

  wlr_scene_node* WorkspaceGroup::revealRoot(bool incoming) const {
    auto* workspace = incoming ? revealIncoming(m_slide.progress) : m_slide.base;
    return revealActive() && workspace != nullptr ? &workspace->tileShadowLayer()->node.parent->node : nullptr;
  }

  void WorkspaceGroup::refreshReveal() {
    if (m_slide.reveal == nullptr) {
      return;
    }
    const auto& animation = config().animation;
    if (!animation.enabled || !animation.workspaces.enabled || animation.workspaces.style != "reveal") {
      slideFinish();
      return;
    }
    const double progress = m_slide.progress;
    Workspace* incoming = revealIncoming(progress);
    const float axis = incoming == m_slide.previous ? -1.0F : 1.0F;
    for (Workspace* workspace : {m_slide.base, m_slide.previous, m_slide.next}) {
      if (workspace == nullptr) {
        continue;
      }
      for (auto* tree : {workspace->tileShadowLayer()->node.parent, workspace->fullscreenTree()}) {
        fx_animation_parameters parameters{};
        parameters.progress = static_cast<float>(std::clamp(std::abs(progress), 0.0, 1.0));
        parameters.linear_progress = parameters.progress;
        parameters.direction = workspace == m_slide.base ? -1.0F : 1.0F;
        parameters.transition_id = m_slide.transition.id;
        std::ranges::copy(m_slide.transition.seed, parameters.random_seed);
        if (m_slide.revealPreset) {
          auto& registry = effectRegistry();
          registry.fillTimeUniforms(parameters, registry.clockSeconds(), *m_slide.revealPreset, m_slide.reveal);
        }
        if (auto* direction = fx_parameters_add_uniform(&parameters, "umbriel_workspace_axis", FX_UNIFORM_VEC2, 1)) {
          direction->floats[0] = m_workspaceAxis == WorkspaceAxis::Horizontal ? axis : 0.0F;
          direction->floats[1] = m_workspaceAxis == WorkspaceAxis::Vertical ? axis : 0.0F;
        }
        wlr_scene_node_set_animation(&tree->node, FX_SLOT_WORKSPACES, m_slide.reveal, &parameters);
        if (!wlr_scene_node_set_animation_isolation(&tree->node, FX_SLOT_WORKSPACES, workspace)) {
          abandonReveal();
          return;
        }
      }
    }
  }

  void WorkspaceGroup::cancelReveal() {
    if (m_slide.reveal != nullptr) {
      slideFinish();
    }
  }

  void WorkspaceGroup::abandonReveal() {
    if (m_slide.reveal != nullptr) {
      for (Workspace* workspace : {m_slide.base, m_slide.previous, m_slide.next}) {
        if (workspace != nullptr) {
          wlr_scene_node_set_animation(
              &workspace->tileShadowLayer()->node.parent->node, FX_SLOT_WORKSPACES, nullptr, nullptr
          );
          wlr_scene_node_set_animation(&workspace->fullscreenTree()->node, FX_SLOT_WORKSPACES, nullptr, nullptr);
        }
      }
      fx_effect_shader_unref(m_slide.reveal);
      m_slide.reveal = nullptr;
      m_slide.revealPreset.reset();
      slideApply(m_slide.progress);
    }
  }

  void WorkspaceGroup::slideFinish() {
    abandonReveal();
    m_slideAnim.snap(0.0);
    if (m_slide.base != nullptr) {
      m_slide.base->endSwitchTransition();
    }
    if (m_slide.previous != nullptr) {
      m_slide.previous->endSwitchTransition();
    }
    if (m_slide.next != nullptr) {
      m_slide.next->endSwitchTransition();
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
    const double extent = m_workspaceAxis == WorkspaceAxis::Horizontal ? box.width : box.height;
    if (extent <= 0) {
      return false;
    }
    // A settle still running between neighbours is heading for a whole step, so where it is on screen minus that step
    // is where it sits relative to the workspace that is now active. A gesture that starts now carries on from there
    // rather than snapping the slide to the end of the animation first. A jump across several workspaces has a
    // non-neighbour on screen that the gesture cannot show, so that one snaps.
    const bool carry = m_slide.base != nullptr
        && std::max(m_slide.base->index(), m_active->index()) - std::min(m_slide.base->index(), m_active->index()) <= 1;
    const double start = carry ? m_slideAnim.current() - m_slideAnim.target() : 0.0;
    slideFinish();
    m_slide.base = m_active;
    beginReveal();
    m_slide.extent = extent;
    m_slide.progress = start;
    const size_t idx = m_active->index();
    m_slide.previous = (includePrev && idx > 0) ? workspaceAt(idx - 1) : nullptr;
    m_slide.next = includeNext ? workspaceAt(idx + 1) : nullptr;
    m_slide.base->beginSwitchTransition();
    if (m_slide.previous != nullptr) {
      m_slide.previous->beginSwitchTransition();
      m_slide.previous->showSwitchViews();
      m_slide.previous->arrange(false);
    }
    if (m_slide.next != nullptr) {
      m_slide.next->beginSwitchTransition();
      m_slide.next->showSwitchViews();
      m_slide.next->arrange(false);
    }
    slideApply(start);
    return true;
  }

  void WorkspaceGroup::slideApply(double progress) {
    m_slide.progress = progress;
    const double extent = m_slide.extent;
    // Increasing workspace index moves outgoing content toward negative coordinates
    // on the group's axis; the other coordinate stays at rest.
    const bool horizontal = m_workspaceAxis == WorkspaceAxis::Horizontal;
    const Workspace* incoming = m_slide.reveal != nullptr ? revealIncoming(progress) : nullptr;
    const auto offset = [&](Workspace* workspace, double displacement) {
      if (m_slide.reveal != nullptr && (workspace == m_slide.base || workspace == incoming)) {
        displacement = 0.0;
      }
      workspace->setSlideOffset(horizontal ? displacement : 0.0, horizontal ? 0.0 : displacement);
    };
    offset(m_slide.base, -progress * extent);
    if (m_slide.next != nullptr) {
      offset(m_slide.next, (1.0 - progress) * extent);
    }
    if (m_slide.previous != nullptr) {
      offset(m_slide.previous, (-1.0 - progress) * extent);
    }
    wlr_output_schedule_frame(m_output->wlr());
  }

  void WorkspaceGroup::slideSettle(int delta, double velocity) {
    Workspace* target = nullptr;
    if (delta < 0) {
      target = m_slide.previous;
    } else if (delta > 0) {
      target = m_slide.next;
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
      Workspace* unused = (delta > 0) ? m_slide.previous : m_slide.next;
      if (unused != nullptr) {
        unused->endSwitchTransition();
      }
    }
    kLog.debug("slide workspace {} → {} on {}", m_slide.base->name(), target->name(), m_output->wlr()->name);
    const auto& animation = config().animation;
    const auto& workspaces = animation.workspaces;
    if (!animation.enabled || !workspaces.enabled) {
      m_slideAnim.snap(delta);
      slideApply(delta);
      slideFinish();
      reconcileDynamic();
      return;
    }
    m_slideAnim.snap(m_slide.progress);
    if (workspaces.curve.easing == Easing::Spring) {
      // A spring carries the release speed, so the slide keeps moving the way the fingers were instead of stopping
      // dead at the release and starting over from rest.
      m_slideAnim.settleSpring(static_cast<double>(delta), workspaces.curve.spring, velocity);
    } else {
      m_slideAnim.retarget(static_cast<double>(delta), workspaces.durationMs, workspaces.curve);
    }
    wlr_output_schedule_frame(m_output->wlr());
  }

  bool WorkspaceGroup::tickAnimations(double nowMsec) {
    const bool ticked = m_slideAnim.tick(nowMsec);
    if (ticked && m_slideAnim.animating() && m_slideAnim.curve().easing == Easing::Spring) {
      // One step is `extent` pixels, so a spring that can no longer move a pixel has already landed there: end it
      // now rather than letting it creep at amplitudes the pixel grid rounds away.
      static_cast<void>(m_slideAnim.finishSpringTail(m_slide.extent));
    }
    if (config().animation.workspaces.style == "slide") {
      bindAnimationEffect(&m_output->viewRoot()->node, AnimationEvent::Workspaces, m_slideAnim);
    } else {
      wlr_scene_node_set_animation(&m_output->viewRoot()->node, FX_SLOT_WORKSPACES, nullptr, nullptr);
    }
    bool active = false;
    if (ticked) {
      slideApply(m_slideAnim.current());
      if (m_slideAnim.animating()) {
        active = true;
      } else {
        slideFinish();
        reconcileDynamic();
      }
    }
    // After reconcileDynamic, which may have dropped an emptied workspace.
    for (const auto& workspace : m_workspaces) {
      active = workspace->tickLayoutMotion(nowMsec) || active;
    }
    return active;
  }

  bool WorkspaceGroup::hasActiveAnimations() const {
    return m_slideAnim.animating()
        || std::ranges::any_of(m_workspaces, [](const auto& workspace) { return workspace->layoutMotionActive(); });
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
    const bool doAnimate = animate
        && config().animation.enabled
        && config().animation.workspaces.enabled
        && m_active != nullptr
        && !m_server->sessionLocked()
        && !overviewActive;
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
    const double extent = m_workspaceAxis == WorkspaceAxis::Horizontal ? box.width : box.height;
    if (extent <= 0) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = workspace;
      m_active->setActive(true);
      reconcileDynamic();
      return;
    }
    const int sign = workspace->index() > m_active->index() ? 1 : -1;
    m_slide.base = m_active;
    beginReveal();
    m_slide.extent = extent;
    m_slide.progress = 0;
    if (sign > 0) {
      m_slide.next = workspace;
    } else {
      m_slide.previous = workspace;
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

  Workspace* WorkspaceGroup::createWorkspace(const char* /*name*/) {
    if (!m_dynamic) {
      return nullptr;
    }
    const auto empty = std::ranges::find_if(m_workspaces.rbegin(), m_workspaces.rend(), [](const auto& workspace) {
      return !workspace->displacedWorkspace() && !workspace->named() && !workspace->hasViews();
    });
    if (empty != m_workspaces.rend()) {
      kLog.debug("using empty dynamic workspace for create request on {}", m_output->wlr()->name);
      return empty->get();
    }
    return insertDynamicWorkspace(m_workspaces.size());
  }

  Workspace* WorkspaceGroup::transferDestination() {
    if (m_dynamic) {
      return createWorkspace(nullptr);
    }
    const auto empty = std::ranges::find_if(m_workspaces.rbegin(), m_workspaces.rend(), [](const auto& workspace) {
      return !workspace->displacedWorkspace() && !workspace->hasViews();
    });
    return empty != m_workspaces.rend() ? empty->get() : nullptr;
  }

  Workspace* WorkspaceGroup::displacedDestination(std::shared_ptr<const DisplacedWorkspace> origin) {
    if (origin == nullptr || m_output == nullptr || m_output->wlr() == nullptr) {
      return nullptr;
    }
    const auto existing = std::ranges::find_if(m_workspaces, [&origin](const auto& workspace) {
      const auto& saved = workspace->displacedWorkspace();
      return saved != nullptr
          && saved->outputName == origin->outputName
          && saved->workspaceName == origin->workspaceName
          && saved->workspaceIndex == origin->workspaceIndex
          && saved->workspaceNamed == origin->workspaceNamed;
    });
    if (existing != m_workspaces.end()) {
      return existing->get();
    }
    if (m_workspaces.size() >= kMaxWorkspaces) {
      return nullptr;
    }
    const size_t index = m_workspaces.size();
    auto workspace = createConfiguredWorkspace(
        {origin->workspaceNamed ? origin->workspaceName : std::to_string(index + 1), origin->workspaceNamed,
         displacedWorkspaceLayout(*origin)},
        index
    );
    workspace->m_displacedWorkspace = std::move(origin);
    if (workspace->m_displacedWorkspace->configGeneration == configStore().generation()) {
      workspace->m_layoutModeOverride = workspace->m_displacedWorkspace->layoutModeOverride;
    }
    Workspace* result = workspace.get();
    m_workspaces.push_back(std::move(workspace));
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceInventoryChanged(this);
    }
    return result;
  }

} // namespace umbriel
