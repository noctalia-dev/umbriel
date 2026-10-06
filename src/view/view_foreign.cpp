#include "config/config.h"
#include "config/resolve.h"
#include "core/log.h"
#include "input/cursor.h"
#include "output/frame_schedule.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <chrono>

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

  void View::onForeignDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_foreignDestroy);
    self->handleForeignDestroy();
  }

  void View::handleSetTitle() {
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
      return;
    }
    applyDynamicRules();
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
    detachCaptureAudio();
    wl_list_remove(&m_captureSourceDestroy.link);
    m_captureSourceDestroy.link.next = nullptr;
    m_captureSource = nullptr;
  }

  void View::attachCaptureAudio() {
    if (m_captureScene == nullptr || wl_list_empty(&m_captureScene->outputs)) {
      return;
    }
    auto* sceneOutput = wl_container_of(m_captureScene->outputs.next, static_cast<wlr_scene_output*>(nullptr), link);
    m_captureOutput = sceneOutput->output;
    m_captureAudioFrame.notify = onCaptureAudioFrame;
    // Latch and bind before the capture source's own frame listener builds its
    // scene. The trailing listener sees whether that build actually committed.
    wl_list_insert(&m_captureOutput->events.frame.listener_list, &m_captureAudioFrame.link);
    m_captureAudioAfterFrame.notify = onCaptureAudioAfterFrame;
    wl_signal_add(&m_captureOutput->events.frame, &m_captureAudioAfterFrame);
    m_captureAudioCommit.notify = onCaptureAudioCommit;
    wl_signal_add(&m_captureOutput->events.commit, &m_captureAudioCommit);
    effectRegistry().registerAudioCapture(m_captureSource, [this] { scheduleCaptureAudio(); });
  }

  void View::detachCaptureAudio() {
    if (m_captureAudioTimer != nullptr) {
      wl_event_source_remove(m_captureAudioTimer);
      m_captureAudioTimer = nullptr;
    }
    if (m_captureOutput != nullptr) {
      wl_list_remove(&m_captureAudioFrame.link);
      wl_list_remove(&m_captureAudioAfterFrame.link);
      wl_list_remove(&m_captureAudioCommit.link);
      m_captureOutput = nullptr;
    }
    effectRegistry().removeAudioCapture(m_captureSource);
    m_captureSessions = 0;
  }

  void View::changeCaptureSessions(int delta) {
    if (delta > 0) {
      ++m_captureSessions;
    } else if (m_captureSessions > 0) {
      --m_captureSessions;
    }
    syncAnimationEffects();
    if (m_captureSessions == 0 && m_captureAudioTimer != nullptr) {
      wl_event_source_remove(m_captureAudioTimer);
      m_captureAudioTimer = nullptr;
    }
    scheduleCaptureAudio();
  }

  void View::scheduleCaptureAudio() {
    if (m_captureOutput == nullptr || m_captureSessions == 0 || !effectRegistry().audioDirty(m_captureSource)) {
      return;
    }
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count()
    );
    const auto delay = effectFrameDelayMs(config().effects.maxFps, now, m_captureAudioLastMsec);
    if (delay <= 1) {
      wlr_output_schedule_frame(m_captureOutput);
      return;
    }
    if (m_captureAudioTimer == nullptr) {
      m_captureAudioTimer =
          wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onCaptureAudioTimer, this);
    }
    if (m_captureAudioTimer != nullptr) {
      wl_event_source_timer_update(m_captureAudioTimer, static_cast<int>(delay));
    }
  }

  int View::onCaptureAudioTimer(void* data) {
    auto& self = *static_cast<View*>(data);
    wl_event_source_remove(self.m_captureAudioTimer);
    self.m_captureAudioTimer = nullptr;
    self.scheduleCaptureAudio();
    return 0;
  }

  void View::onCaptureAudioFrame(wl_listener* listener, void*) {
    View* self = wl_container_of(listener, self, m_captureAudioFrame);
    self->m_captureAudioSubmitted = false;
    if (self->m_captureSessions == 0) {
      return;
    }
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count()
    );
    const bool advance = effectFrameDelayMs(config().effects.maxFps, now, self->m_captureAudioLastMsec) <= 1;
    effectRegistry().beginAudioCapture(self->m_captureSource, advance);
    if (advance) {
      self->m_captureAudioLastMsec = now;
    }
    self->syncAnimationEffects();
  }

  void View::onCaptureAudioAfterFrame(wl_listener* listener, void*) {
    View* self = wl_container_of(listener, self, m_captureAudioAfterFrame);
    effectRegistry().finishAudioCapture(self->m_captureSource, self->m_captureAudioSubmitted);
    self->scheduleCaptureAudio();
  }

  void View::onCaptureAudioCommit(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_captureAudioCommit);
    const auto* event = static_cast<wlr_output_event_commit*>(data);
    if ((event->state->committed & WLR_OUTPUT_STATE_BUFFER) != 0) {
      self->m_captureAudioSubmitted = true;
    }
  }
} // namespace umbriel
