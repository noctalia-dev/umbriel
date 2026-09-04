#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

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
      wlr_foreign_toplevel_handle_v1_set_title(m_foreign, m_toplevel->title != nullptr ? m_toplevel->title : "");
      wlr_foreign_toplevel_handle_v1_set_app_id(m_foreign, m_toplevel->app_id != nullptr ? m_toplevel->app_id : "");
    }
    if (m_extForeign != nullptr) {
      const wlr_ext_foreign_toplevel_handle_v1_state state = {
          .title = m_toplevel->title,
          .app_id = m_toplevel->app_id,
      };
      wlr_ext_foreign_toplevel_handle_v1_update_state(m_extForeign, &state);
    }
    m_server->scheduleIpcWindowsEvent();
  }

  void View::updateForeignState() {
    if (m_foreign == nullptr) {
      return;
    }
    wlr_foreign_toplevel_handle_v1_set_maximized(m_foreign, m_toplevel->current.maximized);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(m_foreign, m_toplevel->current.fullscreen);
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

  void View::onForeignDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_foreignDestroy);
    self->handleForeignDestroy();
  }

  void View::handleSetTitle() {
    updateForeignIdentity();
    // A title the client set settles the opening rules even when it is empty: an empty title is matchable, an absent
    // one is not. applyWindowRules refreshes dynamic effects itself.
    if (!m_initialRulesSettled && m_toplevel->title != nullptr) {
      m_initialRulesSettled = true;
      applyWindowRules(m_initialRules);
      return;
    }
    applyDynamicRules();
  }

  void View::handleSetAppId() {
    kLog.debug("app_id='{}'", m_toplevel->app_id != nullptr ? m_toplevel->app_id : "");
    updateForeignIdentity();
    if (!m_initialRulesSettled) {
      // Title hasn't arrived yet. If no rule cares about title, we can settle now.
      // Otherwise only update non-disruptive effects; disruptive rules wait for the title.
      if (!anyWindowRuleHasTitlePattern(config())) {
        m_initialRulesSettled = true;
        applyWindowRules(m_initialRules);
      } else {
        applyDynamicRules();
      }
    } else {
      applyDynamicRules();
    }
    if (m_mapped) {
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
        m_toplevel->app_id != nullptr ? m_toplevel->app_id : "", m_mapped, m_onActiveWorkspace,
        workspace != nullptr ? workspace->name() : "", workspace != nullptr && !workspace->active()
    );
    m_server->focusView(this, FocusReason::ForeignActivation);
  }

  void View::handleForeignClose() { wlr_xdg_toplevel_send_close(m_toplevel); }

  void View::handleForeignDestroy() {
    wl_list_remove(&m_foreignActivate.link);
    wl_list_remove(&m_foreignClose.link);
    wl_list_remove(&m_foreignDestroy.link);
    m_foreignActivate.link.next = nullptr;
    m_foreignClose.link.next = nullptr;
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
  }
} // namespace umbriel
