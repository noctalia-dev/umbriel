#include "config/config.h"
#include "input/seat.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <array>
#include <vector>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {

  View* View::placementRoot() {
    View* root = this;
    while (root->m_attachedTo != nullptr) {
      root = root->m_attachedTo;
    }
    return root;
  }

  const View* View::placementRoot() const {
    const View* root = this;
    while (root->m_attachedTo != nullptr) {
      root = root->m_attachedTo;
    }
    return root;
  }

  View* View::inputDelegate() {
    View* delegate = this;
    while (!delegate->m_attachments.empty()) {
      delegate = delegate->m_attachments.back();
    }
    return delegate;
  }

  void View::setDialog(wlr_xdg_dialog_v1* dialog) {
    releaseDialog();
    m_dialog = dialog;
    m_dialogSetModal.notify = onDialogSetModal;
    wl_signal_add(&dialog->events.set_modal, &m_dialogSetModal);
    m_dialogDestroy.notify = onDialogDestroy;
    wl_signal_add(&dialog->events.destroy, &m_dialogDestroy);
    reconcileAttachment();
  }

  void View::releaseDialog() {
    if (m_dialog == nullptr) {
      return;
    }
    wl_list_remove(&m_dialogSetModal.link);
    wl_list_remove(&m_dialogDestroy.link);
    m_dialog = nullptr;
  }

  void View::onDialogSetModal(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_dialogSetModal);
    self->reconcileAttachment();
  }

  void View::onDialogDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_dialogDestroy);
    // Destroying the object takes its modality back.
    self->releaseDialog();
    self->reconcileAttachment();
  }

  bool View::wantsAttach() {
    if (!m_mapped || m_toplevel == nullptr || !config().general.attachModalDialogs) {
      return false;
    }
    View* parent = shellParent();
    if (parent == nullptr || parent->m_toplevel == nullptr) {
      return false;
    }
    for (const View* ancestor = parent; ancestor != nullptr; ancestor = ancestor->m_attachedTo) {
      if (ancestor == this) {
        return false;
      }
    }
    // The dialog's own rule decides first. Otherwise its parent's does, so a rule written for an application also
    // covers the portal dialogs it opens, which carry the portal's app id instead of the application's.
    std::optional<bool> rule = resolvedRules().attachToParent;
    if (!rule) {
      rule = parent->resolvedRules().attachToParent;
    }
    if (!rule.value_or(true)) {
      return false;
    }
    if (m_dialog != nullptr && m_dialog->modal) {
      return true;
    }
    return wl_resource_get_client(m_toplevel->resource) != wl_resource_get_client(parent->m_toplevel->resource);
  }

  void View::reconcileAttachment() {
    View* parent = wantsAttach() ? shellParent() : nullptr;
    if (parent == m_attachedTo) {
      return;
    }
    if (parent != nullptr) {
      attach(parent);
    } else {
      detach();
    }
  }

  void View::reconcileDialogAttachments() {
    // Only a rule can make a parent's app id or title matter to its dialogs; without one, skip the registry scan.
    const auto decides = [](const WindowRule& rule) { return rule.attachToParent.has_value(); };
    if (std::ranges::none_of(config().windowRules, decides)) {
      return;
    }
    for (const auto& entry : m_server->registry().all()) {
      if (View* child = entry.get(); child != this && child->mapped() && child->shellParent() == this) {
        child->reconcileAttachment();
      }
    }
  }

  void View::attach(View* parent) {
    const View* focused = fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface);
    const bool takesInput = focused != nullptr
        && (focused->placementRoot() == parent->placementRoot() || focused->placementRoot() == placementRoot());
    if (m_attachedTo != nullptr) {
      unlinkAttachment();
    } else {
      if (scheduledFullscreen() || currentFullscreen()) {
        setFullscreen(false);
      }
      m_pinned = false;
      m_restoreTiledAfterUnpin = false;
      m_restorePinnedAfterFullscreen = false;
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
        scratchpad->remove(this);
      }
      if (m_workspace != nullptr) {
        setWorkspace(nullptr, false);
      }
      m_displacedHome.reset();
      if (m_tiled) {
        m_tiled = false;
        setTiledState(
            config().appearance.preferNoCsd ? WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT : 0
        );
      }
      if (scheduledMaximized()) {
        setMaximizedState(false);
      }
      m_floatingMaximized = false;
      m_maximizedToEdges = false;
      m_restoreMaximizedToEdges = false;
      m_aloneOpeningSeed = AloneSeed::None;
      cancelPositionAnimation();
    }

    m_attachedTo = parent;
    parent->m_attachments.push_back(this);
    // The raw reparent below supersedes any deferred resting re-home.
    m_pendingHomeParent = nullptr;
    m_releaseRehomeArmed = false;
    wlr_scene_node_reparent(&m_sceneTree->node, parent->m_attachmentTree);
    syncShadowPool();
    setOnActiveWorkspace(true);
    setNodeEnabled(true);
    parent->layoutAttachments();
    parent->retargetModalShade();
    notifyOutputScale();
    enterForeignOutput();
    m_server->scheduleIpcWindowsEvent();
    syncMinimizeEligibility();
    // The application waits on a modal dialog, so the window it attaches to shows.
    if (View* root = placementRoot(); root->minimized()) {
      root->setMinimized(false);
    }
    if (Output* output = currentOutput()) {
      output->updateVrr();
      output->updateHdr();
    }
    if (Overview* overview = m_server->overview()) {
      overview->onViewAttachmentsChanged(parent);
    }
    if (takesInput) {
      m_server->focusView(placementRoot(), FocusReason::Directional);
    }
  }

  View* View::unlinkAttachment() {
    View* parent = m_attachedTo;
    const int x = layoutX();
    const int y = layoutY();
    std::erase(parent->m_attachments, this);
    m_attachedTo = nullptr;
    wlr_scene_node_reparent(&m_sceneTree->node, m_server->xdgTree());
    setPosition(x, y);
    parent->layoutAttachments();
    parent->retargetModalShade();
    if (Overview* overview = m_server->overview()) {
      overview->onViewAttachmentsChanged(parent);
    }
    return parent;
  }

  void View::detach() {
    if (m_attachedTo == nullptr) {
      return;
    }
    const bool heldInput = fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface) == this;
    View* root = placementRoot();
    unlinkAttachment();
    ScratchpadManager* scratchpad = m_server->scratchpadManager();
    if (root->m_workspace != nullptr) {
      setWorkspace(root->m_workspace, false);
    } else if (
        scratchpad == nullptr
        || !scratchpad->assignFromParent(
            this, root,
            ScratchpadManager::AutomaticAdmission{
                .restoreOutput = root->currentOutput(),
                .focusOrigin = scratchpad->outputFor(root),
                .restoreTiled = false,
                .updateRestoreLocation = true,
            }
        )
    ) {
      setOnActiveWorkspace(true);
    }
    rememberFloatingPosition();
    syncOwnedPresentation();
    raiseToTop();
    if (heldInput) {
      m_server->focusView(this, FocusReason::Directional);
    }
    syncMinimizeEligibility();
  }

  void View::releaseAttachments() {
    // Attaching or detaching edits the list, so walk a copy.
    const std::vector<View*> attachments = m_attachments;
    for (View* attachment : attachments) {
      if (m_attachedTo != nullptr) {
        attachment->attach(m_attachedTo);
      } else {
        attachment->detach();
      }
    }
  }

  void View::layoutAttachments() {
    if (m_attachments.empty()) {
      return;
    }
    // Kept on screen as Mutter keeps a modal dialog, against the box this window rests in rather than wherever a
    // scroll or move animation has it now: the dialog moves with the frame it hangs in, and a fullscreen window shows
    // over the panels.
    wlr_box area{};
    if (const Output* output = currentOutput(); output != nullptr) {
      area = scheduledFullscreen() ? fullscreenArea() : output->usableArea();
    }
    const int restX = layoutTargetX();
    const int restY = layoutTargetY();
    for (View* attachment : m_attachments) {
      const wlr_box& geometry = attachment->geometryBox();
      const wlr_box sized{.x = 0, .y = 0, .width = geometry.width, .height = geometry.height};
      const int inset = attachment->borderInset();
      const int x =
          attachmentOffset(restX, m_presentedBox.width, attachment->presentedWidth(sized), area.x, area.width, inset);
      const int y = attachmentOffset(
          restY, m_presentedBox.height, attachment->presentedHeight(sized), area.y, area.height, inset
      );
      wlr_scene_node_set_position(&attachment->m_sceneTree->node, x, y);
      attachment->applyPresentation({
          .x = m_presentedBox.x + x,
          .y = m_presentedBox.y + y,
          .width = geometry.width,
          .height = geometry.height,
      });
    }
  }

  void View::retargetModalShade() {
    // The fade is already headed where it belongs, whether it is still running or has landed.
    if ((m_modalShadeAlpha.target() > 0.5) == blocked()) {
      return;
    }
    const double target = blocked() ? 1.0 : 0.0;
    const auto& animation = config().animation;
    if (animation.enabled && animation.dialogs.enabled && m_mapped) {
      m_modalShadeAlpha.retarget(target, animation.dialogs.durationMs, animation.dialogs.curve);
      scheduleFrame();
    } else {
      m_modalShadeAlpha.snap(target);
    }
    syncModalShade();
  }

  void View::syncModalShade() {
    const auto alpha =
        static_cast<float>(std::clamp(m_modalShadeAlpha.current() * config().animation.dialogs.dim, 0.0, 1.0));
    if (m_modalShade == nullptr) {
      if (alpha <= 0.0F) {
        return;
      }
      static constexpr std::array<float, 4> kClear{0.0F, 0.0F, 0.0F, 0.0F};
      m_modalShade = wlr_scene_rect_create(m_sceneTree, 0, 0, kClear.data());
      // Presses go through to the window underneath, which hands them to its dialog.
      m_modalShade->accepts_input = false;
      wlr_scene_node_place_below(&m_modalShade->node, &m_attachmentTree->node);
    }
    const std::array<float, 4> color{0.0F, 0.0F, 0.0F, alpha};
    wlr_scene_rect_set_color(m_modalShade, color.data());
    wlr_scene_rect_set_size(m_modalShade, m_presentedBox.width, m_presentedBox.height);
    wlr_scene_rect_set_corner_radius(m_modalShade, surfaceRadius());
    wlr_scene_node_set_enabled(&m_modalShade->node, alpha > 0.0F);
  }

} // namespace umbriel
