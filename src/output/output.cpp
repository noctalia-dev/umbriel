#include "output/output.h"

#include "config/config.h"
#include "core/log.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "output/frame_schedule.h"
#include "output/hdr_format.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/config_banner.h"
#include "scene/node.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "server/wine_color_manager.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <drm_fourcc.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("output");
    constexpr int kFrameRetryDelayMs = 16;

    const OutputRule* findRule(const char* name) {
      for (const OutputRule& rule : config().outputs) {
        if (rule.name == name) {
          return &rule;
        }
      }
      return nullptr;
    }
  } // namespace

  Output::Output(Server& server, wlr_output* output)
      : m_server(&server), m_output(output), m_defaultScale(output->scale) {
    m_output->data = this;
    wlr_output_init_render(m_output, m_server->allocator(), m_server->renderer());

    m_frame.notify = onFrame;
    wl_signal_add(&m_output->events.frame, &m_frame);

    m_requestState.notify = onRequestState;
    wl_signal_add(&m_output->events.request_state, &m_requestState);

    m_present.notify = onPresent;
    wl_signal_add(&m_output->events.present, &m_present);

    m_destroy.notify = onDestroy;
    wl_signal_add(&m_output->events.destroy, &m_destroy);

    m_frameRetryTimer =
        wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onFrameRetryTimer, this);

    applyCursorConfig();
    (void)applyConfiguredState();
    m_sceneOutput = wlr_scene_output_create(m_server->scene(), m_output);
    wlr_scene_output_set_direct_scanout_enabled(m_sceneOutput, configuredDirectScanoutEnabled());
    updateSceneSdrWhite();
    if (configuredEnabled()) {
      wlr_output_layout_output* layoutOutput = addToLayout();
      wlr_scene_output_layout_add_output(m_server->sceneLayout(), layoutOutput, m_sceneOutput);
    }

    for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      m_layerTrees[layer] = wlr_scene_tree_create(m_server->shellLayerTree(layer));
    }
    m_popupTree = wlr_scene_tree_create(m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY));
    m_viewRoot = wlr_scene_tree_create(m_server->xdgTree());
    m_fullscreenRoot = wlr_scene_tree_create(m_server->fullscreenTree());
    m_pinnedRoot = wlr_scene_tree_create(m_server->pinnedTree());
    m_pinnedShadowRoot = wlr_scene_tree_create(m_server->pinnedShadowTree());
    arrangeLayers();
    m_workspaceGroup = std::make_unique<WorkspaceGroup>(*m_server, *this);
  }

  bool Output::configuredEnabled() const {
    const OutputRule* rule = findRule(m_output->name);
    return rule == nullptr || rule->enabled;
  }

  wlr_box Output::layoutBox() const {
    wlr_box box{.x = m_arrangedLayoutX, .y = m_arrangedLayoutY, .width = 0, .height = 0};
    wlr_output_effective_resolution(m_output, &box.width, &box.height);
    if (const wlr_output_layout_output* layoutOutput = wlr_output_layout_get(m_server->outputLayout(), m_output)) {
      box.x = layoutOutput->x;
      box.y = layoutOutput->y;
    }
    return box;
  }

  wlr_box Output::usableArea() const {
    wlr_box area = m_localUsableArea;
    const wlr_box box = layoutBox();
    area.x += box.x;
    area.y += box.y;
    return area;
  }

  HdrMode Output::hdrMode() const {
    const OutputRule* rule = findRule(m_output->name);
    return rule != nullptr ? rule->hdr : HdrMode::Off;
  }

  bool Output::hdrRequested() const {
    if (wlr_surface* surface = m_server->seat()->wlr()->keyboard_state.focused_surface) {
      if (View* view = View::fromSurface(surface); view != nullptr && view->mapped() && view->currentOutput() == this) {
        if (const std::optional<HdrMode> mode = view->resolvedRules().hdr) {
          const bool fullscreen = view->layoutFullscreen() || view->toplevel()->current.fullscreen;
          return hdrEnabled(*mode, fullscreen, autoHdrEligible(view));
        }
      }
    }
    const HdrMode mode = hdrMode();
    return hdrEnabled(mode, m_fullscreenHdrRequested, m_autoHdrOwner != nullptr);
  }

  bool Output::hdrActive() const { return m_output->image_description != nullptr; }

  float Output::configuredSdrWhite() const {
    const OutputRule* rule = findRule(m_output->name);
    return rule != nullptr ? rule->sdrWhite : 203.0F;
  }

  bool Output::configuredDirectScanoutEnabled() const {
    const OutputRule* rule = findRule(m_output->name);
    return rule == nullptr || rule->directScanout;
  }

  bool Output::configuredTearingAllowed() const {
    const OutputRule* rule = findRule(m_output->name);
    return rule != nullptr && rule->allowTearing;
  }

  View* Output::tearingCandidate() const {
    const Workspace* workspace = m_workspaceGroup != nullptr ? m_workspaceGroup->active() : nullptr;
    if (workspace == nullptr) {
      return nullptr;
    }
    const auto eligible = [this](View* view) {
      return view != nullptr
          && view->mapped()
          && view->onActiveWorkspace()
          && view->currentOutput() == this
          && view->layoutFullscreen()
          && view->toplevel()->current.fullscreen;
    };
    if (View* focused = workspace->focusedView(); eligible(focused)) {
      return focused;
    }
    const auto& views = workspace->allViews();
    const auto candidate = std::ranges::find_if(views, eligible);
    return candidate != views.end() ? *candidate : nullptr;
  }

  bool Output::clientTearingHintAsync(const View* view) const {
    if (view == nullptr || m_server->tearingControlManager() == nullptr) {
      return false;
    }
    return wlr_tearing_control_manager_v1_surface_hint_from_surface(
               m_server->tearingControlManager(), view->toplevel()->base->surface
           )
        == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
  }

  bool Output::tearingEligible(View* view) const {
    if (view == nullptr || tearingCandidate() != view) {
      return false;
    }
    return tearingEnabled(configuredTearingAllowed(), view->resolvedRules().allowTearing, clientTearingHintAsync(view));
  }

  bool Output::tearingRequested() const { return tearingEligible(tearingCandidate()); }

  std::optional<bool> Output::lastPresentationVsync() const {
    if (!m_lastPresentationFlags || *m_lastPresentationFlags == 0) {
      return std::nullopt;
    }
    return (*m_lastPresentationFlags & WLR_OUTPUT_PRESENT_VSYNC) != 0;
  }

  void Output::resetTearingState() {
    m_tearingFallbackReason.clear();
    m_tearingRecovery.reset();
    wlr_output_schedule_frame(m_output);
  }

  void Output::applyDirectScanoutConfig() {
    wlr_scene_output_set_direct_scanout_enabled(m_sceneOutput, configuredDirectScanoutEnabled());
  }

  void Output::setHdrFallbackReason(std::string_view reason) {
    if (m_hdrFallbackReason == reason) {
      return;
    }
    m_hdrFallbackReason = reason;
    if (!reason.empty()) {
      kLog.warn("output '{}': HDR unavailable: {}", m_output->name, reason);
    }
  }

  void Output::updateSceneSdrWhite() {
    if (m_sceneOutput == nullptr) {
      return;
    }
    const float sdrWhite = hdrActive() ? configuredSdrWhite() : 0.0F;
    wlr_scene_output_set_sdr_white_level(m_sceneOutput, sdrWhite);
  }

  void Output::rejectGammaControl(wlr_gamma_control_v1* control) {
    if (control != nullptr) {
      wlr_gamma_control_v1_send_failed_and_destroy(control);
      if (!m_hdrGammaWarningLogged) {
        kLog.warn("output '{}': gamma control is unavailable while HDR is active", m_output->name);
        m_hdrGammaWarningLogged = true;
      }
    }
    m_gammaDirty = false;
  }

  bool Output::applyConfiguredState() {
    const OutputRule* rule = findRule(m_output->name);
    const std::optional<double> configuredScale = rule != nullptr ? rule->scale : std::nullopt;
    const bool configured = configuredEnabled();
    const bool enabled = configured && !m_dpmsOff;
    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enabled);

    const bool hdrWasActive = hdrActive();
    const bool hdrRequested = this->hdrRequested();
    m_lastHdrRequested = hdrRequested;
    bool hdrAttempted = false;

    bool vrrRequested = false;
    bool vrrStaged = false;
    bool scaleStaged = false;
    if (enabled) {
      if (rule != nullptr && rule->mode) {
        if (wlr_output_is_wl(m_output)) {
          kLog.info("output '{}': mode is ignored in nested sessions", m_output->name);
        } else {
          const OutputMode& configured = *rule->mode;
          wlr_output_mode* selected = nullptr;
          wlr_output_mode* mode = nullptr;
          wl_list_for_each(mode, &m_output->modes, link) {
            if (mode->width != configured.width || mode->height != configured.height) {
              continue;
            }
            if (configured.refreshMHz != 0) {
              if (selected == nullptr
                  || std::abs(mode->refresh - configured.refreshMHz)
                      < std::abs(selected->refresh - configured.refreshMHz)) {
                selected = mode;
              }
            } else if (
                selected == nullptr
                || (mode->preferred && !selected->preferred)
                || (mode->preferred == selected->preferred && mode->refresh > selected->refresh)
            ) {
              selected = mode;
            }
          }
          if (selected != nullptr) {
            wlr_output_state_set_mode(&state, selected);
          } else {
            wlr_output_state_set_custom_mode(&state, configured.width, configured.height, configured.refreshMHz);
          }
        }
      } else if (!wlr_output_is_wl(m_output)) {
        if (wlr_output_mode* mode = wlr_output_preferred_mode(m_output)) {
          wlr_output_state_set_mode(&state, mode);
        }
      }

      if (configuredScale) {
        wlr_output_state_set_scale(&state, static_cast<float>(*configuredScale));
        scaleStaged = true;
      } else if (m_appliedConfiguredScale) {
        wlr_output_state_set_scale(&state, m_defaultScale);
        scaleStaged = true;
      }
      if (rule != nullptr && rule->transform) {
        wlr_output_state_set_transform(&state, static_cast<wl_output_transform>(*rule->transform));
      }
      vrrRequested = configuredVrrEnabled();
      if (m_output->adaptive_sync_supported) {
        wlr_output_state_set_adaptive_sync_enabled(&state, vrrRequested);
        vrrStaged = vrrRequested;
      } else if (vrrRequested) {
        kLog.warn("output '{}': VRR requested but adaptive sync is not supported", m_output->name);
      }

      std::string_view hdrFallback;
      if (hdrRequested) {
        if ((m_output->supported_transfer_functions & WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) == 0) {
          hdrFallback = "display does not advertise PQ";
        } else if ((m_output->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020) == 0) {
          hdrFallback = "display does not advertise BT.2020 primaries";
        } else if (!m_server->renderer()->features.output_color_transform) {
          hdrFallback = "renderer lacks FP16 output transform";
        } else {
          const wlr_output_image_description description = {
              .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
              .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
              .mastering_display_primaries = {},
              .mastering_luminance = {},
              .max_cll = 0,
              .max_fall = 0,
          };
          if (!wlr_output_state_set_image_description(&state, &description)) {
            hdrFallback = "failed to stage HDR image description";
          } else {
            const wlr_drm_format_set* primaryFormats =
                wlr_output_get_primary_formats(m_output, m_server->allocator()->buffer_caps);
            const auto selectFormat = [&]() {
              return selectHdrRenderFormat(m_output->render_format, [&](uint32_t format) {
                if (primaryFormats != nullptr && wlr_drm_format_set_get(primaryFormats, format) == nullptr) {
                  return false;
                }
                wlr_output_state_set_render_format(&state, format);
                return wlr_output_test_state(m_output, &state);
              });
            };

            std::optional<uint32_t> renderFormat = selectFormat();
            if (!renderFormat && vrrStaged) {
              wlr_output_state_set_adaptive_sync_enabled(&state, false);
              vrrStaged = false;
              renderFormat = selectFormat();
              if (renderFormat) {
                kLog.warn("output '{}': HDR is incompatible with VRR, keeping VRR disabled", m_output->name);
              } else {
                wlr_output_state_set_adaptive_sync_enabled(&state, vrrRequested);
                vrrStaged = vrrRequested;
              }
            }
            if (renderFormat) {
              hdrAttempted = true;
              kLog.info(
                  "output '{}': selected HDR render format {}", m_output->name,
                  *renderFormat == DRM_FORMAT_XRGB2101010 ? "XR30" : "XB30"
              );
            } else {
              hdrFallback = "backend rejected all 10-bit HDR render formats";
            }
          }
        }
      }
      if (!hdrFallback.empty()) {
        setHdrFallbackReason(hdrFallback);
      }
    }

    if ((!hdrRequested && hdrWasActive) || (enabled && hdrRequested && !hdrAttempted)) {
      wlr_output_state_set_image_description(&state, nullptr);
      wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
    }

    const auto commitConfiguredState = [&]() {
      bool success = wlr_output_commit_state(m_output, &state);
      if (!success && vrrStaged) {
        kLog.warn("output '{}': configured state commit failed, retrying with VRR disabled", m_output->name);
        wlr_output_state_set_adaptive_sync_enabled(&state, false);
        vrrStaged = false;
        success = wlr_output_commit_state(m_output, &state);
      }
      return success;
    };

    bool committed = commitConfiguredState();
    if (!committed && hdrAttempted) {
      setHdrFallbackReason("HDR commit rejected by backend");
      wlr_output_state_set_image_description(&state, nullptr);
      wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
      if (m_output->adaptive_sync_supported) {
        wlr_output_state_set_adaptive_sync_enabled(&state, vrrRequested);
        vrrStaged = vrrRequested;
      }
      committed = commitConfiguredState();
    }
    wlr_output_state_finish(&state);
    if (!committed) {
      kLog.error("output '{}': failed to commit configured state", m_output->name);
      return false;
    }
    if (enabled && scaleStaged) {
      m_appliedConfiguredScale = configuredScale.has_value();
    }
    const bool hdrIsActive = hdrActive();
    if (hdrIsActive) {
      setHdrFallbackReason({});
      rejectGammaControl(wlr_gamma_control_manager_v1_get_control(m_server->gammaManager(), m_output));
    } else {
      if (!hdrRequested) {
        setHdrFallbackReason({});
      }
      if (hdrWasActive) {
        m_gammaDirty = true;
      }
      m_hdrGammaWarningLogged = false;
    }
    updateSceneSdrWhite();
    m_server->updateIdleInhibit();
    if (enabled) {
      kLog.info(
          "output '{}': applied mode={}x{}@{}mHz scale={} transform={}", m_output->name, m_output->width,
          m_output->height, m_output->refresh, m_output->scale, static_cast<int>(m_output->transform)
      );
    } else if (!configured) {
      kLog.info("output '{}': disabled by config", m_output->name);
    } else {
      kLog.info("output '{}': powered off", m_output->name);
    }
    m_server->updateColorPreferences();
    return true;
  }

  bool Output::configuredVrrEnabled() const {
    const OutputRule* rule = findRule(m_output->name);
    const VrrMode outputMode = rule != nullptr ? rule->vrr : VrrMode::Disabled;
    const bool fullscreen = hasFullscreenView();

    std::optional<VrrMode> focusedMode;
    bool focusedFullscreen = false;
    if (wlr_surface* surface = m_server->seat()->wlr()->keyboard_state.focused_surface) {
      if (View* view = View::fromSurface(surface); view != nullptr && view->mapped() && view->currentOutput() == this) {
        focusedMode = view->resolvedRules().vrr;
        focusedFullscreen = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
      }
    }
    return effectiveVrrEnabled(outputMode, fullscreen, focusedMode, focusedFullscreen);
  }

  bool Output::hasFullscreenView(const View* ignored) const {
    const Workspace* workspace = m_workspaceGroup != nullptr ? m_workspaceGroup->active() : nullptr;
    return workspace != nullptr && std::ranges::any_of(workspace->allViews(), [ignored](const View* view) {
             return view != ignored
                 && view->mapped()
                 && (view->layoutFullscreen() || view->toplevel()->current.fullscreen);
           });
  }

  bool Output::autoHdrEligible(const View* view) const {
    if (view == nullptr
        || !view->mapped()
        || !view->onActiveWorkspace()
        || view->currentOutput() != this
        || (!view->layoutFullscreen() && !view->toplevel()->current.fullscreen)) {
      return false;
    }
    return m_server->surfaceTreeHdrDescription(view->toplevel()->base->surface) != nullptr;
  }

  View* Output::findAutoHdrCandidate() const {
    const auto candidate =
        std::ranges::find_if(m_server->views(), [this](const auto& view) { return autoHdrEligible(view.get()); });
    return candidate != m_server->views().end() ? candidate->get() : nullptr;
  }

  void Output::updateHdr() {
    const HdrMode mode = hdrMode();
    if (mode == HdrMode::Fullscreen) {
      m_autoHdrOwner = nullptr;
      m_fullscreenHdrRequested = hasFullscreenView();
    } else if (mode == HdrMode::Auto) {
      m_fullscreenHdrRequested = false;
      if (!autoHdrEligible(m_autoHdrOwner)) {
        m_autoHdrOwner = findAutoHdrCandidate();
      }
    } else {
      m_fullscreenHdrRequested = false;
      m_autoHdrOwner = nullptr;
    }

    if (hdrRequested() != m_lastHdrRequested && applyConfiguredState()) {
      m_server->updateOutputManagerConfig();
    }
    m_server->updateColorPreferences();
  }

  void Output::forgetHdrView(const View* view) {
    if (hdrMode() == HdrMode::Fullscreen && !hasFullscreenView(view)) {
      m_fullscreenHdrRequested = false;
    }
    if (m_autoHdrOwner == view) {
      m_autoHdrOwner = nullptr;
    }
    if (hdrRequested() != m_lastHdrRequested && applyConfiguredState()) {
      m_server->updateOutputManagerConfig();
    }
    m_server->updateColorPreferences();
  }

  void Output::updateVrr() {
    // wlroots rejects adaptive-sync commits on disabled outputs; nothing to
    // update while the monitor is off.
    if (!m_output->adaptive_sync_supported || !m_output->enabled) {
      return;
    }
    const bool enabled = configuredVrrEnabled();
    const bool currentlyEnabled = m_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    if (enabled == currentlyEnabled) {
      return;
    }

    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
    if (!wlr_output_commit_state(m_output, &state)) {
      kLog.warn("output '{}': failed to {} VRR", m_output->name, enabled ? "enable" : "disable");
    } else {
      kLog.info("output '{}': VRR {}", m_output->name, enabled ? "enabled" : "disabled");
      m_server->updateOutputManagerConfig();
    }
    wlr_output_state_finish(&state);
  }

  wlr_output_layout_output* Output::addToLayout() {
    const OutputRule* rule = findRule(m_output->name);
    if (rule != nullptr && rule->position) {
      return wlr_output_layout_add(m_server->outputLayout(), m_output, (*rule->position)[0], (*rule->position)[1]);
    }
    return wlr_output_layout_add_auto(m_server->outputLayout(), m_output);
  }

  void Output::applyOutputState() {
    const HdrMode nextHdrMode = hdrMode();
    if (nextHdrMode == HdrMode::Auto) {
      m_fullscreenHdrRequested = false;
      if (!autoHdrEligible(m_autoHdrOwner)) {
        m_autoHdrOwner = findAutoHdrCandidate();
      }
    } else if (nextHdrMode == HdrMode::Fullscreen) {
      m_autoHdrOwner = nullptr;
      m_fullscreenHdrRequested = hasFullscreenView();
    } else {
      m_autoHdrOwner = nullptr;
      m_fullscreenHdrRequested = false;
    }
    if (!applyConfiguredState()) {
      return;
    }
    if (configuredEnabled()) {
      wlr_output_layout_output* layoutOutput = addToLayout();
      // Re-bind the scene output after a disable removed it from the layout.
      // No-op while it is still bound.
      wlr_scene_output_layout_add_output(m_server->sceneLayout(), layoutOutput, m_sceneOutput);
    } else {
      wlr_output_layout_remove(m_server->outputLayout(), m_output);
    }
    markDirty(Dirty::LayerArrange | Dirty::Banner);
    if (m_server->sessionLocked()) {
      m_server->updateLockBlank();
    }
    wlr_output_schedule_frame(m_output);
  }

  bool Output::setPowered(bool powered) {
    if (!configuredEnabled()) {
      return false;
    }
    const bool dpmsOff = !powered;
    if (m_dpmsOff == dpmsOff) {
      return true;
    }

    const bool previous = m_dpmsOff;
    m_dpmsOff = dpmsOff;
    if (!applyConfiguredState()) {
      m_dpmsOff = previous;
      return false;
    }

    if (powered) {
      m_gammaDirty = true;
      markDirty(Dirty::LayerArrange | Dirty::Banner | Dirty::Backdrop);
      if (m_server->sessionLocked()) {
        m_server->updateLockBlank();
      }
      wlr_output_schedule_frame(m_output);
    }
    m_server->updateOutputManagerConfig();
    return true;
  }

  void Output::applyCursorConfig() {
    const bool lockSoftwareCursor = !config().input.cursor.hardwareCursor;
    if (lockSoftwareCursor == m_softwareCursorLocked) {
      return;
    }
    wlr_output_lock_software_cursors(m_output, lockSoftwareCursor);
    m_softwareCursorLocked = lockSoftwareCursor;
    wlr_output_schedule_frame(m_output);
  }

  void Output::handleExternalConfigChange() {
    // Mode changes can drop the DRM gamma LUT; re-apply on the next frame.
    m_gammaDirty = true;
    markDirty(Dirty::LayerArrange);
    wlr_output_schedule_frame(m_output);
  }

  void Output::notifySurfaceScaleIter(wlr_surface* surface, int /*sx*/, int /*sy*/, void* data) {
    const auto* self = static_cast<Output*>(data);
    if (surface == nullptr || self == nullptr || self->m_output == nullptr) {
      return;
    }
    wlr_fractional_scale_v1_notify_scale(surface, self->m_output->scale);
    wlr_surface_set_preferred_buffer_scale(surface, static_cast<int32_t>(std::ceil(self->m_output->scale)));
  }

  Output::~Output() {
    if (m_frameRetryTimer != nullptr) {
      wl_event_source_remove(m_frameRetryTimer);
      m_frameRetryTimer = nullptr;
    }
    if (m_animationRenderLocked) {
      wlr_output_lock_attach_render(m_output, false);
      m_animationRenderLocked = false;
    }
    if (m_softwareCursorLocked) {
      wlr_output_lock_software_cursors(m_output, false);
      m_softwareCursorLocked = false;
    }
    if (m_output != nullptr && m_output->data == this) {
      m_output->data = nullptr;
    }
    if (m_frame.link.next != nullptr) {
      wl_list_remove(&m_frame.link);
      wl_list_remove(&m_requestState.link);
      wl_list_remove(&m_present.link);
      wl_list_remove(&m_destroy.link);
    }
    // Workspace destructors reparent leftover views onto the server trees, so the group has to go before the roots it
    // hangs under.
    m_workspaceGroup.reset();
    for (wlr_scene_tree*& layerTree : m_layerTrees) {
      if (layerTree != nullptr) {
        wlr_scene_node_destroy(&layerTree->node);
        layerTree = nullptr;
      }
    }
    if (m_popupTree != nullptr) {
      wlr_scene_node_destroy(&m_popupTree->node);
      m_popupTree = nullptr;
    }
    if (m_optimizedBlur != nullptr) {
      wlr_scene_node_destroy(&m_optimizedBlur->node);
      m_optimizedBlur = nullptr;
    }
    for (wlr_scene_tree* root : {m_viewRoot, m_fullscreenRoot, m_pinnedRoot, m_pinnedShadowRoot}) {
      if (root != nullptr) {
        wlr_scene_node_destroy(&root->node);
      }
    }
    m_viewRoot = nullptr;
    m_fullscreenRoot = nullptr;
    m_pinnedRoot = nullptr;
    m_pinnedShadowRoot = nullptr;
  }

  wlr_scene_tree* Output::layerTree(uint32_t layer) const {
    if (layer >= kLayerCount) {
      return m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
    }
    return m_layerTrees[layer];
  }

  void Output::arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive) {
    wlr_scene_node* node = nullptr;
    wl_list_for_each(node, &tree->children, link) {
      SceneNode* sceneNode = sceneNodeFrom(node->data);
      if (sceneNode == nullptr || sceneNode->kind != SceneNodeKind::LayerSurface) {
        continue;
      }
      auto* layerSurface = static_cast<LayerSurface*>(sceneNode);
      if (layerSurface->scene() == nullptr || layerSurface->arrangingOut()) {
        continue;
      }
      wlr_layer_surface_v1* surface = layerSurface->layerSurface();
      if (surface == nullptr || !surface->initialized) {
        continue;
      }
      // Only exclusive_zone > 0 participates in the exclusive pass.
      if ((surface->current.exclusive_zone > 0) != exclusive) {
        continue;
      }
      wlr_scene_layer_surface_v1_configure(layerSurface->scene(), fullArea, usableArea);
    }
  }

  void Output::arrangeLayers() {
    wlr_box fullArea{};
    wlr_output_effective_resolution(m_output, &fullArea.width, &fullArea.height);
    if (fullArea.width <= 0 || fullArea.height <= 0) {
      return;
    }

    wlr_box usableArea = fullArea;

    // Exclusive first, overlay down to background so higher layers win the zone.
    static constexpr uint32_t kExclusiveOrder[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    for (uint32_t layer : kExclusiveOrder) {
      arrangeLayer(m_layerTrees[layer], &fullArea, &usableArea, true);
    }
    for (uint32_t layer : kExclusiveOrder) {
      arrangeLayer(m_layerTrees[layer], &fullArea, &usableArea, false);
    }
    updateOptimizedBlur(fullArea);

    // Layer trees are output-local; pin them to the scene-output origin.
    for (auto& m_layerTree : m_layerTrees) {
      wlr_scene_node_set_position(&m_layerTree->node, m_sceneOutput->x, m_sceneOutput->y);
    }
    wlr_scene_node_set_position(&m_popupTree->node, m_sceneOutput->x, m_sceneOutput->y);

    // Content roots are clipped to this output's layout box, not repositioned: views are laid out in layout
    // coordinates. A disabled output never gets here (fullArea is empty above), and its workspaces have already been
    // evacuated, so the stale clip it keeps has nothing under it.
    const wlr_box outputBox = {
        .x = m_sceneOutput->x,
        .y = m_sceneOutput->y,
        .width = fullArea.width,
        .height = fullArea.height,
    };
    for (wlr_scene_tree* root : {m_viewRoot, m_fullscreenRoot, m_pinnedRoot, m_pinnedShadowRoot}) {
      wlr_scene_tree_set_clip(root, &outputBox);
    }

    // Keep the usable area output-local so layout moves take effect immediately,
    // without waiting for the next layer arrangement.
    m_localUsableArea = usableArea;
    m_arrangedLayoutX = m_sceneOutput->x;
    m_arrangedLayoutY = m_sceneOutput->y;

    const wlr_box layoutUsableArea = this->usableArea();
    kLog.debug(
        "{} usable={}x{}+{}+{}", m_output->name, layoutUsableArea.width, layoutUsableArea.height, layoutUsableArea.x,
        layoutUsableArea.y
    );
    if (m_workspaceGroup != nullptr && m_workspaceGroup->active() != nullptr) {
      m_workspaceGroup->active()->markArrange(false);
    }
  }

  void Output::updateOptimizedBlur(const wlr_box& fullArea) {
    const auto& blur = config().appearance.blur;
    if (!blur.enabled || !blur.optimized) {
      if (m_optimizedBlur != nullptr) {
        wlr_scene_node_destroy(&m_optimizedBlur->node);
        m_optimizedBlur = nullptr;
      }
      if (!blur.enabled) {
        fx_renderer_clear_output_effect_buffers(m_output);
      }
      return;
    }

    if (m_optimizedBlur == nullptr) {
      m_optimizedBlur = wlr_scene_optimized_blur_create(
          m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND), fullArea.width, fullArea.height
      );
      if (m_optimizedBlur == nullptr) {
        return;
      }
    }

    const bool changed = m_optimizedBlur->node.x != m_sceneOutput->x
        || m_optimizedBlur->node.y != m_sceneOutput->y
        || m_optimizedBlur->width != fullArea.width
        || m_optimizedBlur->height != fullArea.height;
    wlr_scene_node_set_enabled(&m_optimizedBlur->node, true);
    wlr_scene_node_set_position(&m_optimizedBlur->node, m_sceneOutput->x, m_sceneOutput->y);
    wlr_scene_optimized_blur_set_size(m_optimizedBlur, fullArea.width, fullArea.height);
    if (changed) {
      wlr_scene_optimized_blur_mark_dirty(m_optimizedBlur);
    }
  }

  void Output::markBlurBackgroundDirty() {
    if (m_optimizedBlur != nullptr) {
      wlr_scene_optimized_blur_mark_dirty(m_optimizedBlur);
    }
  }

  void Output::onGammaChanged(wlr_gamma_control_v1* control) {
    if (hdrActive()) {
      rejectGammaControl(control);
      return;
    }
    // DRM gamma LUT upload is expensive; apply once on change, not every frame.
    m_gammaDirty = true;
    wlr_output_schedule_frame(m_output);
  }

  void Output::onFrame(wl_listener* listener, void* /*data*/) {
    Output* self;
    self = wl_container_of(listener, self, m_frame);
    self->handleFrame();
  }

  void Output::onRequestState(wl_listener* listener, void* data) {
    Output* self;
    self = wl_container_of(listener, self, m_requestState);
    self->handleRequestState(data);
  }

  void Output::onPresent(wl_listener* listener, void* data) {
    Output* self;
    self = wl_container_of(listener, self, m_present);
    self->handlePresent(data);
  }

  void Output::onDestroy(wl_listener* listener, void* /*data*/) {
    Output* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  int Output::onFrameRetryTimer(void* data) {
    auto* self = static_cast<Output*>(data);
    if (outputFrameAllowed(self->m_server->stopping(), self->m_server->session())) {
      wlr_output_schedule_frame(self->m_output);
    }
    return 0;
  }

  void Output::armFrameRetry() {
    if (m_frameRetryTimer != nullptr && outputFrameAllowed(m_server->stopping(), m_server->session())) {
      wl_event_source_timer_update(m_frameRetryTimer, kFrameRetryDelayMs);
    }
  }

  void Output::applyMode(int width, int height) {
    if (width <= 0 || height <= 0) {
      return;
    }

    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, width, height, 0);
    if (!wlr_output_commit_state(m_output, &state)) {
      wlr_log(WLR_ERROR, "failed to commit output mode %dx%d for '%s'", width, height, m_output->name);
    } else {
      // Mode changes can drop the DRM gamma LUT; re-apply on the next frame.
      m_gammaDirty = true;
    }
    wlr_output_state_finish(&state);
    markDirty(Dirty::LayerArrange | Dirty::Banner | Dirty::Backdrop);
    if (m_server->sessionLocked()) {
      m_server->updateLockBlank();
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::markDirty(Dirty what) {
    m_dirty |= what;
    wlr_output_schedule_frame(m_output);
  }

  void Output::flushDirty() {
    // Server-wide chrome is recorded on the Server and flushed by whichever
    // output frames first; each of these is idempotent and cheap.
    Dirty pending = m_dirty | m_server->takeDirty();
    m_dirty = Dirty::None;
    // Order matters: exclusive zones define the usable area, the layout fills
    // it, and the chrome sits over the result.
    if (has(pending, Dirty::LayerArrange)) {
      arrangeLayers();
      // Changing the usable area makes the layout stale, so arrangeLayers marks it, after this set was taken. Take
      // again rather than let that wait a frame; the same holds for anything a later step records for a step further
      // down.
      pending |= m_dirty;
      m_dirty = Dirty::None;
    }
    if (has(pending, Dirty::Layout) && m_workspaceGroup != nullptr) {
      m_workspaceGroup->flushArrange();
    }
    if (has(pending, Dirty::Banner)) {
      m_server->relayoutBanner();
    }
    if (has(pending, Dirty::Backdrop)) {
      m_server->updateBackdrop();
    }
    if (has(pending, Dirty::Cheatsheet)) {
      m_server->relayoutCheatsheet();
    }
    if (has(pending, Dirty::QuitConfirm)) {
      m_server->relayoutQuitConfirm();
    }
  }

  void Output::handleFrame() {
    // A failed DRM commit can immediately queue another frame after logind revokes device access. Stop before that
    // retry loop can keep the final event-loop dispatch alive. A null session belongs to a nested or headless backend
    // and remains renderable.
    if (!outputFrameAllowed(m_server->stopping(), m_server->session())) {
      return;
    }
    if (m_frameRetryTimer != nullptr) {
      wl_event_source_timer_update(m_frameRetryTimer, 0);
    }

    flushDirty();
    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t nowMsec = static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1'000'000;
    m_server->tickAnimations(nowMsec);

    // Surface commits reset scene-buffer opacity to the protocol alpha. Repair
    // pending rule opacity after every commit listener and before composition.
    m_server->flushPendingViewOpacities();

    // A direct-scanned fullscreen client may stop submitting as soon as it loses focus. On VRR outputs that can leave
    // the first workspace-switch frame waiting on the old client, so the compositor never gets a vblank to advance the
    // slide. Keep animated outputs on the render path until their final composed frame has settled.
    const bool animationsActive = m_server->animationsActiveFor(this);
    if (animationsActive != m_animationRenderLocked) {
      wlr_output_lock_attach_render(m_output, animationsActive);
      m_animationRenderLocked = animationsActive;
    }

    // wlroots only knows about descriptions owned by its color manager and resets
    // compatibility-managed Wine buffers to SDR on every surface commit. Restore
    // their descriptions at the render boundary so no frame can observe that
    // transient default state.
    if (WineColorManager* colorManager = m_server->wineColorManager()) {
      colorManager->applySurfaceDescriptions();
    }

    const int externalRenderLocks = m_output->attach_render_locks - (m_animationRenderLocked ? 1 : 0);
    const bool captureActive = externalRenderLocks > 0;
    View* tearingView = tearingCandidate();
    const bool tearingPolicyRequested = tearingEligible(tearingView);

    bool tearingFrameEligible = tearingPolicyRequested;
    if (!configuredTearingAllowed()) {
      m_tearingFallbackReason.clear();
    } else if (tearingView == nullptr) {
      m_tearingFallbackReason.clear();
    } else if (const std::optional<bool> rule = tearingView->resolvedRules().allowTearing; rule && !*rule) {
      m_tearingFallbackReason.clear();
    } else if (!tearingView->resolvedRules().allowTearing && !clientTearingHintAsync(tearingView)) {
      m_tearingFallbackReason.clear();
    } else if (m_server->sessionLocked()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "session locked";
    } else if (m_server->overview() != nullptr && m_server->overview()->active()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "overview active";
    } else if (m_server->configBanner() != nullptr && m_server->configBanner()->visible()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "configuration banner visible";
    } else if (m_server->cheatsheet() != nullptr && m_server->cheatsheet()->visible()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "cheatsheet visible";
    } else if (m_server->quitConfirm() != nullptr && m_server->quitConfirm()->visible()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "quit confirmation visible";
    } else if (animationsActive) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "output animation active";
    } else if (captureActive) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "output capture active";
    } else if (m_tearingRecovery.regularCommitPending()) {
      tearingFrameEligible = false;
      m_tearingFallbackReason = "recovering from failed async commit";
    } else {
      m_tearingFallbackReason.clear();
    }
    const bool requestTearing = m_tearingRecovery.requestTearing(tearingFrameEligible);

    if (m_output->width <= 0 || m_output->height <= 0) {
      // Output not configured yet; no clients can be presenting on it either.
      return;
    }

    // Render + commit only if the scene actually changed or a gamma upload is pending. All exit paths below MUST reach
    // the unconditional wlr_scene_output_send_frame_done call at the bottom: mailbox/FIFO clients (games via DXVK,
    // video players) block on wl_surface.frame before submitting their next buffer. If we skip frame_done on the
    // "nothing to render" path, they never commit again -> damage stays clean -> wlr_scene_output_needs_frame returns
    // false forever -> compositor parks in epoll_wait. (Reproducible with any mailbox/FIFO Vulkan game.)
    bool commitFailed = false;
    if (wlr_scene_output_needs_frame(m_sceneOutput) || m_gammaDirty) {
      m_inFrame = true;

      wlr_output_state state{};
      wlr_output_state_init(&state);

      bool commitOk = false;
      int captureLocks = externalRenderLocks;
      if (wlr_export_dmabuf_manager_v1* manager = m_server->exportDmabufManager()) {
        wlr_export_dmabuf_frame_v1* frame;
        wl_list_for_each(frame, &manager->frames, link) {
          if (frame->output == m_output) {
            --captureLocks;
          }
        }
      }
      wlr_scene_output_state_options sceneOptions{};
      sceneOptions.capture_sdr = hdrActive() && captureLocks > 0;
      if (wlr_scene_output_build_state(m_sceneOutput, &state, &sceneOptions)) {
        // Hardware gamma only (DRM). Nested Wayland has no gamma LUT; leave that alone.
        // Apply only when dirty: uploading the LUT every frame stalls the compositor.
        bool gammaPending = false;
        if (m_gammaDirty) {
          if (hdrActive()) {
            rejectGammaControl(wlr_gamma_control_manager_v1_get_control(m_server->gammaManager(), m_output));
          } else if (wlr_output_get_gamma_size(m_output) > 0) {
            wlr_gamma_control_v1* control =
                wlr_gamma_control_manager_v1_get_control(m_server->gammaManager(), m_output);
            if (!wlr_gamma_control_v1_apply(control, &state)) {
              if (control != nullptr) {
                wlr_gamma_control_v1_send_failed_and_destroy(control);
              }
              m_gammaDirty = false;
            } else {
              gammaPending = true;
            }
          } else {
            m_gammaDirty = false;
          }
        }

        const bool hasBuffer = (state.committed & WLR_OUTPUT_STATE_BUFFER) != 0;
        bool commitTearing = requestTearing && hasBuffer;
        const bool recoveringFromFailedCommit = m_tearingRecovery.regularCommitPending();
        bool backendRejectedTearing = false;
        if (commitTearing) {
          state.tearing_page_flip = true;
          if (!wlr_output_test_state(m_output, &state)) {
            state.tearing_page_flip = false;
            commitTearing = false;
            backendRejectedTearing = true;
            m_tearingRecovery.forceRegularCommit();
            m_tearingFallbackReason = "backend rejected async page flip";
          }
        }

        commitOk = wlr_output_commit_state(m_output, &state);
        if (hasBuffer) {
          m_tearingRecovery.recordCommit(commitTearing, commitOk);
        }
        if (commitOk && gammaPending) {
          m_gammaDirty = false;
        }
        if (commitOk && hasBuffer) {
          m_lastCommitTearing = commitTearing;
          m_trackingPresentation = true;
          m_trackedPresentationCommitSeq = m_output->commit_seq;
          m_lastPresentationPresented.reset();
          m_lastPresentationFlags.reset();
          if (commitTearing) {
            m_tearingFallbackReason.clear();
          } else if (
              recoveringFromFailedCommit && !backendRejectedTearing && !m_tearingRecovery.regularCommitPending()
          ) {
            m_tearingFallbackReason = "recovered with regular page flip";
          }
        } else if (commitTearing) {
          m_tearingFallbackReason = "async page flip commit failed";
        } else if (hasBuffer && m_tearingRecovery.regularCommitPending()) {
          m_tearingFallbackReason = "regular recovery commit failed";
        }
      }

      wlr_output_state_finish(&state);
      m_inFrame = false;
      commitFailed = !commitOk;
    }

    // A request_state that arrived mid-commit is applied now that we're out of it.
    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
    }

    if (commitFailed && m_output->idle_frame != nullptr) {
      // Damage, animation, or deferred output work can schedule another idle frame while this frame callback is still
      // running. Remove it before arming the timer, otherwise the idle dispatcher can still recurse without returning
      // to signals.
      wl_event_source_remove(m_output->idle_frame);
      m_output->idle_frame = nullptr;
    }

    // A failed commit has no vblank to pace an immediate retry. Defer it so event-loop signal and session sources get
    // dispatched first. This also replaces animation scheduling for the failed frame, otherwise the animation path
    // would recreate the same immediate retry loop.
    switch (outputFrameFollowup(m_server->stopping(), m_server->session(), commitFailed, animationsActive)) {
    case OutputFrameFollowup::Schedule:
      wlr_output_schedule_frame(m_output);
      break;
    case OutputFrameFollowup::RetryDelayed:
      armFrameRetry();
      break;
    case OutputFrameFollowup::None:
      break;
    }

    // Unconditional: see comment above. Never gate this on commit success.
    wlr_scene_output_send_frame_done(m_sceneOutput, &now);
  }

  void Output::handleRequestState(void* data) {
    auto* event = static_cast<wlr_output_event_request_state*>(data);

    // Parent configure can arrive while we are flushing a frame commit. Applying a
    // mode change mid-frame makes the wayland backend reject the primary buffer.
    if (m_inFrame
        && (event->state->committed & WLR_OUTPUT_STATE_MODE) != 0
        && event->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
      m_deferredWidth = event->state->custom_mode.width;
      m_deferredHeight = event->state->custom_mode.height;
      m_hasDeferredMode = true;
      return;
    }

    if (!wlr_output_commit_state(m_output, event->state)) {
      wlr_log(WLR_ERROR, "failed to commit requested output state for '%s'", m_output->name);
      return;
    }
    if ((event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED)) != 0) {
      markDirty(Dirty::LayerArrange | Dirty::Banner | Dirty::Backdrop);
      m_gammaDirty = true;
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::handlePresent(void* data) {
    const auto* event = static_cast<const wlr_output_event_present*>(data);
    if (!m_trackingPresentation || event->commit_seq != m_trackedPresentationCommitSeq) {
      return;
    }
    m_trackingPresentation = false;
    m_lastPresentationPresented = event->presented;
    if (event->presented) {
      m_lastPresentationFlags = event->flags;
    } else {
      m_lastPresentationFlags.reset();
      if (m_lastCommitTearing) {
        m_tearingRecovery.forceRegularCommit();
        m_tearingFallbackReason = "async page flip was not presented";
        wlr_damage_ring_add_whole(&m_sceneOutput->damage_ring);
        if (outputFrameAllowed(m_server->stopping(), m_server->session())) {
          wlr_output_schedule_frame(m_output);
        }
      }
    }
  }

  void Output::handleDestroy() {
    if (m_output->data == this) {
      m_output->data = nullptr;
    }
    wl_list_remove(&m_frame.link);
    wl_list_remove(&m_requestState.link);
    wl_list_remove(&m_present.link);
    wl_list_remove(&m_destroy.link);
    m_frame.link.next = nullptr;
    m_requestState.link.next = nullptr;
    m_present.link.next = nullptr;
    m_destroy.link.next = nullptr;
    if (m_optimizedBlur != nullptr && m_server->scene() != nullptr) {
      wlr_scene_node_destroy(&m_optimizedBlur->node);
    }
    m_optimizedBlur = nullptr;
    m_server->removeOutput(this);
  }

} // namespace umbriel
