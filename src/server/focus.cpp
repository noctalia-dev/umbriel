#include "server/focus.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/node.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("focus");

    constexpr std::string_view focusReasonName(FocusReason reason) {
      switch (reason) {
      case FocusReason::Directional:
        return "directional";
      case FocusReason::PointerPress:
        return "pointer-press";
      case FocusReason::PointerHover:
        return "pointer-hover";
      case FocusReason::Grab:
        return "grab";
      case FocusReason::DragDrop:
        return "drag-drop";
      case FocusReason::Gesture:
        return "gesture";
      case FocusReason::Startup:
        return "startup";
      case FocusReason::XdgActivation:
        return "xdg-activation";
      case FocusReason::ForeignActivation:
        return "foreign-activation";
      }
      return "unknown";
    }
  } // namespace

  void FocusManager::focusView(View* view, FocusReason reason) {
    if (view == nullptr || m_server.sessionLocked()) {
      return;
    }

    // PointerHover gate: reject focus entirely when revealing would exceed the configured max scroll fraction. Must run
    // before any side effects (MRU, seat focus) so an over-limit hover focuses nothing, preserving the current behavior
    // where cursor.cpp skipped focusView altogether.
    if (reason == FocusReason::PointerHover && view->tiled()) {
      if (Workspace* workspace = view->workspace()) {
        const auto& maxScroll = config().input.focus.followsMouseMaxScroll;
        if (maxScroll && workspace->scrollFractionToReveal(view) > *maxScroll) {
          return;
        }
      }
    }

    if (Workspace* workspace = view->workspace()) {
      if (WorkspaceGroup* group = workspace->group(); group != nullptr && !view->pinned() && !workspace->active()) {
        const char* appId = view->toplevel()->app_id != nullptr ? view->toplevel()->app_id : "";
        // A group without a live output still has to activate; only the log's output name degrades.
        const Output* output = group->output();
        const char* outputName = output != nullptr && output->wlr() != nullptr ? output->wlr()->name : "<none>";
        const std::string_view current =
            group->active() != nullptr ? std::string_view(group->active()->name()) : std::string_view("<none>");
        kLog.debug(
            "workspace switch trigger=focus reason={} app_id='{}' output='{}' from='{}' to='{}'",
            focusReasonName(reason), appId, outputName, current, workspace->name()
        );
        group->activate(workspace);
      }
    }
    if (!view->onActiveWorkspace() && !view->pinned()) {
      return;
    }
    if (m_server.scratchpadManager() != nullptr) {
      m_server.scratchpadManager()->noteFocus(view);
    }

    m_server.registry().promote(view);
    view->setUrgent(false);

    if (reason == FocusReason::XdgActivation || reason == FocusReason::ForeignActivation) {
      view->applyDeferredUnfullscreen();
    }

    // Keep workspace focus while exclusive layer-shell holds the seat; refocus applies it later. Still clear activation
    // chrome so the previous window does not stay visually focused. Overview owns the seat the same way, but keeps the
    // chrome so card borders track the focused window; the keyboard enter replays when it closes.
    const bool overviewActive = m_server.overview() != nullptr && m_server.overview()->active();
    const bool seatAvailable = exclusiveKeyboardLayer() == nullptr;
    if (seatAvailable) {
      view->applySeatFocus(!overviewActive);
    } else {
      deactivateViews(nullptr);
    }
    Workspace* workspace = view->workspace();
    if (workspace != nullptr && (!view->pinned() || workspace->active())) {
      workspace->setFocusedView(view);
    }
    m_server.refreshOutputPolicies();
    if (overviewActive) {
      m_server.overview()->onFocusChanged();
    }
    // Pointer presses and compositor grabs already align focus with their target. Keyboard and activation-driven focus
    // can instead replace the scene beneath a stationary pointer, so let the next eligible hover refresh it once.
    if (reason != FocusReason::PointerHover
        && reason != FocusReason::PointerPress
        && reason != FocusReason::Grab
        && reason != FocusReason::DragDrop) {
      m_server.cursor()->invalidateHoverFocus();
    }

    // Derive reveal policy from the focus reason.
    if (workspace == nullptr || !view->tiled()) {
      return;
    }
    switch (reason) {
    case FocusReason::Directional:
    case FocusReason::PointerPress:
    case FocusReason::PointerHover:
    case FocusReason::DragDrop:
    case FocusReason::Startup:
    case FocusReason::XdgActivation:
    case FocusReason::ForeignActivation:
      workspace->activateFocusedColumn();
      workspace->markArrange(true);
      break;
    case FocusReason::Grab:
      // No reveal: the grab is about to move/detach the tile; revealing would
      // shift computed grab offsets and cause a visual jump.
      break;
    case FocusReason::Gesture:
      // No reveal: the finger already chose where the strip rests, and a reveal
      // would drag it somewhere else on release.
      break;
    }
  }

  void FocusManager::restoreActivatedViewKeyboardFocus() {
    if (m_server.sessionLocked() || exclusiveKeyboardLayer() != nullptr) {
      return;
    }
    // A pointer drag clears normal pointer focus even when keyboard focus returns to the same activated view. The
    // synthetic motion after release must still get one chance to select the drop target under the cursor.
    m_server.cursor()->invalidateHoverFocus();
    View* seatFocused = View::fromSurface(m_server.seat()->wlr()->keyboard_state.focused_surface);
    for (const auto& entry : m_server.registry().all()) {
      if (entry->mapped() && entry->activated() && (entry->onActiveWorkspace() || entry->pinned())) {
        if (entry.get() == seatFocused) {
          return;
        }
        focusView(entry.get(), FocusReason::DragDrop);
        return;
      }
    }
  }

  LayerSurface* FocusManager::exclusiveKeyboardLayer() const {
    for (const auto& entry : m_server.layerSurfaces()) {
      if (entry->exclusiveKeyboard()) {
        return entry.get();
      }
    }
    return nullptr;
  }

  bool FocusManager::retainCurrentKeyboardFocus() {
    if (m_server.overview() != nullptr && m_server.overview()->active()) {
      return false;
    }

    wlr_surface* focusedSurface = m_server.seat()->wlr()->keyboard_state.focused_surface;
    if (focusedSurface == nullptr || !focusedSurface->mapped) {
      return false;
    }

    const auto outputIsUsable = [this](Output* output) {
      return output != nullptr
          && output->wlr()->enabled
          && wlr_output_layout_get(m_server.outputLayout(), output->wlr()) != nullptr;
    };

    if (View* view = View::fromSurface(focusedSurface)) {
      if (!view->mapped() || (!view->onActiveWorkspace() && !view->pinned())) {
        return false;
      }

      Output* output = nullptr;
      if (ScratchpadManager* scratchpad = m_server.scratchpadManager();
          scratchpad != nullptr && scratchpad->contains(view)) {
        output = scratchpad->outputFor(view);
      } else if (Workspace* workspace = view->workspace(); workspace != nullptr && workspace->group() != nullptr) {
        output = workspace->group()->output();
      } else if (view->pinned()) {
        output = view->currentOutput();
      }
      if (!outputIsUsable(output)) {
        return false;
      }

      // The seat surface is already correct. Refresh only the owner's chrome
      // and stacking so a popup or input grab keeps the exact focused surface.
      view->applySeatFocus(false);
      if (Workspace* workspace = view->workspace(); workspace != nullptr && (!view->pinned() || workspace->active())) {
        workspace->setFocusedView(view);
      }
      m_server.refreshOutputPolicies();
      return true;
    }

    if (LayerSurface* layer = LayerSurface::fromSurface(focusedSurface);
        layer != nullptr && layer->acceptsKeyboard() && outputIsUsable(layer->output())) {
      // Preserve an on-demand layer and any popup grab it currently owns.
      m_server.deactivateViews(nullptr);
      m_server.refreshOutputPolicies();
      return true;
    }
    return false;
  }

  void FocusManager::refocus() {
    if (m_server.sessionLocked()) {
      return;
    }
    if (LayerSurface* layer = exclusiveKeyboardLayer()) {
      wlr_surface* focusedSurface = m_server.seat()->wlr()->keyboard_state.focused_surface;
      if (focusedSurface != nullptr && focusedSurface->mapped && LayerSurface::fromSurface(focusedSurface) == layer) {
        // Infrastructure changes must not replace this layer's popup with its
        // root surface. Explicit interactions still call focus() directly.
        m_server.deactivateViews(nullptr);
        m_server.refreshOutputPolicies();
        return;
      }
      layer->focus();
      return;
    }
    if (retainCurrentKeyboardFocus()) {
      return;
    }
    refocusFallback(nullptr);
  }

  void FocusManager::refocus(Output* preferred) {
    if (m_server.sessionLocked()) {
      return;
    }
    if (LayerSurface* layer = exclusiveKeyboardLayer()) {
      layer->focus();
      return;
    }
    refocusFallback(preferred);
  }

  void FocusManager::refocusFallback(Output* preferred) {
    const auto focusMappedOn = [this](Output* output) -> bool {
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return false;
      }
      Workspace* workspace = output->workspaceGroup()->active();
      if (workspace == nullptr) {
        return false;
      }
      if (View* focused = workspace->focusedView()) {
        if (focused->mapped() && focused->onActiveWorkspace()) {
          focusView(focused);
          return true;
        }
      }
      for (const auto& entry : m_server.registry().all()) {
        if (entry->mapped() && entry->workspace() == workspace) {
          focusView(entry.get());
          return true;
        }
      }
      return false;
    };

    if (preferred != nullptr) {
      if (focusMappedOn(preferred)) {
        return;
      }
      // Empty workspace on this output: clear focus instead of highlighting another display.
      clearKeyboardFocus();
      return;
    }

    Output* underCursor = m_server.outputFromWlr(m_server.preferredOutput());
    if (focusMappedOn(underCursor)) {
      return;
    }
    // Stay on the pointer's output: never steal focus onto another display when
    // this one has no mapped window (closing the last window on DP-1, empty WS, …).
    clearKeyboardFocus();
  }

  void FocusManager::deactivateViews(View* except) {
    for (const auto& entry : m_server.registry().all()) {
      if (entry.get() == except || !entry->mapped()) {
        continue;
      }
      if (entry->toplevel()->scheduled.activated) {
        wlr_xdg_toplevel_set_activated(entry->toplevel(), false);
      }
      entry->setBorderFocused(false);
      entry->setForeignActivated(false);
    }
  }

  void FocusManager::clearKeyboardFocus() {
    deactivateViews(nullptr);
    wlr_seat* seat = m_server.seat()->wlr();
    // Overview and other compositor-owned states must not leave an xdg popup
    // grab intercepting both this clear and the later focus restoration. Keep
    // an active data-device drag intact because its completion performs an
    // explicit focus replay.
    if (seat->drag == nullptr && wlr_seat_keyboard_has_grab(seat)) {
      wlr_seat_keyboard_end_grab(seat);
    }
    m_server.notifyKeyboardClearFocus();
    m_server.refreshOutputPolicies();
  }

  void FocusManager::clearNormalFocus() {
    // A lock takes the whole seat, so the pointer goes too. Everything else is
    // the same teardown as clearKeyboardFocus.
    m_server.cursor()->clearPointerFocusOverridingGrab();
    clearKeyboardFocus();
  }

  View*
  FocusManager::viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer) {
    if (layer != nullptr) {
      *layer = nullptr;
    }

    wlr_scene_node* node = wlr_scene_node_at(&m_server.scene()->tree.node, lx, ly, sx, sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
      return nullptr;
    }

    wlr_scene_buffer* sceneBuffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(sceneBuffer);
    if (sceneSurface == nullptr) {
      return nullptr;
    }

    *surface = sceneSurface->surface;
    wlr_scene_tree* tree = node->parent;
    SceneNode* sceneNode = nullptr;
    while (tree != nullptr && (sceneNode = sceneNodeFrom(tree->node.data)) == nullptr) {
      tree = tree->node.parent;
    }
    if (sceneNode == nullptr) {
      return nullptr;
    }

    if (sceneNode->kind == SceneNodeKind::LockSurface) {
      return nullptr;
    }
    if (m_server.sessionLocked()) {
      *surface = nullptr;
      return nullptr;
    }
    if (sceneNode->kind == SceneNodeKind::LayerSurface) {
      if (layer != nullptr) {
        *layer = static_cast<LayerSurface*>(sceneNode);
      }
      return nullptr;
    }
    auto* view = static_cast<View*>(sceneNode);
    // Workspace transitions and scratchpad fade-outs keep an inactive view's scene enabled until the animation
    // finishes. They are visual snapshots, not interactive windows.
    if (!view->pinned() && !view->onActiveWorkspace()) {
      *surface = nullptr;
      return nullptr;
    }
    return view;
  }

} // namespace umbriel
