#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "input/cursor.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("view");
  } // namespace

  void View::setForeignActivated(bool activated) {
    if (m_activated == activated) {
      return;
    }
    m_activated = activated;
    if (m_foreign != nullptr) {
      wlr_foreign_toplevel_handle_v1_set_activated(m_foreign, activated);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::updateForeignIdentity() {
    if (m_foreign != nullptr) {
      wlr_foreign_toplevel_handle_v1_set_title(m_foreign, title() != nullptr ? title() : "");
      wlr_foreign_toplevel_handle_v1_set_app_id(m_foreign, appId() != nullptr ? appId() : "");
    }
    if (m_extForeign != nullptr) {
      const wlr_ext_foreign_toplevel_handle_v1_state state = {
          .title = title(),
          .app_id = appId(),
      };
      wlr_ext_foreign_toplevel_handle_v1_update_state(m_extForeign, &state);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::updateForeignState() {
    if (m_foreign == nullptr) {
      return;
    }
    wlr_foreign_toplevel_handle_v1_set_maximized(m_foreign, currentMaximized());
    wlr_foreign_toplevel_handle_v1_set_fullscreen(m_foreign, currentFullscreen());
  }

  void View::enterForeignOutput() {
    Output* output = nullptr;
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      output = m_workspace->group()->output();
    } else {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    enterForeignOutput(output);
  }

  void View::enterForeignOutput(Output* output) {
    wlr_output* wlrOutput = output != nullptr ? output->wlr() : nullptr;
    if (m_foreign == nullptr || wlrOutput == m_foreignOutput) {
      return;
    }
    leaveForeignOutput();
    if (wlrOutput != nullptr) {
      wlr_foreign_toplevel_handle_v1_output_enter(m_foreign, wlrOutput);
      m_foreignOutput = wlrOutput;
    }
  }

  void View::leaveForeignOutput() {
    if (m_foreign == nullptr || m_foreignOutput == nullptr) {
      return;
    }
    wlr_foreign_toplevel_handle_v1_output_leave(m_foreign, m_foreignOutput);
    m_foreignOutput = nullptr;
  }

  void View::onForeignActivate(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_foreignActivate);
    self->handleForeignActivate();
  }

  void View::onForeignClose(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_foreignClose);
    self->handleForeignClose();
  }

  void View::onForeignMinimize(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_foreignMinimize);
    self->handleForeignMinimize(data);
  }

  void View::onForeignDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_foreignDestroy);
    self->handleForeignDestroy();
  }

  void View::handleSetTitle() {
    syncSsdFrame();
    updateForeignIdentity();
    if (m_workspace != nullptr) {
      m_workspace->tabs().memberChanged(this);
    }
    // A title the client set settles the opening rules even when it is empty: an empty title is matchable, an absent
    // one is not. applyWindowRules refreshes dynamic effects itself.
    if (!m_initialTitleRulesSettled && title() != nullptr) {
      m_initialTitleRulesSettled = true;
      m_initialRulesAppId = ruleText(appId());
      m_initialRulesTitle = ruleText(title());
      applyWindowRules();
    } else {
      applyDynamicRules();
    }
    // A rule may opt this dialog, or the dialogs of this window, out of attaching by title.
    if (m_mapped) {
      reconcileAttachment();
      reconcileDialogAttachments();
    }
  }

  void View::handleSetAppId() {
    kLog.debug("app_id='{}'", appId() != nullptr ? appId() : "");
    updateForeignIdentity();
    // A tab without a title is labelled with its app id.
    if (m_workspace != nullptr) {
      m_workspace->tabs().memberChanged(this);
    }
    if (!m_initialTitleRulesSettled) {
      // Title hasn't arrived yet. If no rule cares about title, we can settle now.
      // Otherwise only update non-disruptive effects; disruptive rules wait for the title.
      if (!anyWindowRuleHasTitlePattern(config())) {
        m_initialTitleRulesSettled = true;
        m_initialRulesAppId = ruleText(appId());
        m_initialRulesTitle = ruleText(title());
        applyWindowRules();
      } else {
        applyDynamicRules();
      }
    } else {
      applyDynamicRules();
    }
    if (m_mapped) {
      reconcileAttachment();
      reconcileDialogAttachments();
      if (Output* output = currentOutput()) {
        output->updateHdr();
      }
    }
  }

  void View::handleForeignActivate() {
    if (!m_mapped) {
      return;
    }
    Workspace* workspace = m_workspace;
    kLog.debug(
        "foreign-toplevel activate app_id='{}' mapped={} visible={} workspace='{}' other_workspace={}",
        appId() != nullptr ? appId() : "", m_mapped, m_onActiveWorkspace, workspace != nullptr ? workspace->name() : "",
        workspace != nullptr && !workspace->active()
    );
    m_server->focusView(this, FocusReason::ForeignActivation);
    Overview* overview = m_server->overview();
    if (config().input.cursor.followsFocus && (overview == nullptr || !overview->active())) {
      m_server->cursor()->warpToView(*this);
    }
  }

  void View::handleForeignClose() { requestClose(); }

  void View::handleForeignMinimize(void* data) {
    if (!m_mapped) {
      return;
    }
    // Unminimizing shows the window where it was without focusing it; a taskbar activates separately.
    if (static_cast<wlr_foreign_toplevel_handle_v1_minimized_event*>(data)->minimized) {
      requestMinimize();
    } else {
      setMinimized(false);
    }
  }

  void View::handleForeignDestroy() {
    wl_list_remove(&m_foreignActivate.link);
    wl_list_remove(&m_foreignClose.link);
    wl_list_remove(&m_foreignMinimize.link);
    wl_list_remove(&m_foreignDestroy.link);
    m_foreignActivate.link.next = nullptr;
    m_foreignClose.link.next = nullptr;
    m_foreignMinimize.link.next = nullptr;
    m_foreignDestroy.link.next = nullptr;
    m_foreign = nullptr;
    m_foreignOutput = nullptr;
  }

  void View::onExtForeignDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_extForeignDestroy);
    self->handleExtForeignDestroy();
  }

  void View::handleExtForeignDestroy() {
    wl_list_remove(&m_extForeignDestroy.link);
    m_extForeignDestroy.link.next = nullptr;
    m_extForeign = nullptr;
  }

  void View::onCaptureSourceDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_captureSourceDestroy);
    self->handleCaptureSourceDestroy();
  }

  void View::handleCaptureSourceDestroy() {
    wl_list_remove(&m_captureSourceDestroy.link);
    m_captureSourceDestroy.link.next = nullptr;
    m_captureSource = nullptr;
    if (m_mapped) {
      refreshStateRuleEffects();
    }
  }

  void View::refreshCaptureProtection() {
    const auto parent = [](View& view) -> View* {
      wlr_surface* surface = nullptr;
      if (view.m_toplevel != nullptr && view.m_toplevel->parent != nullptr) {
        surface = view.m_toplevel->parent->base->surface;
      } else if (view.m_xsurface != nullptr && view.m_xsurface->parent != nullptr) {
        surface = view.m_xsurface->parent->surface;
      }
      return surface != nullptr ? View::fromSurface(surface) : nullptr;
    };
    const auto effective = [this, &parent](View& view) {
      bool excluded = false;
      size_t remaining = m_server->views().size();
      View* owner = &view;
      for (; owner != nullptr && remaining > 0; owner = parent(*owner), --remaining) {
        excluded |= owner->m_ruleCaptureBlocked;
      }
      return excluded || owner != nullptr;
    };
    if (m_captureBlocked == effective(*this)) {
      return;
    }
    // Resolve complete parent chains independently of reload/registry order.
    for (const auto& entry : m_server->views()) {
      if (entry == nullptr) {
        continue;
      }
      View& view = *entry;
      const bool excluded = effective(view);
      if (excluded == view.m_captureBlocked) {
        continue;
      }
      view.m_captureBlocked = excluded;
      // Allocation failure makes scene captures fail closed, without hiding the local view.
      (void)umbrielfx_scene_node_set_capture_excluded(&view.m_sceneTree->node, excluded);
      if (excluded && view.m_captureSource != nullptr) {
        // Copy submission stops immediately; destruction is deferred past the
        // new-session/rule-evaluation stack that may have enabled protection.
        umbrielfx_scene_capture_source_revoke(view.m_captureSource);
      }
    }
    if (m_server->xwayland() != nullptr) {
      m_server->xwayland()->refreshCaptureProtection();
    }
    for (const auto& output : m_server->outputs()) {
      output->scheduleFullFrame();
    }
    m_server->scheduleSuspensionSync();
  }

} // namespace umbriel
