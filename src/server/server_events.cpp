#include "config/change.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "config/resolve.h"
#include "config/store.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/gestures.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/ipc.h"
#include "server/server.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <vector>

namespace umbriel {

  namespace {
    constexpr Logger kLog("server");

    // Dynamic workspaces are numbered, static ones can be named; sort the numbers by value and push names to the end.
    size_t workspaceOrder(std::string_view name) {
      size_t index = 0;
      const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), index);
      return error == std::errc{} && end == name.data() + name.size() ? index : std::numeric_limits<size_t>::max();
    }

    View* viewForSurface(Server& server, wlr_surface* surface) {
      if (surface == nullptr) {
        return nullptr;
      }
      wlr_surface* root = wlr_surface_get_root_surface(surface);
      wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
      if (toplevel == nullptr) {
        return nullptr;
      }
      for (const auto& entry : server.registry().all()) {
        if (entry->toplevel() == toplevel) {
          return entry.get();
        }
      }
      return nullptr;
    }

    pid_t surfaceClientPid(wlr_surface* surface) {
      if (surface == nullptr || surface->resource == nullptr) {
        return -1;
      }
      pid_t pid = -1;
      uid_t uid = 0;
      gid_t gid = 0;
      wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, &uid, &gid);
      return pid;
    }

    const char* deviceName(const wlr_input_device* device) {
      return device->name != nullptr ? device->name : "unknown";
    }

    wlr_xdg_toplevel_decoration_v1_mode resolvedDecorationMode(wlr_xdg_toplevel_decoration_v1* decoration) {
      // Honor an explicit client request; otherwise prefer SSD when configured.
      wlr_xdg_toplevel_decoration_v1_mode mode = decoration->requested_mode;
      if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
        mode = config().appearance.preferNoCsd ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                               : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
      }
      return mode;
    }

    struct XdgDecorationWatch {
      wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
      wlr_xdg_toplevel_decoration_v1_mode pendingMode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
      wl_listener requestMode{};
      wl_listener surfaceCommit{};
      wl_listener destroy{};
    };

    void clearDecorationCommit(XdgDecorationWatch* watch) {
      if (watch->surfaceCommit.notify == nullptr) {
        return;
      }
      wl_list_remove(&watch->surfaceCommit.link);
      watch->surfaceCommit.notify = nullptr;
    }

    void onDecorationSurfaceCommit(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, surfaceCommit);
      if (watch->decoration == nullptr
          || watch->decoration->toplevel == nullptr
          || !watch->decoration->toplevel->base->initial_commit) {
        return;
      }
      wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
      clearDecorationCommit(watch);
    }

    void applyXdgDecorationMode(XdgDecorationWatch* watch) {
      if (watch == nullptr || watch->decoration == nullptr || watch->decoration->toplevel == nullptr) {
        return;
      }
      wlr_xdg_surface* surface = watch->decoration->toplevel->base;
      if (surface == nullptr) {
        return;
      }
      watch->pendingMode = resolvedDecorationMode(watch->decoration);
      // set_mode schedules a configure; that asserts unless the xdg_surface is ready.
      if (surface->initialized) {
        clearDecorationCommit(watch);
        wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
        return;
      }
      if (watch->surfaceCommit.notify != nullptr) {
        return;
      }
      watch->surfaceCommit.notify = onDecorationSurfaceCommit;
      wl_signal_add(&surface->surface->events.commit, &watch->surfaceCommit);
    }

    void applyKdeDecorationDefault(wlr_server_decoration_manager* manager) {
      if (manager == nullptr) {
        return;
      }
      const uint32_t mode = config().appearance.preferNoCsd ? WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
                                                            : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
      wlr_server_decoration_manager_set_default_mode(manager, mode);
    }

    void onDecorationRequestMode(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, requestMode);
      applyXdgDecorationMode(watch);
    }

    void onDecorationDestroy(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (watch->decoration != nullptr) {
        watch->decoration->data = nullptr;
      }
      wl_list_remove(&watch->requestMode.link);
      clearDecorationCommit(watch);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

    void applyNaturalScroll(
        libinput_device* libinputDevice, const wlr_input_device* device, const std::optional<bool>& configured,
        std::string_view setting
    ) {
      if (libinput_device_config_scroll_has_natural_scroll(libinputDevice) == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      const bool enabled =
          configured.value_or(libinput_device_config_scroll_get_default_natural_scroll_enabled(libinputDevice) != 0);
      if (libinput_device_config_scroll_set_natural_scroll_enabled(libinputDevice, enabled)
          != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
      }
    }

    void applyMouseAcceleration(
        libinput_device* libinputDevice, const wlr_input_device* device,
        const std::optional<AccelProfile>& configuredProfile, const std::optional<double>& configuredSensitivity,
        std::string_view accelSetting, std::string_view sensitivitySetting
    ) {
      if (libinput_device_config_accel_is_available(libinputDevice) == 0) {
        return;
      }

      if (!configuredProfile) {
        const auto profile = libinput_device_config_accel_get_default_profile(libinputDevice);
        if (libinput_device_config_accel_set_profile(libinputDevice, profile) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to restore the default acceleration profile for '{}'", deviceName(device));
          return;
        }
      } else {
        enum libinput_config_accel_profile profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
        const char* profileName = "flat";
        switch (configuredProfile->kind) {
        case AccelProfile::Kind::Flat:
          break;
        case AccelProfile::Kind::Adaptive:
          profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
          profileName = "adaptive";
          break;
        case AccelProfile::Kind::Custom:
          profile = LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM;
          profileName = "custom";
          break;
        }
        if ((libinput_device_config_accel_get_profiles(libinputDevice) & profile) == 0) {
          kLog.warn("input: '{}' does not support the {} acceleration profile", deviceName(device), profileName);
          return;
        }

        if (configuredProfile->kind == AccelProfile::Kind::Custom) {
          libinput_config_accel* acceleration = libinput_config_accel_create(profile);
          if (acceleration == nullptr) {
            kLog.warn("input: failed to create custom acceleration profile for '{}'", deviceName(device));
            return;
          }
          const auto pointsStatus = libinput_config_accel_set_points(
              acceleration, LIBINPUT_ACCEL_TYPE_MOTION, configuredProfile->step, configuredProfile->points.size(),
              configuredProfile->points.data()
          );
          const auto applyStatus = pointsStatus == LIBINPUT_CONFIG_STATUS_SUCCESS
              ? libinput_device_config_accel_apply(libinputDevice, acceleration)
              : pointsStatus;
          libinput_config_accel_destroy(acceleration);
          if (applyStatus != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            kLog.warn("input: failed to apply {} to '{}'", accelSetting, deviceName(device));
          }
          return;
        }

        if (libinput_device_config_accel_set_profile(libinputDevice, profile) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to apply {} to '{}'", accelSetting, deviceName(device));
          return;
        }
      }

      const double sensitivity =
          configuredSensitivity.value_or(libinput_device_config_accel_get_default_speed(libinputDevice));
      if (libinput_device_config_accel_set_speed(libinputDevice, sensitivity) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        if (configuredSensitivity) {
          kLog.warn("input: failed to apply {} to '{}'", sensitivitySetting, deviceName(device));
        } else {
          kLog.warn("input: failed to restore the default acceleration speed for '{}'", deviceName(device));
        }
      }
    }

    void applyPointerConfig(wlr_input_device* device) {
      if (!wlr_input_device_is_libinput(device)) {
        return;
      }
      libinput_device* libinputDevice = wlr_libinput_get_device_handle(device);
      if (libinputDevice == nullptr) {
        return;
      }

      const Config::Input& input = config().input;
      const Config::Input::Device* override = input.findDevice(device->name != nullptr ? device->name : "");
      const bool isTouchpad = libinput_device_config_tap_get_finger_count(libinputDevice) > 0;
      if (isTouchpad) {
        const std::optional<bool>& tap = override != nullptr && override->tap ? override->tap : input.touchpad.tap;
        const auto tapState =
            tap.value_or(libinput_device_config_tap_get_default_enabled(libinputDevice) == LIBINPUT_CONFIG_TAP_ENABLED)
            ? LIBINPUT_CONFIG_TAP_ENABLED
            : LIBINPUT_CONFIG_TAP_DISABLED;
        if (libinput_device_config_tap_set_enabled(libinputDevice, tapState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn(
              "input: failed to apply {} to '{}'",
              override != nullptr && override->tap ? "input.device.tap" : "input.touchpad.tap", deviceName(device)
          );
        }

        const bool hasDwtOverride = override != nullptr && override->disableWhileTyping.has_value();
        const std::optional<bool>& dwt =
            hasDwtOverride ? override->disableWhileTyping : input.touchpad.disableWhileTyping;
        const std::string_view dwtSetting =
            hasDwtOverride ? "input.device.disable_while_typing" : "input.touchpad.disable_while_typing";
        if (libinput_device_config_dwt_is_available(libinputDevice)) {
          const auto dwtState =
              dwt.value_or(
                  libinput_device_config_dwt_get_default_enabled(libinputDevice) == LIBINPUT_CONFIG_DWT_ENABLED
              )
              ? LIBINPUT_CONFIG_DWT_ENABLED
              : LIBINPUT_CONFIG_DWT_DISABLED;
          if (libinput_device_config_dwt_set_enabled(libinputDevice, dwtState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            if (dwt) {
              kLog.warn("input: failed to apply {} to '{}'", dwtSetting, deviceName(device));
            } else {
              kLog.warn("input: failed to restore default disable-while-typing for '{}'", deviceName(device));
            }
          }
        } else if (dwt) {
          kLog.warn(
              "input: could not apply {} to '{}': device does not support disable-while-typing", dwtSetting,
              deviceName(device)
          );
        }
      }

      const std::optional<bool>& naturalScroll = override != nullptr && override->naturalScroll
          ? override->naturalScroll
          : isTouchpad ? input.touchpad.naturalScroll
                       : input.mouse.naturalScroll;
      applyNaturalScroll(
          libinputDevice, device, naturalScroll,
          override != nullptr && override->naturalScroll ? "input.device.natural_scroll"
              : isTouchpad                               ? "input.touchpad.natural_scroll"
                                                         : "input.mouse.natural_scroll"
      );

      const std::optional<AccelProfile> accelProfile = override != nullptr && override->accelProfile
          ? override->accelProfile
          : isTouchpad ? input.touchpad.accelProfile
                       : std::optional{input.mouse.accelProfile};
      const std::optional<double> sensitivity = override != nullptr && override->sensitivity ? override->sensitivity
          : isTouchpad ? input.touchpad.sensitivity
                       : std::optional{input.mouse.sensitivity};
      applyMouseAcceleration(
          libinputDevice, device, accelProfile, sensitivity,
          override != nullptr && override->accelProfile ? "input.device.accel_profile"
              : isTouchpad                              ? "input.touchpad.accel_profile"
                                                        : "input.mouse.accel_profile",
          override != nullptr && override->sensitivity ? "input.device.sensitivity"
              : isTouchpad                             ? "input.touchpad.sensitivity"
                                                       : "input.mouse.sensitivity"
      );
    }
  } // namespace
  void Server::applyConfig(const ConfigEffects& effects) {
    if (!effects.any()) {
      return;
    }

    if (effects.sceneBlur) {
      const Config::Appearance::Blur& blur = config().appearance.blur;
      wlr_scene_set_blur_data(
          m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
          static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
      );
      for (const auto& output : m_outputs) {
        output->markDirty(Dirty::LayerArrange);
      }
    }
    if (effects.input) {
      for (const auto& keyboard : m_keyboards) {
        keyboard->applyConfig();
      }
      for (const auto& pointer : m_pointers) {
        applyPointerConfig(pointer->device);
      }
      for (const auto& tablet : m_tabletDevices) {
        applyTabletConfig(*tablet);
      }
      for (const auto& pad : m_tabletPads) {
        applyTabletPadConfig(*pad);
      }
      m_seat->applyConfig();
      m_cursor->applyConfig();
      for (const auto& output : m_outputs) {
        output->applyCursorConfig();
      }
    }
    if (effects.internalUi) {
      markDirty(Dirty::Cheatsheet);
    }
    if (effects.outputState) {
      for (const auto& output : m_outputs) {
        output->applyOutputState();
      }
      for (const auto& output : m_outputs) {
        if (output->wlr()->enabled) {
          continue;
        }
        Output* fallback = nullptr;
        for (const auto& candidate : m_outputs) {
          if (candidate.get() != output.get() && candidate->wlr()->enabled) {
            fallback = candidate.get();
            break;
          }
        }
        reassignOutputViews(output.get(), fallback);
      }
      scheduleDisplacedViewRestore();
      updateOutputManagerConfig();
      // A disabled output must not keep keyboard focus: pull it onto a live one.
      refocus();
      // Scale may have changed; every surface must hear about it or clients
      // like xwayland-satellite keep mapping input with the stale scale.
      refreshSurfaceScales();
    }
    if (effects.tearingPolicy) {
      for (const auto& output : m_outputs) {
        output->resetTearingState();
      }
    }
    if (effects.directScanoutPolicy) {
      for (const auto& output : m_outputs) {
        output->applyDirectScanoutConfig();
      }
    }
    if (effects.workspaceInventory) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->reconcileInventory();
        }
      }
    }
    if (effects.workspaceLayout) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->refreshLayouts();
        }
      }
      m_cursor->cancelStaleTiledResize();
    }
    if (effects.viewChrome) {
      for (const auto& view : m_registry.all()) {
        if (view->mapped()) {
          view->refreshConfigChrome();
        }
      }
      applyKdeDecorationDefault(m_serverDecorationManager);
      if (m_xdgDecorationManager != nullptr) {
        wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
        wl_list_for_each(decoration, &m_xdgDecorationManager->decorations, link) {
          if (auto* watch = static_cast<XdgDecorationWatch*>(decoration->data)) {
            applyXdgDecorationMode(watch);
          }
        }
      }
      // The view refresh cleared every focus ring; put the active one back.
      refocus();
      markDirty(Dirty::Backdrop);
      if (m_sessionLocked) {
        updateLockBlank();
      }
    }
    if (effects.animation && m_scratchpadManager != nullptr) {
      m_scratchpadManager->applyConfig();
    }
    if (effects.layerEffects) {
      for (const auto& layer : m_layerSurfaces) {
        if (layer->mapped()) {
          layer->applyConfig();
        }
      }
    }
  }

  void Server::handleConfigReload() {
    cancelModifierTap();
    const ConfigReloadResult result = reloadConfig();
    if (result.success) {
      if (result.effects.invalidatesOverview()) {
        m_overview->forceClose();
      }
      applyConfig(result.effects);
      if ((result.change.colors || result.change.appearance) && m_ipc != nullptr) {
        m_ipc->notifyThemeChanged();
      }
      const std::string changed = result.change.summary();
      const std::string effects = result.effects.summary();
      kLog.info(
          "config reloaded (sections: {}; effects: {})", changed.empty() ? "none" : changed,
          effects.empty() ? "none" : effects
      );
    }
    showConfigDiagnostics();
    markDirty(Dirty::Cheatsheet);
    if (m_configWatcher != nullptr) {
      m_configWatcher->watch(configWatchPaths());
    }
  }

  // Slow tick that keeps hidden-workspace toplevels driving their game/network loops (see kBackgroundFrameIntervalMs).
  // wlr_scene_output_send_frame_done walks only enabled scene nodes, so a view whose workspace has been deactivated
  // stops receiving wl_surface.frame callbacks entirely; any client that gates advance-work on the callback stalls
  // until it is shown again.
  int Server::onBackgroundFrameTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (const auto& view : self->m_registry.all()) {
      if (!view->mapped() || view->onActiveWorkspace()) {
        continue;
      }
      wlr_xdg_surface_for_each_surface(
          view->toplevel()->base,
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }

    if (self->m_backgroundFrameTimer != nullptr) {
      wl_event_source_timer_update(self->m_backgroundFrameTimer, kBackgroundFrameIntervalMs);
    }
    return 0;
  }

  // Fires when the underlying GL context is invalidated (GPU reset, VRAM lost after suspend, driver-detected hang).
  // Without this, the renderer keeps issuing GL calls into a dead context: Mesa's context_lost_nop_handler no-ops each
  // one and spams "[GLES2] GL_CONTEXT_LOST in context lost" ~40k lines/sec, and the desktop never comes back. Rebuild
  // the renderer and rebind everything.
  void Server::onRendererLost(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_rendererLost);
    self->recreateRenderer();
  }

  void Server::recreateRenderer() {
    kLog.warn("GPU context lost, recreating renderer");

    wlr_renderer* oldRenderer = m_renderer;
    wlr_allocator* oldAllocator = m_allocator;

    wlr_renderer* newRenderer = fx_renderer_create(m_backend);
    if (newRenderer == nullptr) {
      kLog.error("could not recreate fx_renderer after GPU reset, terminating");
      stop();
      return;
    }
    wlr_allocator* newAllocator = wlr_allocator_autocreate(m_backend, newRenderer);
    if (newAllocator == nullptr) {
      kLog.error("could not recreate allocator after GPU reset, terminating");
      wlr_renderer_destroy(newRenderer);
      stop();
      return;
    }

    // Rewire the lost signal onto the new renderer BEFORE swapping the pointers so that
    // a second reset during recreation is delivered.
    wl_list_remove(&m_rendererLost.link);
    wl_signal_add(&newRenderer->events.lost, &m_rendererLost);

    m_renderer = newRenderer;
    m_allocator = newAllocator;

    // Point the compositor at the new renderer so clients' shm/dma-buf textures get
    // re-imported on next attach.
    wlr_compositor_set_renderer(m_compositor, newRenderer);

    // Re-init every output's render pipeline with the new renderer/allocator, and force
    // a fresh frame so damage tracking rebuilds from scratch.
    for (const auto& output : m_outputs) {
      wlr_output* wlrOutput = output->wlr();
      wlr_output_init_render(wlrOutput, newAllocator, newRenderer);
      wlr_output_schedule_frame(wlrOutput);
    }

    wlr_allocator_destroy(oldAllocator);
    wlr_renderer_destroy(oldRenderer);

    kLog.info("renderer recreated");
  }

  void Server::onNewOutput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newOutput);
    self->addOutput(static_cast<wlr_output*>(data));
  }

  void Server::onNewInput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newInput);
    auto* device = static_cast<wlr_input_device*>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
      self->addKeyboard(device);
      break;
    case WLR_INPUT_DEVICE_POINTER:
      self->addPointer(device);
      break;
    case WLR_INPUT_DEVICE_TOUCH:
      self->addTouch(device);
      break;
    case WLR_INPUT_DEVICE_TABLET:
      self->addTablet(device);
      break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
      self->addTabletPad(device);
      break;
    default:
      break;
    }
    self->updateSeatCapabilities();
  }

  void Server::onNewXdgToplevel(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgToplevel);
    self->m_registry.add(std::make_unique<View>(*self, static_cast<wlr_xdg_toplevel*>(data)));
  }

  void Server::onNewXdgPopup(wl_listener* /*listener*/, void* data) {
    auto* popup = static_cast<wlr_xdg_popup*>(data);
    // Layer-shell popups (and any popup without a parent yet) are handled
    // elsewhere; parent can be null when xdg_shell emits new_popup.
    if (popup->parent == nullptr || wlr_xdg_surface_try_from_wlr_surface(popup->parent) == nullptr) {
      return;
    }
    View* owner = View::fromSurface(popup->parent);
    new Popup(popup, nullptr, owner != nullptr ? owner->captureTree() : nullptr);
  }

  void Server::onNewXdgDecoration(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgDecoration);
    auto* decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);
    auto* watch = new XdgDecorationWatch{.decoration = decoration};
    decoration->data = watch;
    watch->requestMode.notify = onDecorationRequestMode;
    wl_signal_add(&decoration->events.request_mode, &watch->requestMode);
    watch->destroy.notify = onDecorationDestroy;
    wl_signal_add(&decoration->events.destroy, &watch->destroy);
    applyXdgDecorationMode(watch);
    (void)self;
  }

  void Server::onNewLayerSurface(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newLayerSurface);
    auto surface = std::make_unique<LayerSurface>(*self, static_cast<wlr_layer_surface_v1*>(data));
    if (surface->layerSurface() == nullptr) {
      return;
    }
    self->m_layerSurfaces.push_back(std::move(surface));
  }

  void Server::onNewSessionLock(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newSessionLock);
    self->beginSessionLock(static_cast<wlr_session_lock_v1*>(data));
  }

  void Server::onNewPointerConstraint(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newPointerConstraint);
    self->m_cursor->handleNewConstraint(static_cast<wlr_pointer_constraint_v1*>(data));
  }

  void Server::onNewVirtualKeyboard(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newVirtualKeyboard);
    auto* keyboard = static_cast<wlr_virtual_keyboard_v1*>(data);
    self->addKeyboard(&keyboard->keyboard.base);
    self->updateSeatCapabilities();
  }

  void Server::onNewVirtualPointer(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newVirtualPointer);
    auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
    auto* vpointer = event->new_pointer;

    auto device = std::make_unique<VirtualPointerDevice>();
    device->server = self;
    device->vpointer = vpointer;
    device->destroy.notify = onVirtualPointerDestroy;
    wl_resource_add_destroy_listener(vpointer->resource, &device->destroy);

    // Attach to the cursor exactly like a physical pointer (see addPointer), so a virtual pointer runs the same Cursor
    // pipeline: hover, click-to-focus, mouse binds, and interactive move and resize. Hand-wiring these signals instead
    // only warped the cursor and forwarded buttons to the seat, so a virtual pointer could move the cursor but never
    // focus or drag anything.
    self->m_cursor->attachInputDevice(&vpointer->pointer.base);

    self->m_virtualPointers.push_back(std::move(device));
  }

  void Server::onVirtualPointerDestroy(wl_listener* listener, void* /*data*/) {
    VirtualPointerDevice* device;
    device = wl_container_of(listener, device, destroy);
    // wlr_cursor detaches the device itself when the pointer is destroyed.
    wl_list_remove(&device->destroy.link);
    std::erase_if(device->server->m_virtualPointers, [device](const std::unique_ptr<VirtualPointerDevice>& ptr) {
      return ptr.get() == device;
    });
  }

  void Server::onNewIdleInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newIdleInhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    auto* watch = new IdleInhibitorWatch();
    watch->server = self;
    watch->destroy.notify = onIdleInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->updateIdleInhibit();
    kLog.debug("idle inhibitor added");
  }

  void Server::onIdleInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    IdleInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    delete watch;
    server->updateIdleInhibit();
    kLog.debug("idle inhibitor removed");
  }
  void Server::onPointerDestroy(wl_listener* listener, void* /*data*/) {
    PointerDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_pointers, [watch](const std::unique_ptr<PointerDevice>& pointer) {
      return pointer.get() == watch;
    });
  }

  void Server::onRequestActivate(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_requestActivate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    wlr_xdg_activation_token_v1* token = event->token;
    const char* tokenName = token != nullptr ? wlr_xdg_activation_token_v1_get_name(token) : nullptr;
    const auto* watch = token != nullptr ? static_cast<ActivationTokenWatch*>(token->data) : nullptr;
    const auto age = watch != nullptr
        ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watch->createdAt)
              .count()
        : -1;
    View* source = token != nullptr ? viewForSurface(*self, token->surface) : nullptr;
    View* target = viewForSurface(*self, event->surface);
    Workspace* targetWorkspace = target != nullptr ? target->workspace() : nullptr;
    const bool targetKeyboardFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    const bool targetPointerFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    kLog.debug(
        "xdg-activation activate token='{}' age_ms={} serial={} seat={} source_surface={} source_pid={} "
        "source_app_id='{}' app_id_hint='{}' target_surface={} target_pid={} target_app_id='{}' mapped={} "
        "visible={} keyboard_focused={} pointer_focused={} workspace='{}' workspace_active={} other_workspace={} "
        "locked={}",
        tokenName != nullptr ? tokenName : "<unknown>", age, token != nullptr ? token->serial : 0,
        static_cast<const void*>(token != nullptr ? token->seat : nullptr),
        static_cast<const void*>(token != nullptr ? token->surface : nullptr),
        surfaceClientPid(token != nullptr ? token->surface : nullptr),
        source != nullptr && source->toplevel()->app_id != nullptr ? source->toplevel()->app_id : "",
        token != nullptr && token->app_id != nullptr ? token->app_id : "", static_cast<const void*>(event->surface),
        surfaceClientPid(event->surface),
        target != nullptr && target->toplevel()->app_id != nullptr ? target->toplevel()->app_id : "",
        target != nullptr && target->mapped(), target != nullptr && target->onActiveWorkspace(), targetKeyboardFocused,
        targetPointerFocused, targetWorkspace != nullptr ? targetWorkspace->name() : "",
        targetWorkspace != nullptr && targetWorkspace->active(),
        targetWorkspace != nullptr && !targetWorkspace->active(), self->m_sessionLocked
    );
    if (self->m_sessionLocked) {
      return;
    }

    wlr_surface* root = wlr_surface_get_root_surface(event->surface);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
    if (toplevel == nullptr) {
      return;
    }

    for (const auto& entry : self->m_registry.all()) {
      if (entry->toplevel() == toplevel && entry->mapped()) {
        const bool focusOnActivate = entry->resolvedRules().focusOnActivate.value_or(config().general.focusOnActivate);
        const bool alreadyFocused = entry->workspace() != nullptr
            && entry->workspace()->active()
            && entry->workspace()->focusedView() == entry.get();
        kLog.debug(
            "xdg-activation policy target_app_id='{}' focus_on_activate={} already_focused={} action={}",
            entry->toplevel()->app_id != nullptr ? entry->toplevel()->app_id : "", focusOnActivate, alreadyFocused,
            alreadyFocused ? "none" : (focusOnActivate ? "focus" : "urgent")
        );
        if (alreadyFocused) {
          entry->setUrgent(false);
        } else if (focusOnActivate) {
          self->focusView(entry.get(), FocusReason::XdgActivation);
        } else {
          entry->setUrgent(true);
        }
        return;
      }
    }
  }

  void Server::onNewActivationToken(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newActivationToken);
    auto* token = static_cast<wlr_xdg_activation_token_v1*>(data);
    auto* watch = new ActivationTokenWatch{
        .createdAt = std::chrono::steady_clock::now(),
    };
    watch->destroy.notify = onActivationTokenDestroy;
    wl_signal_add(&token->events.destroy, &watch->destroy);
    token->data = watch;

    View* source = viewForSurface(*self, token->surface);
    const char* tokenName = wlr_xdg_activation_token_v1_get_name(token);
    kLog.debug(
        "xdg-activation token token='{}' serial={} seat={} source_surface={} source_pid={} source_app_id='{}' "
        "app_id_hint='{}' source_mapped={} source_visible={} source_keyboard_focused={} source_pointer_focused={}",
        tokenName != nullptr ? tokenName : "<unknown>", token->serial, static_cast<const void*>(token->seat),
        static_cast<const void*>(token->surface), surfaceClientPid(token->surface),
        source != nullptr && source->toplevel()->app_id != nullptr ? source->toplevel()->app_id : "",
        token->app_id != nullptr ? token->app_id : "", source != nullptr && source->mapped(),
        source != nullptr && source->onActiveWorkspace(),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface)
    );
  }

  void Server::onActivationTokenDestroy(wl_listener* listener, void* /*data*/) {
    ActivationTokenWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    wl_list_remove(&watch->destroy.link);
    delete watch;
  }

  void Server::onWorkspaceCommit(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_workspaceCommit);
    self->handleWorkspaceCommit(data);
  }

  void Server::onSetGamma(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_setGamma);
    auto* event = static_cast<wlr_gamma_control_manager_v1_set_gamma_event*>(data);
    if (Output* out = self->outputFromWlr(event->output)) {
      out->onGammaChanged(event->control);
    }
  }

  void Server::handleWorkspaceCommit(void* data) {
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link) {
      switch (request->type) {
      case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE: {
        WorkspaceGroup* group = workspaceGroupFromHandle(request->create_workspace.group);
        if (group != nullptr) {
          group->createWorkspace(request->create_workspace.name);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->activate.workspace)) {
          workspace->group()->activate(workspace);
          refocus(workspace->group()->output());
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->deactivate.workspace)) {
          workspace->group()->deactivate(workspace);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
        // Workspaces stay bound to their output group.
        break;
      case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
        break;
      }
    }
  }

  Workspace* Server::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<Workspace*>(handle->data);
  }

  WorkspaceGroup* Server::workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<WorkspaceGroup*>(handle->data);
  }

  void Server::updateIdleInhibit() {
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor;
    wl_list_for_each(inhibitor, &m_idleInhibitManager->inhibitors, link) {
      wlr_surface* root = wlr_surface_get_root_surface(inhibitor->surface);
      if (root == nullptr) {
        continue;
      }

      if (m_sessionLocked) {
        wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(root);
        inhibited =
            lockSurface != nullptr && root->mapped && lockSurface->output != nullptr && lockSurface->output->enabled;
      } else if (View* view = View::fromSurface(root)) {
        int x = 0;
        int y = 0;
        inhibited = view->mapped() && wlr_scene_node_coords(&view->sceneTree()->node, &x, &y);
      } else if (wlr_layer_surface_v1* wlrLayer = wlr_layer_surface_v1_try_from_wlr_surface(root)) {
        auto* layer = static_cast<LayerSurface*>(wlrLayer->data);
        Output* output = layer != nullptr ? layer->output() : nullptr;
        int x = 0;
        int y = 0;
        inhibited = layer != nullptr
            && layer->mapped()
            && output != nullptr
            && output->wlr()->enabled
            && wlr_scene_node_coords(&layer->scene()->tree->node, &x, &y);
      }

      if (inhibited) {
        break;
      }
    }
    wlr_idle_notifier_v1_set_inhibited(m_idleNotifier, inhibited);
  }

  void Server::notifyIdleActivity() { wlr_idle_notifier_v1_notify_activity(m_idleNotifier, m_seat->wlr()); }

  void Server::notifyInputActivity() {
    wakeDpmsOutputs();
    notifyIdleActivity();
  }

  void Server::wakeDpmsOutputs() {
    const bool anyPowered = std::ranges::any_of(m_outputs, [](const std::unique_ptr<Output>& output) {
      return output->configuredEnabled() && !output->dpmsOff();
    });
    if (anyPowered) {
      return;
    }
    for (const auto& output : m_outputs) {
      if (output->dpmsOff()) {
        (void)output->setPowered(true);
      }
    }
  }

  void Server::beginSessionLock(wlr_session_lock_v1* lock) {
    if (m_sessionLock != nullptr) {
      kLog.info("denying session lock; one is already active");
      wlr_session_lock_v1_destroy(lock);
      return;
    }

    cancelModifierTap();
    m_sessionLocked = true;
    m_overview->forceClose();
    if (m_cheatsheet != nullptr) {
      m_cheatsheet->hide();
    }
    if (m_quitConfirm != nullptr) {
      m_quitConfirm->hide();
    }
    m_cursor->resetMode();
    m_cursor->clearConstraint();
    clearNormalFocus();
    updateIdleInhibit();
    updateLockBlank();
    setLockBlankEnabled(true);
    raiseLockTree();
    m_sessionLock = std::make_unique<SessionLock>(*this, lock);
  }

  void Server::unlockSession() {
    m_sessionLocked = false;
    updateIdleInhibit();
    wlr_scene_node_set_enabled(&m_lockBlank->node, false);
    if (View* recent = m_registry.mostRecent()) {
      focusView(recent);
    }
  }

  void Server::removeSessionLock(SessionLock* lock) {
    if (m_sessionLock.get() != lock) {
      return;
    }
    m_sessionLock.reset();
    if (!m_sessionLocked) {
      wlr_scene_node_set_enabled(&m_lockTree->node, false);
    }
  }

  void Server::setLockBlankEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_lockTree->node, enabled);
    wlr_scene_node_set_enabled(&m_lockBlank->node, enabled);
    if (enabled) {
      raiseLockTree();
    }
  }

  // Security boundary: never defer this through Dirty. One stale frame can
  // expose content that the lock exists to hide.
  void Server::updateLockBlank() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_rect_set_color(m_lockBlank, config().appearance.backdropColor.data());
    wlr_scene_rect_set_size(m_lockBlank, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_lockBlank->node, layoutBox.x, layoutBox.y);
  }

  void Server::updateBackdrop() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_rect_set_color(m_backdrop, config().appearance.backdropColor.data());
    wlr_scene_rect_set_size(m_backdrop, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_backdrop->node, layoutBox.x, layoutBox.y);
    for (const auto& output : m_outputs) {
      output->markBlurBackgroundDirty();
    }
  }

  void Server::raiseLockTree() { wlr_scene_node_raise_to_top(&m_lockTree->node); }

  void Server::addOutput(wlr_output* output) {
    if (!m_pendingOutputName.empty()) {
      // Before the Output exists: adding it to the layout advertises the name to clients, and it cannot change after.
      wlr_output_set_name(output, m_pendingOutputName.c_str());
    }
    m_outputs.push_back(std::make_unique<Output>(*this, output));
    scheduleDisplacedViewRestore();
    markDirty(Dirty::Backdrop | Dirty::Banner | Dirty::Cheatsheet | Dirty::QuitConfirm);
    if (m_sessionLocked) {
      updateLockBlank();
      raiseLockTree();
    }
    updateOutputManagerConfig();
    refreshSurfaceScales();
  }

  void Server::addKeyboard(wlr_input_device* device) {
    m_keyboards.push_back(std::make_unique<Keyboard>(*this, device));
  }

  bool Server::cycleKeyboardLayout() {
    bool switched = false;
    for (const auto& keyboard : m_keyboards) {
      switched = keyboard->cycleLayout() || switched;
    }
    return switched;
  }

  std::optional<Server::KeyboardLayoutState> Server::keyboardLayoutState() const {
    for (const auto& keyboard : m_keyboards) {
      wlr_keyboard* wlrKeyboard = keyboard->wlr();
      if (keyboard->virtualDevice() || wlrKeyboard->keymap == nullptr || wlrKeyboard->xkb_state == nullptr) {
        continue;
      }
      KeyboardLayoutState state;
      const xkb_layout_index_t count = xkb_keymap_num_layouts(wlrKeyboard->keymap);
      state.names.reserve(count);
      for (xkb_layout_index_t i = 0; i < count; ++i) {
        const char* name = xkb_keymap_layout_get_name(wlrKeyboard->keymap, i);
        state.names.emplace_back(name != nullptr ? name : "");
      }
      state.currentIndex = xkb_state_serialize_layout(wlrKeyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
      return state;
    }
    return std::nullopt;
  }

  void Server::notifyKeyboardLayoutIpc() {
    if (m_ipc != nullptr) {
      m_ipc->notifyKeyboardLayoutChanged();
    }
  }

  void Server::notifyOverviewChanged() {
    if (m_ipc != nullptr) {
      m_ipc->notifyOverviewChanged();
    }
  }

  void Server::scheduleIpcWindowsEvent() {
    if (m_ipc == nullptr || m_ipcWindowsIdle != nullptr) {
      return;
    }
    m_ipcWindowsIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onIpcWindowsIdle, this);
    if (m_ipcWindowsIdle == nullptr) {
      kLog.error("failed to register IPC windows idle source");
    }
  }

  void Server::onIpcWindowsIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_ipcWindowsIdle = nullptr;
    if (server->m_ipc != nullptr) {
      server->m_ipc->notifyWindowsChanged();
    }
  }

  void Server::addPointer(wlr_input_device* device) {
    auto pointer = std::make_unique<PointerDevice>();
    pointer->server = this;
    pointer->device = device;
    pointer->destroy.notify = onPointerDestroy;
    wl_signal_add(&device->events.destroy, &pointer->destroy);
    applyPointerConfig(device);
    m_cursor->attachInputDevice(device);
    m_pointers.push_back(std::move(pointer));
  }

  void Server::addTouch(wlr_input_device* device) {
    auto touch = std::make_unique<TouchDevice>();
    touch->server = this;
    touch->device = device;
    touch->destroy.notify = onTouchDestroy;
    wl_signal_add(&device->events.destroy, &touch->destroy);
    m_cursor->attachInputDevice(device);
    m_touchDevices.push_back(std::move(touch));
    kLog.info("input: added touch device '{}'", deviceName(device));
  }

  void Server::onTouchDestroy(wl_listener* listener, void* /*data*/) {
    TouchDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_touchDevices, [watch](const std::unique_ptr<TouchDevice>& entry) {
      return entry.get() == watch;
    });
    server->updateSeatCapabilities();
  }

  void Server::addTablet(wlr_input_device* device) {
    auto tablet = std::make_unique<TabletDevice>();
    tablet->server = this;
    tablet->device = device;
    tablet->v2 = wlr_tablet_create(m_tabletManager, m_seat->wlr(), device);
    tablet->destroy.notify = onTabletDestroy;
    wl_signal_add(&device->events.destroy, &tablet->destroy);
    m_cursor->attachInputDevice(device);
    applyTabletConfig(*tablet);
    m_tabletDevices.push_back(std::move(tablet));
    pairTabletPads();
    kLog.info("input: added tablet '{}'", deviceName(device));
  }

  void Server::applyTabletConfig(TabletDevice& tablet) {
    if (wlr_input_device_is_libinput(tablet.device) == 0) {
      kLog.debug("input: tablet '{}' is not a libinput device; tablet settings skipped", deviceName(tablet.device));
      return;
    }
    libinput_device* libinputDevice = wlr_libinput_get_device_handle(tablet.device);
    if (libinputDevice == nullptr) {
      return;
    }
    const Config::Input::Tablet& cfg = config().input.tablet;
    if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
      libinput_device_config_send_events_set_mode(
          libinputDevice, cfg.enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
      );
    } else if (!cfg.enabled) {
      kLog.warn("input: '{}' cannot be disabled", deviceName(tablet.device));
    }
    if (libinput_device_config_left_handed_is_available(libinputDevice) != 0
        && libinput_device_config_left_handed_set(libinputDevice, cfg.leftHanded) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
      kLog.warn("input: failed to apply input.tablet.left_handed to '{}'", deviceName(tablet.device));
    }
    if (libinput_device_config_calibration_has_matrix(libinputDevice) != 0) {
      if (cfg.calibrationMatrix.has_value()) {
        libinput_device_config_calibration_set_matrix(libinputDevice, cfg.calibrationMatrix->data());
      } else {
        // Removing the key on reload restores the device default.
        float matrix[6] = {};
        libinput_device_config_calibration_get_default_matrix(libinputDevice, matrix);
        libinput_device_config_calibration_set_matrix(libinputDevice, matrix);
      }
    }
    remapTablets();
  }

  void Server::addTabletPad(wlr_input_device* device) {
    auto pad = std::make_unique<TabletPadDevice>();
    pad->server = this;
    pad->device = device;
    pad->v2 = wlr_tablet_pad_create(m_tabletManager, m_seat->wlr(), device);
    wlr_tablet_pad* wlrPad = wlr_tablet_pad_from_input_device(device);
    pad->destroy.notify = onTabletPadDestroy;
    wl_signal_add(&device->events.destroy, &pad->destroy);
    pad->button.notify = onTabletPadButton;
    wl_signal_add(&wlrPad->events.button, &pad->button);
    pad->ring.notify = onTabletPadRing;
    wl_signal_add(&wlrPad->events.ring, &pad->ring);
    pad->strip.notify = onTabletPadStrip;
    wl_signal_add(&wlrPad->events.strip, &pad->strip);
    applyTabletPadConfig(*pad);
    m_tabletPads.push_back(std::move(pad));
    pairTabletPads();
    // Deliver an initial enter to the keyboard-focused surface so a client
    // already holding focus gets the pad without a focus change.
    if (wlr_surface* focused = m_seat->wlr()->keyboard_state.focused_surface;
        focused != nullptr && m_tabletPads.back()->tablet != nullptr) {
      TabletPadDevice* entry = m_tabletPads.back().get();
      entry->enteredSurface = focused;
      wlr_tablet_v2_tablet_pad_notify_enter(entry->v2, entry->tablet->v2, focused);
    }
    kLog.info("input: added tablet pad '{}'", deviceName(device));
  }

  void Server::applyTabletPadConfig(TabletPadDevice& pad) {
    if (wlr_input_device_is_libinput(pad.device) == 0) {
      return;
    }
    libinput_device* libinputDevice = wlr_libinput_get_device_handle(pad.device);
    if (libinputDevice == nullptr) {
      return;
    }
    const bool enabled = config().input.tablet.enabled;
    if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
      libinput_device_config_send_events_set_mode(
          libinputDevice, enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
      );
    } else if (!enabled) {
      kLog.warn("input: '{}' cannot be disabled", deviceName(pad.device));
    }
  }

  void Server::pairTabletPads() {
    for (const auto& pad : m_tabletPads) {
      TabletDevice* match = nullptr;
      for (const auto& candidate : m_tabletDevices) {
        if (wlr_input_device_is_libinput(pad->device) != 0 && wlr_input_device_is_libinput(candidate->device) != 0) {
          libinput_device* padHandle = wlr_libinput_get_device_handle(pad->device);
          libinput_device* tabletHandle = wlr_libinput_get_device_handle(candidate->device);
          if (padHandle != nullptr
              && tabletHandle != nullptr
              && libinput_device_get_device_group(padHandle) == libinput_device_get_device_group(tabletHandle)) {
            match = candidate.get();
            break;
          }
        }
      }
      if (match == nullptr && m_tabletDevices.size() == 1) {
        match = m_tabletDevices.front().get();
      }
      pad->tablet = match;
    }
  }

  void Server::remapTablets() {
    const Config::Input::Tablet& cfg = config().input.tablet;
    for (const auto& tablet : m_tabletDevices) {
      wlr_box region{};
      wlr_output* output = nullptr;
      if (cfg.mapToFocusedWindow) {
        if (View* view = View::fromSurface(m_seat->wlr()->keyboard_state.focused_surface)) {
          const wlr_box geo = view->toplevel()->base->geometry;
          if (geo.width > 0 && geo.height > 0) {
            region = {view->layoutTargetX(), view->layoutTargetY(), geo.width, geo.height};
          }
        }
      }
      if (wlr_box_empty(&region) && (cfg.mapToFocusedWindow || cfg.mapToFocusedOutput)) {
        if (Output* out = focusedOutput()) {
          output = out->wlr();
        }
      }
      if (wlr_box_empty(&region) && output == nullptr && !cfg.mapToOutput.empty()) {
        if (Output* out = outputFromName(cfg.mapToOutput)) {
          output = out->wlr();
        }
      }
      wlr_cursor_map_input_to_region(m_cursor->wlr(), tablet->device, &region);
      wlr_cursor_map_input_to_output(m_cursor->wlr(), tablet->device, output);
    }
  }

  wlr_tablet_v2_tablet* Server::tabletV2FromWlr(const wlr_tablet* tablet) const {
    if (tablet == nullptr) {
      return nullptr;
    }
    for (const auto& entry : m_tabletDevices) {
      if (&tablet->base == entry->device) {
        return entry->v2;
      }
    }
    return nullptr;
  }

  void Server::onTabletDestroy(wl_listener* listener, void* /*data*/) {
    TabletDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    for (const auto& pad : server->m_tabletPads) {
      if (pad->tablet != watch) {
        continue;
      }
      if (pad->enteredSurface != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, pad->enteredSurface);
        pad->enteredSurface = nullptr;
      }
      pad->tablet = nullptr;
    }
    std::erase_if(server->m_tabletDevices, [watch](const std::unique_ptr<TabletDevice>& entry) {
      return entry.get() == watch;
    });
    server->pairTabletPads();
  }

  void Server::onTabletPadButton(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, button);
    auto* event = static_cast<wlr_tablet_pad_button_event*>(data);
    if (event->state == WLR_BUTTON_PRESSED) {
      watch->server->notifyInputActivity();
    } else {
      watch->server->notifyIdleActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_button(
        watch->v2, event->button, event->time_msec,
        event->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                           : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED
    );
  }

  void Server::onTabletPadRing(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, ring);
    auto* event = static_cast<wlr_tablet_pad_ring_event*>(data);
    if (event->position < 0) {
      watch->server->notifyIdleActivity();
    } else {
      watch->server->notifyInputActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_ring(
        watch->v2, event->ring, event->position, event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, event->time_msec
    );
  }

  void Server::onTabletPadStrip(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, strip);
    auto* event = static_cast<wlr_tablet_pad_strip_event*>(data);
    if (event->position < 0) {
      watch->server->notifyIdleActivity();
    } else {
      watch->server->notifyInputActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_strip(
        watch->v2, event->strip, event->position, event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, event->time_msec
    );
  }

  void Server::onPadKeyboardFocusChange(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_padKeyboardFocusChange);
    auto* event = static_cast<wlr_seat_keyboard_focus_change_event*>(data);
    for (const auto& pad : self->m_tabletPads) {
      if (pad->tablet == nullptr) {
        continue;
      }
      if (pad->enteredSurface != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, pad->enteredSurface);
        pad->enteredSurface = nullptr;
      }
      if (event->new_surface != nullptr) {
        pad->enteredSurface = event->new_surface;
        wlr_tablet_v2_tablet_pad_notify_enter(pad->v2, pad->tablet->v2, event->new_surface);
      }
    }
  }

  void Server::onTabletPadDestroy(wl_listener* listener, void* /*data*/) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->button.link);
    wl_list_remove(&watch->ring.link);
    wl_list_remove(&watch->strip.link);
    if (watch->enteredSurface != nullptr) {
      wlr_tablet_v2_tablet_pad_notify_leave(watch->v2, watch->enteredSurface);
    }
    std::erase_if(server->m_tabletPads, [watch](const std::unique_ptr<TabletPadDevice>& entry) {
      return entry.get() == watch;
    });
    server->pairTabletPads();
  }

  void Server::removeOutput(Output* output) {
    m_overview->onOutputRemoved(output);
    m_gestures->cancelForOutput(output);
    if (!m_cursor->isPassthrough()) {
      m_cursor->resetMode();
    }
    if (m_insertHint != nullptr && m_insertHint->output() == output) {
      m_insertHint->hideImmediate();
    }
    // Dropping the snapshot unregisters it and destroys its scene tree.
    std::erase_if(m_closeSnapshots, [this, output](const std::unique_ptr<CloseSnapshot>& snap) {
      if (!snap->animatesOn(output)) {
        return false;
      }
      unregisterAnimatable(snap.get());
      return true;
    });
    // wlroots 0.20 does not track output lifetime for layer surfaces, so their wlr_output pointer would dangle once the
    // output is freed. Destroy them now while the wlr_output is still valid.
    {
      wlr_output* dying = output->wlr();
      std::vector<wlr_layer_surface_v1*> toClose;
      for (const auto& ls : m_layerSurfaces) {
        if (ls->layerSurface() != nullptr && ls->layerSurface()->output == dying) {
          toClose.push_back(ls->layerSurface());
        }
      }
      for (wlr_layer_surface_v1* ls : toClose) {
        wlr_layer_surface_v1_destroy(ls);
      }
    }

    Output* fallback = nullptr;
    for (const auto& entry : m_outputs) {
      // Prefer a live monitor: rehoming windows onto a disabled one would
      // strand them off-screen.
      if (entry.get() != output && entry->wlr()->enabled) {
        fallback = entry.get();
        break;
      }
    }

    reassignOutputViews(output, fallback);
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->releaseOutput(output);
    }

    std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) { return entry.get() == output; });
    markDirty(Dirty::Banner | Dirty::Cheatsheet | Dirty::QuitConfirm);
    if (m_sessionLocked) {
      updateLockBlank();
    }
    updateOutputManagerConfig();
    // Scratchpad and pinned views rehome without going through setWorkspace.
    refreshSurfaceScales();
  }

  void Server::reassignOutputViews(Output* source, Output* destination) {
    if (source == nullptr || source == destination) {
      return;
    }
    WorkspaceGroup* sourceGroup = source->workspaceGroup();
    Workspace* targetWorkspace = destination != nullptr && destination->workspaceGroup() != nullptr
        ? destination->workspaceGroup()->active()
        : nullptr;
    const char* sourceName = source->wlr()->name;
    if (sourceGroup != nullptr && sourceGroup->active() != nullptr && sourceName != nullptr) {
      const auto existing = std::ranges::find_if(m_displacedWorkspaceSelections, [sourceName](const auto& selection) {
        return selection.outputName == sourceName;
      });
      if (existing == m_displacedWorkspaceSelections.end()) {
        m_displacedWorkspaceSelections.push_back({
            .outputName = sourceName,
            .workspaceName = sourceGroup->active()->name(),
            .workspaceIndex = sourceGroup->active()->index(),
        });
      }
    }
    const auto rememberWorkspace = [this](Workspace* workspace, Output* output, bool tiledOnly) {
      if (workspace == nullptr || output == nullptr || output->wlr()->name == nullptr) {
        return;
      }
      const wlr_box outputBox = output->layoutBox();
      const LayoutCapture capture = workspace->scrollingLayout() != nullptr
          ? workspace->scrollingLayout()->captureStateForViewport(workspace->scrollViewportExtent())
          : workspace->layout().captureState();
      for (const auto& view : m_registry.all()) {
        if (view->workspace() != workspace || view->displacedHome()) {
          continue;
        }
        const auto member = std::ranges::find_if(capture.members, [view = view.get()](const LayoutMember& entry) {
          return entry.view == view;
        });
        if (tiledOnly && member == capture.members.end()) {
          continue;
        }
        if (view->floating()) {
          view->rememberFloatingPosition();
        }
        View::DisplacedHome home{
            .outputName = output->wlr()->name,
            .workspaceName = workspace->name(),
            .layoutSnapshot = nullptr,
            .layoutMember = 0,
            .layoutModeOverride = workspace->layoutModeOverride(),
            .floatingOutputPosition = std::nullopt,
            .configGeneration = configStore().generation(),
            .layoutProtectionOnly = tiledOnly,
        };
        if (view->floating() && outputBox.width > 0 && outputBox.height > 0) {
          home.floatingOutputPosition = {{
              static_cast<double>(view->sceneTree()->node.x - outputBox.x) / outputBox.width,
              static_cast<double>(view->sceneTree()->node.y - outputBox.y) / outputBox.height,
          }};
        }
        if (member != capture.members.end()) {
          home.layoutSnapshot = capture.snapshot;
          home.layoutMember = member->id;
        }
        view->markDisplaced(std::move(home));
      }
    };

    if (sourceGroup != nullptr) {
      std::vector<View*> leaving;
      for (const auto& view : m_registry.all()) {
        Workspace* workspace = view->workspace();
        if (workspace == nullptr || workspace->group() != sourceGroup) {
          continue;
        }
        leaving.push_back(view.get());
      }

      // Capture every source workspace before the first move. Moving a view can
      // collapse and renumber a dynamic workspace group.
      for (size_t index = 0; index < sourceGroup->workspaceCount(); ++index) {
        rememberWorkspace(sourceGroup->workspaceAt(index), source, false);
      }
      // Refugees alter the destination's tiled structure. Preserve its native
      // members before the first refugee joins, so a later loss of that output
      // does not snapshot an already mixed layout.
      const bool contaminatesDestination = !leaving.empty();
      if (contaminatesDestination && targetWorkspace != nullptr) {
        rememberWorkspace(targetWorkspace, destination, true);
      }

      for (View* view : leaving) {
        if (view->displacedHome() && view->displacedHome()->layoutProtectionOnly) {
          View::DisplacedHome home = *view->displacedHome();
          home.layoutProtectionOnly = false;
          view->markDisplaced(std::move(home));
        }
        const bool floating = view->floating();
        view->setWorkspace(targetWorkspace);
        if (floating && targetWorkspace != nullptr) {
          // This output is only a refuge while the recorded home is absent. Present the saved geometry here without
          // replacing it, since another output can disappear before this animation finishes during VT deactivation.
          view->restoreFloatingPosition(false);
        }
      }
    }
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->moveOutput(source, destination);
    }
  }

  // Deferred to idle: outputs come back one at a time, and a returning one has no workspace group until addOutput is
  // done with it.
  void Server::scheduleDisplacedViewRestore() {
    if (m_displacedRestoreIdle != nullptr) {
      return;
    }
    m_displacedRestoreIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onDisplacedRestoreIdle, this);
    if (m_displacedRestoreIdle == nullptr) {
      kLog.error("failed to register displaced window restore idle source");
    }
  }

  void Server::onDisplacedRestoreIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_displacedRestoreIdle = nullptr;
    server->restoreDisplacedViews();
  }

  void Server::restoreDisplacedViews() {
    Output* fallback = outputFromWlr(preferredOutput());
    if (fallback == nullptr) {
      return;
    }
    std::vector<View*> displaced;
    for (const auto& entry : m_registry.all()) {
      if (entry->displacedHome()) {
        displaced.push_back(entry.get());
      }
    }
    const auto homeIsAvailable = [this](const View* view) {
      const View::DisplacedHome& home = *view->displacedHome();
      Output* output = outputFromName(home.outputName);
      return output != nullptr && output->workspaceGroup() != nullptr;
    };
    // Available homes go first, before refugees can occupy their layouts.
    // Within an output, resolving and populating numbered workspaces in order
    // lets a dynamic group create workspace N+1 before it is requested.
    std::ranges::sort(displaced, [homeIsAvailable](const View* lhs, const View* rhs) {
      const View::DisplacedHome& left = *lhs->displacedHome();
      const View::DisplacedHome& right = *rhs->displacedHome();
      const bool leftAvailable = homeIsAvailable(lhs);
      const bool rightAvailable = homeIsAvailable(rhs);
      if (leftAvailable != rightAvailable) {
        return leftAvailable;
      }
      if (leftAvailable && left.layoutProtectionOnly != right.layoutProtectionOnly) {
        return !left.layoutProtectionOnly;
      }
      if (left.outputName != right.outputName) {
        return left.outputName < right.outputName;
      }
      const size_t leftIndex = workspaceOrder(left.workspaceName);
      const size_t rightIndex = workspaceOrder(right.workspaceName);
      return leftIndex != rightIndex ? leftIndex < rightIndex : left.workspaceName < right.workspaceName;
    });

    struct RestoredViewport {
      std::shared_ptr<const LayoutSnapshot> snapshot;
      Workspace* workspace = nullptr;
      bool geometryUnchanged = false;
    };
    std::vector<RestoredViewport> restoredViewports;
    size_t restored = 0;

    for (size_t first = 0; first < displaced.size();) {
      const View::DisplacedHome groupHome = *displaced[first]->displacedHome();
      size_t last = first + 1;
      while (last < displaced.size()) {
        const View::DisplacedHome& candidate = *displaced[last]->displacedHome();
        if (candidate.outputName != groupHome.outputName
            || candidate.workspaceName != groupHome.workspaceName
            || candidate.layoutProtectionOnly != groupHome.layoutProtectionOnly) {
          break;
        }
        ++last;
      }

      Output* target = outputFromName(groupHome.outputName);
      WorkspaceGroup* targetGroup = target != nullptr ? target->workspaceGroup() : nullptr;
      if (targetGroup == nullptr) {
        WorkspaceGroup* fallbackGroup = fallback->workspaceGroup();
        Workspace* refuge = fallbackGroup != nullptr ? fallbackGroup->active() : nullptr;
        if (refuge != nullptr) {
          for (size_t index = first; index < last; ++index) {
            View* view = displaced[index];
            if (view->workspace() != nullptr) {
              continue;
            }
            const bool floating = view->floating();
            view->setWorkspace(refuge);
            if (floating) {
              view->restoreFloatingPosition(false);
            }
            ++restored;
          }
        }
        first = last;
        continue;
      }

      Workspace* workspace = nullptr;
      if (targetGroup->dynamic()) {
        const size_t desired = workspaceOrder(groupHome.workspaceName);
        if (desired != std::numeric_limits<size_t>::max() && desired >= 1) {
          // A recreated dynamic group starts with workspace 1. Materialize an
          // empty active workspace before a surviving workspace 2 is restored,
          // then ordinary reconciliation can retain both.
          while (targetGroup->workspaceCount() < desired) {
            if (targetGroup->insertDynamicWorkspace(targetGroup->workspaceCount()) == nullptr) {
              break;
            }
          }
          workspace = targetGroup->workspaceNamed(groupHome.workspaceName);
        }
      } else {
        workspace = targetGroup->workspaceForSelector(groupHome.workspaceName);
      }
      const bool selectorMatched = workspace != nullptr;
      if (workspace == nullptr) {
        workspace = targetGroup->active();
      }
      if (workspace == nullptr) {
        first = last;
        continue;
      }

      struct SnapshotCandidate {
        std::shared_ptr<const LayoutSnapshot> snapshot;
        std::vector<View*> views;
        View::DisplacedHome representative;
      };
      std::vector<SnapshotCandidate> candidates;
      if (selectorMatched) {
        for (size_t index = first; index < last; ++index) {
          View* view = displaced[index];
          const View::DisplacedHome& home = *view->displacedHome();
          if (!view->tiled() || home.layoutSnapshot == nullptr) {
            continue;
          }
          auto candidate = std::ranges::find_if(candidates, [&home](const SnapshotCandidate& entry) {
            return entry.snapshot.get() == home.layoutSnapshot.get();
          });
          if (candidate == candidates.end()) {
            candidates.push_back({
                .snapshot = home.layoutSnapshot,
                .views = {},
                .representative = home,
            });
            candidate = std::prev(candidates.end());
          }
          candidate->views.push_back(view);
        }
      }

      SnapshotCandidate* exact = nullptr;
      for (SnapshotCandidate& candidate : candidates) {
        if (exact == nullptr
            || candidate.views.size() > exact->views.size()
            || (candidate.views.size() == exact->views.size()
                && candidate.snapshot->memberCount() > exact->snapshot->memberCount())) {
          exact = &candidate;
        }
      }
      if (exact != nullptr
          && workspace->layoutMode() != exact->snapshot->mode()
          && exact->representative.layoutModeOverride.has_value()
          && *exact->representative.layoutModeOverride == exact->snapshot->mode()
          && exact->representative.configGeneration == configStore().generation()) {
        workspace->overrideLayoutMode(*exact->representative.layoutModeOverride);
      }
      if (exact != nullptr && workspace->layoutMode() != exact->snapshot->mode()) {
        exact = nullptr;
      }
      const bool snapshotMemberIsUnmapped = exact != nullptr
          && std::ranges::any_of(displaced.begin() + static_cast<std::ptrdiff_t>(first),
                                 displaced.begin() + static_cast<std::ptrdiff_t>(last), [exact](const View* view) {
                                   const auto& home = view->displacedHome();
                                   return !view->mapped()
                                       && home.has_value()
                                       && home->layoutSnapshot.get() == exact->snapshot.get();
                                 });

      std::vector<View*> exactViews;
      if (exact != nullptr) {
        for (View* view : exact->views) {
          view->setWorkspace(workspace, false);
          if (view->workspace() == workspace && view->mapped() && view->tiled()) {
            exactViews.push_back(view);
          }
        }
      }

      if (exact != nullptr && !exactViews.empty()) {
        std::vector<View*> previousOrder;
        for (const Column& column : workspace->layout().columns()) {
          for (View* view : column.views) {
            if (view != nullptr && std::ranges::find(previousOrder, view) == previousOrder.end()) {
              previousOrder.push_back(view);
            }
          }
        }
        for (View* view : previousOrder) {
          if (view->workspace() == workspace && workspace->layout().columnOf(view) >= 0) {
            workspace->layoutDetach(view, false);
          }
        }

        std::vector<LayoutMember> members;
        members.reserve(exactViews.size());
        for (View* view : exactViews) {
          const View::DisplacedHome& home = *view->displacedHome();
          members.push_back({.id = home.layoutMember, .view = view});
        }
        const bool restoredState = workspace->layout().restoreState(*exact->snapshot, members);
        for (View* view : previousOrder) {
          if (view->workspace() == workspace && view->mapped() && view->tiled()) {
            workspace->layoutAttach(view);
          }
        }
        if (!restoredState) {
          for (View* view : exactViews) {
            if (view->workspace() == workspace && view->mapped() && view->tiled()) {
              workspace->layoutAttach(view);
            }
          }
        } else {
          if (workspace->scrollingLayout() != nullptr) {
            restoredViewports.push_back({
                .snapshot = exact->snapshot,
                .workspace = workspace,
                .geometryUnchanged = exact->representative.configGeneration == configStore().generation(),
            });
          }
          workspace->markArrange(false);
        }
        restored += exactViews.size();
      }

      const bool foreignRefugeesRemain =
          std::ranges::any_of(m_registry.all(), [&groupHome, workspace](const auto& entry) {
            const auto& home = entry->displacedHome();
            return entry->workspace() == workspace && home.has_value() && home->outputName != groupHome.outputName;
          });
      const bool retainLayoutProtection =
          exact != nullptr && exact->representative.layoutProtectionOnly && foreignRefugeesRemain;
      const auto retainHome = [exact, retainLayoutProtection, snapshotMemberIsUnmapped, selectorMatched,
                               workspace](const View* view) {
        const auto& home = view->displacedHome();
        if (!home) {
          return false;
        }
        return (retainLayoutProtection && home->layoutProtectionOnly)
            || (snapshotMemberIsUnmapped && home->layoutSnapshot.get() == exact->snapshot.get())
            || (!view->mapped()
                && selectorMatched
                && home->layoutSnapshot != nullptr
                && workspace->layoutMode() == home->layoutSnapshot->mode());
      };

      for (size_t index = first; index < last; ++index) {
        View* view = displaced[index];
        if (std::ranges::find(exactViews, view) != exactViews.end()) {
          if (!retainHome(view)) {
            view->clearDisplaced();
          }
          continue;
        }
        const View::DisplacedHome home = *view->displacedHome();
        const bool floating = view->floating();
        if (!retainHome(view)) {
          view->clearDisplaced();
        }
        const bool moved = view->workspace() != workspace;
        if (moved) {
          view->setWorkspace(workspace);
        }
        if (floating && home.floatingOutputPosition && target != nullptr) {
          const wlr_box outputBox = target->layoutBox();
          if (outputBox.width > 0 && outputBox.height > 0) {
            view->cancelPositionAnimation();
            view->setPosition(
                outputBox.x + static_cast<int>(std::lround((*home.floatingOutputPosition)[0] * outputBox.width)),
                outputBox.y + static_cast<int>(std::lround((*home.floatingOutputPosition)[1] * outputBox.height))
            );
          }
        } else if (floating && moved) {
          view->restoreFloatingPosition(true);
        }
        restored += moved ? 1 : 0;
      }

      first = last;
    }

    if (m_scratchpadManager != nullptr) {
      restored += m_scratchpadManager->restoreDisplaced(fallback);
    }

    // A fixed-size client can finish mapping while every output is absent. It never had a workspace, so it has no
    // displaced home for the restore pass above to find. Scratchpad entries are intentionally workspace-less; attach
    // every other mapped orphan once an output can own it.
    WorkspaceGroup* fallbackGroup = fallback->workspaceGroup();
    Workspace* refuge = fallbackGroup != nullptr ? fallbackGroup->active() : nullptr;
    if (refuge != nullptr) {
      for (const auto& entry : m_registry.all()) {
        View* view = entry.get();
        if (!view->mapped()
            || view->workspace() != nullptr
            || (m_scratchpadManager != nullptr && m_scratchpadManager->contains(view))) {
          continue;
        }
        const bool floating = view->floating();
        const bool positioned = view->m_positioned;
        const ResolvedWindowRule rule = view->resolvedRules();
        if (!view->attachToAvailableWorkspace(rule)) {
          continue;
        }
        if (floating) {
          if (positioned) {
            view->restoreFloatingPosition(false);
          } else {
            view->placeInUsableArea(rule.defaultPosition);
          }
          if (Workspace* workspace = view->workspace()) {
            workspace->syncViewPresentation(view);
          }
        }
        ++restored;
      }
    }

    bool workspaceSelectionRestored = false;
    for (auto selection = m_displacedWorkspaceSelections.begin(); selection != m_displacedWorkspaceSelections.end();) {
      Output* output = outputFromName(selection->outputName);
      WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
      if (group == nullptr) {
        ++selection;
        continue;
      }

      Workspace* workspace = group->workspaceNamed(selection->workspaceName);
      if (workspace == nullptr && group->dynamic()) {
        while (group->workspaceCount() <= selection->workspaceIndex) {
          if (group->insertDynamicWorkspace(group->workspaceCount()) == nullptr) {
            break;
          }
        }
        workspace = group->workspaceNamed(selection->workspaceName);
      }
      if (workspace == nullptr && group->workspaceCount() > 0) {
        workspace = group->workspaceAt(std::min(selection->workspaceIndex, group->workspaceCount() - 1));
      }
      if (workspace == nullptr) {
        ++selection;
        continue;
      }
      group->activate(workspace, false);
      selection = m_displacedWorkspaceSelections.erase(selection);
      workspaceSelectionRestored = true;
    }

    if (restored > 0) {
      kLog.info("restored {} displaced windows", restored);
      refreshSurfaceScales();
    }
    if (restored > 0 || workspaceSelectionRestored) {
      refocus();
    }

    // Refocus can reveal another scrolling lane. Reapply the snapshot anchor
    // synchronously while both the snapshot and its surviving views are alive.
    for (const RestoredViewport& restoredViewport : restoredViewports) {
      Workspace* workspace = restoredViewport.workspace;
      if (workspace == nullptr || workspace->layoutMode() != restoredViewport.snapshot->mode()) {
        continue;
      }
      if (ScrollingLayout* scrolling = workspace->scrollingLayout(); scrolling != nullptr) {
        scrolling->restoreSnapshotViewport(
            *restoredViewport.snapshot, workspace->scrollViewportExtent(), restoredViewport.geometryUnchanged
        );
      }
      workspace->markArrange(false);
    }
    if (restored > 0 || workspaceSelectionRestored) {
      scheduleIpcWindowsEvent();
    }
  }

  void Server::removeKeyboard(Keyboard* keyboard) {
    std::erase_if(m_keyboards, [keyboard](const std::unique_ptr<Keyboard>& entry) { return entry.get() == keyboard; });
    updateSeatCapabilities();
  }

  void Server::removeView(View* view) {
    if (view == nullptr) {
      return;
    }
    for (const auto& output : m_outputs) {
      output->forgetHdrView(view);
    }
    const bool hadKeyboardFocus = m_seat->wlr()->keyboard_state.focused_surface == view->toplevel()->base->surface;
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->remove(view);
    }
    View* replacement = nullptr;
    Output* output = nullptr;
    if (Workspace* workspace = view->workspace()) {
      if (workspace->group() != nullptr) {
        output = workspace->group()->output();
      }
      replacement = workspace->removeView(view);
      view->detachWorkspace();
    }
    const bool restoreAfterRemoval = view->displacedHome().has_value();
    m_registry.remove(view);
    if (restoreAfterRemoval) {
      scheduleDisplacedViewRestore();
    }
    if (hadKeyboardFocus) {
      wlr_seat_keyboard_notify_clear_focus(m_seat->wlr());
      if (replacement != nullptr) {
        focusView(replacement);
      } else {
        refocus(output);
      }
    }
  }

  void Server::removeLayerSurface(LayerSurface* layerSurface, wlr_output* output) {
    std::erase_if(m_layerSurfaces, [layerSurface](const std::unique_ptr<LayerSurface>& entry) {
      return entry.get() == layerSurface;
    });
    arrangeLayers(output);
  }

  void Server::onOutputManagerApply(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerApply);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), false);
  }

  void Server::onOutputManagerTest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerTest);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), true);
  }

  void Server::onOutputLayoutChange(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_outputLayoutChange);
    self->markDirty(Dirty::Backdrop);
    // A neighbour appearing, moving, or resizing changes where every output's content clip has to sit, and the clip is
    // refreshed from arrangeLayers.
    for (const auto& output : self->m_outputs) {
      output->markDirty(Dirty::LayerArrange);
    }
    self->updateOutputManagerConfig();
  }

  void Server::updateOutputManagerConfig() {
    if (m_outputManager == nullptr) {
      return;
    }
    wlr_output_configuration_v1* cfg = wlr_output_configuration_v1_create();
    for (const auto& output : m_outputs) {
      wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(cfg, output->wlr());
      if (wlr_output_layout_output* lo = wlr_output_layout_get(m_outputLayout, output->wlr())) {
        head->state.x = lo->x;
        head->state.y = lo->y;
      }
    }
    wlr_output_manager_v1_set_configuration(m_outputManager, cfg);
  }

  void Server::applyOutputManagerConfig(wlr_output_configuration_v1* config, bool testOnly) {
    // Reject disabling outputs: the protocol commit would bypass the layout and scene handling that
    // Output::applyOutputState does for the config `enabled` key, leaving the monitor off but still on the desktop.
    wlr_output_configuration_head_v1* head = nullptr;
    wl_list_for_each(head, &config->heads, link) {
      if (!head->state.enabled) {
        kLog.warn("output-management: disabling outputs is not supported, use the config `enabled` key");
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
      }
    }

    size_t statesLen = 0;
    wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config, &statesLen);
    if (states == nullptr) {
      wlr_output_configuration_v1_send_failed(config);
      wlr_output_configuration_v1_destroy(config);
      return;
    }

    bool ok = wlr_backend_test(m_backend, states, statesLen);
    if (ok && !testOnly) {
      ok = wlr_backend_commit(m_backend, states, statesLen);
    }

    if (ok && !testOnly) {
      // Apply layout positions and refresh affected outputs.
      wl_list_for_each(head, &config->heads, link) {
        wlr_output_layout_add(m_outputLayout, head->state.output, head->state.x, head->state.y);
        if (Output* out = outputFromWlr(head->state.output)) {
          out->handleExternalConfigChange();
        }
      }
      markDirty(Dirty::Banner | Dirty::Cheatsheet | Dirty::QuitConfirm);
      if (m_sessionLocked) {
        updateLockBlank();
      }
      refreshSurfaceScales();
    }

    free(states);
    if (ok) {
      wlr_output_configuration_v1_send_succeeded(config);
    } else {
      wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    if (ok && !testOnly) {
      updateOutputManagerConfig();
    }
  }

  void Server::onToplevelCaptureRequest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_toplevelCaptureRequest);
    auto* request = static_cast<wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*>(data);

    if (request->toplevel_handle == nullptr || request->toplevel_handle->data == nullptr) {
      return;
    }
    auto* view = static_cast<View*>(request->toplevel_handle->data);

    // Reuse a cached capture source when one already exists for this view.
    if (view->m_captureSource == nullptr) {
      wlr_scene_tree* captureTree = view->captureTree();
      if (captureTree == nullptr) {
        return;
      }
      view->m_captureSource = wlr_ext_image_capture_source_v1_create_with_scene_node(
          &captureTree->node, wl_display_get_event_loop(self->m_display), self->m_allocator, self->m_renderer
      );
      if (view->m_captureSource == nullptr) {
        return;
      }
      view->m_captureSourceDestroy.notify = View::onCaptureSourceDestroy;
      wl_signal_add(&view->m_captureSource->events.destroy, &view->m_captureSourceDestroy);
    }

    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, view->m_captureSource);
  }
} // namespace umbriel
