#include "server/server.h"

#include "config/config.h"
#include "config/config_diag.h"
#include "config/config_watcher.h"
#include "core/fdlimit.h"
#include "core/log.h"
#include "core/process.h"
#include "input/cursor.h"
#include "input/gestures.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "input/text_input.h"
#include "layer/layer_surface.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/color.h"
#include "scene/config_banner.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/ipc.h"
#include "server/wine_color_manager.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/supervisor.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace umbriel {

  namespace {

    constexpr Logger kLog("server");
    constexpr size_t kWaylandClientBufferSize = 1024 * 1024;
    // Security-context clients only receive reviewed, ordinary application
    // protocols. New globals stay unavailable until they are classified here.
    constexpr std::array<std::string_view, 28> kAllowedSecurityContextGlobals{
        "wl_shm",
        "wl_drm",
        "zwp_linux_dmabuf_v1",
        "wp_linux_drm_syncobj_manager_v1",
        "wl_compositor",
        "wl_subcompositor",
        "wl_data_device_manager",
        "zwp_primary_selection_device_manager_v1",
        "wp_viewporter",
        "wp_fractional_scale_manager_v1",
        "wp_presentation",
        "wp_tearing_control_manager_v1",
        "wp_content_type_manager_v1",
        "wl_output",
        "wp_color_manager_v1",
        "xdg_wm_base",
        "xdg_toplevel_tag_manager_v1",
        "zxdg_decoration_manager_v1",
        "org_kde_kwin_server_decoration_manager",
        "zwp_relative_pointer_manager_v1",
        "zwp_pointer_constraints_v1",
        "zwp_pointer_gestures_v1",
        "zwp_tablet_manager_v2",
        "zwp_idle_inhibit_manager_v1",
        "xdg_activation_v1",
        "wl_seat",
        "wp_cursor_shape_manager_v1",
        "zwp_text_input_manager_v3",
    };

    bool isAllowedSecurityContextGlobal(std::string_view interface) {
      return std::ranges::find(kAllowedSecurityContextGlobals, interface) != kAllowedSecurityContextGlobals.end();
    }

    bool filterGlobal(const wl_client* client, const wl_global* global, void* data) {
      auto* server = static_cast<Server*>(data);
      const wl_interface* interface = wl_global_get_interface(global);
      if (interface == nullptr) {
        return true;
      }
      const std::string_view interfaceName(interface->name);
      if (server->clientHasSecurityContext(client) && !isAllowedSecurityContextGlobal(interfaceName)) {
        return false;
      }
      if (interfaceName == "zwp_primary_selection_device_manager_v1") {
        return config().input.middleClickPaste;
      }
      if (interfaceName == "wp_color_manager_v1") {
        const bool wine = WineColorManager::clientNeedsCompatibility(client);
        if (server->wineColorManager() != nullptr && global == server->wineColorManager()->global()) {
          return wine;
        }
        return !wine;
      }
      return true;
    }

    std::string shellQuote(std::string_view value) {
      std::string quoted("'");
      for (const char character : value) {
        if (character == '\'') {
          quoted += "'\\''";
        } else {
          quoted += character;
        }
      }
      quoted += '\'';
      return quoted;
    }

    void applyConfiguredEnvironment() {
      for (const auto& [name, value] : config().environment.variables) {
        if (setenv(name.c_str(), value.c_str(), 1) != 0) {
          kLog.warn("failed to export environment variable {}", name);
        }
      }
    }

    std::string sessionEnvironmentCommand() {
      std::string configuredAssignments;
      for (const auto& [name, value] : config().environment.variables) {
        configuredAssignments += ' ';
        configuredAssignments += shellQuote(name + "=" + value);
      }

      const std::string publishConfigured =
          configuredAssignments.empty() ? "" : " && systemctl --user set-environment" + configuredAssignments;
      std::string command =
          "variables='WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE "
          "UMBRIEL_SOCKET'; systemd_ready=false; "
          "if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then "
          "if systemctl --user import-environment $variables";
      command += publishConfigured;
      command += "; then systemd_ready=true; else "
                 "echo 'umbriel: failed to publish the session environment' >&2; fi; fi; "
                 "if command -v dbus-update-activation-environment >/dev/null 2>&1; then "
                 "dbus-update-activation-environment $variables || "
                 "echo 'umbriel: failed to update the D-Bus activation environment' >&2; fi; "
                 "if $systemd_ready; then systemctl --user start --no-block umbriel-session.target || "
                 "echo 'umbriel: failed to start the systemd session target' >&2; fi";
      return command;
    }

  } // namespace

  bool Server::clientHasSecurityContext(const wl_client* client) const {
    return m_securityContextManager != nullptr
        && wlr_security_context_manager_v1_lookup_client(m_securityContextManager, client) != nullptr;
  }

  ContentType Server::surfaceContentType(wlr_surface* surface) const {
    if (m_contentTypeManager == nullptr || surface == nullptr) {
      return ContentType::None;
    }
    switch (wlr_surface_get_content_type_v1(m_contentTypeManager, surface)) {
    case WP_CONTENT_TYPE_V1_TYPE_PHOTO:
      return ContentType::Photo;
    case WP_CONTENT_TYPE_V1_TYPE_VIDEO:
      return ContentType::Video;
    case WP_CONTENT_TYPE_V1_TYPE_GAME:
      return ContentType::Game;
    case WP_CONTENT_TYPE_V1_TYPE_NONE:
    default:
      return ContentType::None;
    }
  }

  bool Server::isXwaylandSurface(const wlr_surface* surface) const {
    if (m_xwayland == nullptr || surface == nullptr || surface->resource == nullptr) {
      return false;
    }

    pid_t pid = -1;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, &uid, &gid);
    return pid > 0 && pid == m_xwayland->pid();
  }

  Server::Server() {
    m_nested = std::getenv("WAYLAND_DISPLAY") != nullptr
        || std::getenv("WAYLAND_SOCKET") != nullptr
        || std::getenv("DISPLAY") != nullptr;

    m_display = wl_display_create();
    if (m_display == nullptr) {
      throw std::runtime_error("failed to create wl_display");
    }
    wl_display_set_default_max_buffer_size(m_display, kWaylandClientBufferSize);

    m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display), &m_session);
    if (m_backend == nullptr) {
      throw std::runtime_error("failed to create wlr_backend");
    }

    m_renderer = fx_renderer_create(m_backend);
    if (m_renderer == nullptr) {
      throw std::runtime_error("failed to create fx_renderer");
    }
    m_rendererLost.notify = onRendererLost;
    wl_signal_add(&m_renderer->events.lost, &m_rendererLost);

    if (!wlr_renderer_init_wl_shm(m_renderer, m_display)) {
      throw std::runtime_error("failed to initialize wl_shm");
    }

    const int drmFd = wlr_renderer_get_drm_fd(m_renderer);
    wlr_linux_dmabuf_v1* linuxDmabuf = nullptr;
    if (wlr_renderer_get_texture_formats(m_renderer, WLR_BUFFER_CAP_DMABUF) != nullptr && drmFd >= 0) {
      if (wlr_drm_create(m_display, m_renderer) == nullptr) {
        kLog.warn("failed to create legacy wl_drm global");
      }
      linuxDmabuf = wlr_linux_dmabuf_v1_create_with_renderer(m_display, 4, m_renderer);
      if (linuxDmabuf == nullptr) {
        throw std::runtime_error("failed to create linux-dmabuf global");
      }
    }

    if (drmFd >= 0 && m_renderer->features.timeline && m_backend->features.timeline) {
      if (wlr_linux_drm_syncobj_manager_v1_create(m_display, 1, drmFd) == nullptr) {
        throw std::runtime_error("failed to create linux-drm-syncobj manager");
      }
      kLog.info("explicit synchronization enabled");
    }

    m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
    if (m_allocator == nullptr) {
      throw std::runtime_error("failed to create wlr_allocator");
    }

    m_compositor = wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_data_device_manager_create(m_display);
    if (wlr_primary_selection_v1_device_manager_create(m_display) == nullptr) {
      throw std::runtime_error("failed to create primary-selection manager");
    }
    m_securityContextManager = wlr_security_context_manager_v1_create(m_display);
    if (m_securityContextManager == nullptr) {
      throw std::runtime_error("failed to create security-context manager");
    }
    wl_display_set_global_filter(m_display, filterGlobal, this);
    wlr_viewporter_create(m_display);
    wlr_fractional_scale_manager_v1_create(m_display, 1);
    wlr_presentation_create(m_display, m_backend, 2);
    m_tearingControlManager = wlr_tearing_control_manager_v1_create(m_display, 1);
    if (m_tearingControlManager == nullptr) {
      throw std::runtime_error("failed to create tearing-control manager");
    }
    m_contentTypeManager = wlr_content_type_manager_v1_create(m_display, 1);
    if (m_contentTypeManager == nullptr) {
      throw std::runtime_error("failed to create content-type manager");
    }
    wlr_ext_data_control_manager_v1_create(m_display, 1);

    m_outputLayout = wlr_output_layout_create(m_display);
    wlr_xdg_output_manager_v1_create(m_display, m_outputLayout);
    m_scene = wlr_scene_create();
    if (linuxDmabuf != nullptr) {
      wlr_scene_set_linux_dmabuf_v1(m_scene, linuxDmabuf);
    }
    if (m_renderer->features.input_color_transform) {
      size_t transferFunctionsLen = 0;
      wp_color_manager_v1_transfer_function* transferFunctions =
          wlr_color_manager_v1_transfer_function_list_from_renderer(m_renderer, &transferFunctionsLen);
      size_t primariesLen = 0;
      wp_color_manager_v1_primaries* primaries =
          wlr_color_manager_v1_primaries_list_from_renderer(m_renderer, &primariesLen);
      constexpr wp_color_manager_v1_render_intent renderIntents[] = {
          WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
      };
      const wlr_color_manager_v1_options options = {
          .features =
              {
                  .icc_v2_v4 = false,
                  .parametric = true,
                  .set_primaries = false,
                  .set_tf_power = false,
                  .set_luminances = false,
                  .set_mastering_display_primaries = true,
                  .extended_target_volume = false,
                  .windows_scrgb = false,
              },
          .render_intents = renderIntents,
          .render_intents_len = std::size(renderIntents),
          .transfer_functions = transferFunctions,
          .transfer_functions_len = transferFunctionsLen,
          .primaries = primaries,
          .primaries_len = primariesLen,
      };
      m_colorManager = wlr_color_manager_v1_create(m_display, 2, &options);
      std::free(transferFunctions);
      std::free(primaries);
      if (m_colorManager == nullptr) {
        throw std::runtime_error("failed to create color-management manager");
      }
      wlr_scene_set_color_manager_v1(m_scene, m_colorManager);
      m_wineColorManager = std::make_unique<WineColorManager>(*this);
      if (!m_wineColorManager->valid()) {
        throw std::runtime_error("failed to create Wine color-management manager");
      }
    }
    const Config::Appearance::Blur& blur = config().appearance.blur;
    wlr_scene_set_blur_data(
        m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
        static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
    );
    m_sceneLayout = wlr_scene_attach_output_layout(m_scene, m_outputLayout);

    // Global stacking keeps scratchpads above normal windows and below drag, panels, fullscreen, overlays, and lock.
    // Per-output layer trees keep normal windows below panels.
    m_backdrop = wlr_scene_rect_create(&m_scene->tree, 0, 0, config().appearance.backdropColor.data());
    wlr_scene_rect_set_corner_radius(m_backdrop, 0);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&m_scene->tree);
    m_overviewBlurTree = wlr_scene_tree_create(&m_scene->tree);
    wlr_scene_node_set_enabled(&m_overviewBlurTree->node, false);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&m_scene->tree);
    m_xdgTree = wlr_scene_tree_create(&m_scene->tree);
    m_scratchpadTree = wlr_scene_tree_create(&m_scene->tree);
    m_scratchpadShadowTree = wlr_scene_tree_create(m_scratchpadTree);
    m_scratchpadContentTree = wlr_scene_tree_create(m_scratchpadTree);
    m_scratchpadManager = std::make_unique<ScratchpadManager>(*this, m_scratchpadContentTree, m_scratchpadShadowTree);
    m_overviewTree = wlr_scene_tree_create(&m_scene->tree);
    wlr_scene_node_set_enabled(&m_overviewTree->node, false);
    m_dragShadowTree = wlr_scene_tree_create(&m_scene->tree);
    m_dragTree = wlr_scene_tree_create(&m_scene->tree);
    m_dragIconTree = wlr_scene_tree_create(&m_scene->tree);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&m_scene->tree);
    m_fullscreenTree = wlr_scene_tree_create(&m_scene->tree);
    m_pinnedShadowTree = wlr_scene_tree_create(&m_scene->tree);
    m_pinnedTree = wlr_scene_tree_create(&m_scene->tree);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&m_scene->tree);
    m_imPopupTree = wlr_scene_tree_create(&m_scene->tree);
    m_cheatsheetTree = wlr_scene_tree_create(&m_scene->tree);
    m_bannerTree = wlr_scene_tree_create(&m_scene->tree);
    m_quitConfirmTree = wlr_scene_tree_create(&m_scene->tree);
    m_lockTree = wlr_scene_tree_create(&m_scene->tree);
    m_lockBlank = wlr_scene_rect_create(m_lockTree, 0, 0, config().appearance.backdropColor.data());
    wlr_scene_rect_set_corner_radius(m_lockBlank, 0);
    wlr_scene_node_set_enabled(&m_lockBlank->node, false);
    wlr_scene_node_set_enabled(&m_lockTree->node, false);
    wlr_scene_node_lower_to_bottom(&m_backdrop->node);

    m_gammaManager = wlr_gamma_control_manager_v1_create(m_display);
    m_setGamma.notify = onSetGamma;
    wl_signal_add(&m_gammaManager->events.set_gamma, &m_setGamma);

    m_xdgShell = wlr_xdg_shell_create(m_display, 3);
    m_newXdgToplevel.notify = onNewXdgToplevel;
    wl_signal_add(&m_xdgShell->events.new_toplevel, &m_newXdgToplevel);
    m_newXdgPopup.notify = onNewXdgPopup;
    wl_signal_add(&m_xdgShell->events.new_popup, &m_newXdgPopup);

    m_xdgToplevelTagManager = wlr_xdg_toplevel_tag_manager_v1_create(m_display, 1);
    if (m_xdgToplevelTagManager == nullptr) {
      throw std::runtime_error("failed to create XDG toplevel tag manager");
    }
    m_setXdgToplevelTag.notify = onSetXdgToplevelTag;
    wl_signal_add(&m_xdgToplevelTagManager->events.set_tag, &m_setXdgToplevelTag);

    m_xdgDecorationManager = wlr_xdg_decoration_manager_v1_create(m_display);
    m_newXdgDecoration.notify = onNewXdgDecoration;
    wl_signal_add(&m_xdgDecorationManager->events.new_toplevel_decoration, &m_newXdgDecoration);

    m_serverDecorationManager = wlr_server_decoration_manager_create(m_display);
    wlr_server_decoration_manager_set_default_mode(
        m_serverDecorationManager,
        config().appearance.preferNoCsd ? WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
                                        : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT
    );

    m_layerShell = wlr_layer_shell_v1_create(m_display, 4);
    m_newLayerSurface.notify = onNewLayerSurface;
    wl_signal_add(&m_layerShell->events.new_surface, &m_newLayerSurface);

    m_foreignToplevelManager = wlr_foreign_toplevel_manager_v1_create(m_display);
    m_extForeignToplevelList = wlr_ext_foreign_toplevel_list_v1_create(m_display, 1);
    m_toplevelCaptureSourceManager = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(m_display, 1);
    m_toplevelCaptureRequest.notify = onToplevelCaptureRequest;
    wl_signal_add(&m_toplevelCaptureSourceManager->events.new_request, &m_toplevelCaptureRequest);

    m_workspaceManager = wlr_ext_workspace_manager_v1_create(m_display, 1);
    m_workspaceCommit.notify = onWorkspaceCommit;
    wl_signal_add(&m_workspaceManager->events.commit, &m_workspaceCommit);

    m_sessionLockManager = wlr_session_lock_manager_v1_create(m_display);
    m_newSessionLock.notify = onNewSessionLock;
    wl_signal_add(&m_sessionLockManager->events.new_lock, &m_newSessionLock);

    m_relativePointerManager = wlr_relative_pointer_manager_v1_create(m_display);
    m_pointerConstraints = wlr_pointer_constraints_v1_create(m_display);
    m_newPointerConstraint.notify = onNewPointerConstraint;
    wl_signal_add(&m_pointerConstraints->events.new_constraint, &m_newPointerConstraint);
    m_pointerGestures = wlr_pointer_gestures_v1_create(m_display);
    m_virtualKeyboardManager = wlr_virtual_keyboard_manager_v1_create(m_display);
    m_newVirtualKeyboard.notify = onNewVirtualKeyboard;
    wl_signal_add(&m_virtualKeyboardManager->events.new_virtual_keyboard, &m_newVirtualKeyboard);
    m_virtualPointerManager = wlr_virtual_pointer_manager_v1_create(m_display);
    m_newVirtualPointer.notify = onNewVirtualPointer;
    wl_signal_add(&m_virtualPointerManager->events.new_virtual_pointer, &m_newVirtualPointer);
    m_tabletManager = wlr_tablet_v2_create(m_display);

    m_idleNotifier = wlr_idle_notifier_v1_create(m_display);
    m_idleInhibitManager = wlr_idle_inhibit_v1_create(m_display);
    m_newIdleInhibitor.notify = onNewIdleInhibitor;
    wl_signal_add(&m_idleInhibitManager->events.new_inhibitor, &m_newIdleInhibitor);

    wlr_screencopy_manager_v1_create(m_display);
    m_exportDmabufManager = wlr_export_dmabuf_manager_v1_create(m_display);
    wlr_ext_output_image_capture_source_manager_v1_create(m_display, 1);
    wlr_ext_image_copy_capture_manager_v1_create(m_display, 1);

    // Create the manager so apply/test listeners stay wired, but leave heads empty (see updateOutputManagerConfig).
    // Advertising a full configuration on bind currently takes down the desktop shell from this flake.
    m_outputManager = wlr_output_manager_v1_create(m_display);
    m_outputManagerApply.notify = onOutputManagerApply;
    wl_signal_add(&m_outputManager->events.apply, &m_outputManagerApply);
    m_outputManagerTest.notify = onOutputManagerTest;
    wl_signal_add(&m_outputManager->events.test, &m_outputManagerTest);
    m_outputLayoutChange.notify = onOutputLayoutChange;
    wl_signal_add(&m_outputLayout->events.change, &m_outputLayoutChange);

    m_xdgActivation = wlr_xdg_activation_v1_create(m_display);
    m_newActivationToken.notify = onNewActivationToken;
    wl_signal_add(&m_xdgActivation->events.new_token, &m_newActivationToken);
    m_requestActivate.notify = onRequestActivate;
    wl_signal_add(&m_xdgActivation->events.request_activate, &m_requestActivate);

    m_cursor = std::make_unique<Cursor>(*this);
    m_seat = std::make_unique<Seat>(*this);
    m_padKeyboardFocusChange.notify = onPadKeyboardFocusChange;
    wl_signal_add(&m_seat->wlr()->keyboard_state.events.focus_change, &m_padKeyboardFocusChange);
    m_inputMethodRelay = std::make_unique<InputMethodRelay>(*this);
    m_gestures = std::make_unique<Gestures>(*this);
    m_overview = std::make_unique<Overview>(*this);
    updateSeatCapabilities();

    m_newOutput.notify = onNewOutput;
    wl_signal_add(&m_backend->events.new_output, &m_newOutput);
    m_newInput.notify = onNewInput;
    wl_signal_add(&m_backend->events.new_input, &m_newInput);

    wlr_log(WLR_INFO, "mod key: %s (%s session)", m_nested ? "Alt" : "Super", m_nested ? "nested" : "native");
    kLog.info("mod key: {} ({} session)", m_nested ? "Alt" : "Super", m_nested ? "nested" : "native");
  }

  Server::~Server() {
    m_stopping = true;
    wl_list_remove(&m_newOutput.link);
    wl_list_remove(&m_newInput.link);
    wl_list_remove(&m_newXdgToplevel.link);
    wl_list_remove(&m_setXdgToplevelTag.link);
    wl_list_remove(&m_newXdgPopup.link);
    wl_list_remove(&m_newXdgDecoration.link);
    wl_list_remove(&m_newLayerSurface.link);
    wl_list_remove(&m_newSessionLock.link);
    wl_list_remove(&m_newPointerConstraint.link);
    wl_list_remove(&m_newVirtualKeyboard.link);
    wl_list_remove(&m_newVirtualPointer.link);
    wl_list_remove(&m_newIdleInhibitor.link);
    wl_list_remove(&m_newActivationToken.link);
    wl_list_remove(&m_requestActivate.link);
    wl_list_remove(&m_workspaceCommit.link);
    wl_list_remove(&m_setGamma.link);
    wl_list_remove(&m_outputManagerApply.link);
    wl_list_remove(&m_outputManagerTest.link);
    wl_list_remove(&m_outputLayoutChange.link);
    wl_list_remove(&m_rendererLost.link);
    wl_list_remove(&m_toplevelCaptureRequest.link);
    wl_list_remove(&m_padKeyboardFocusChange.link);
    m_configWatcher.reset();
    m_ipc.reset();

    m_insertHint.reset();
    // Each snapshot destroys its own scene tree.
    m_closeSnapshots.clear();
    m_configBanner.reset();
    // Owns scene nodes under m_scene; must go before the scene teardown below.
    m_cheatsheet.reset();
    m_overview.reset();
    m_sessionLock.reset();
    m_layerSurfaces.clear();
    m_registry.clear();
    m_keyboards.clear();
    m_outputs.clear();
    m_inputMethodRelay.reset();
    m_seat.reset();
    // Holds listeners on the wlr_cursor's gesture signals. wlr_cursor_destroy
    // asserts its signal lists are empty, so this must precede the cursor.
    m_gestures.reset();
    m_cursor.reset();

    // Tear down xwayland-satellite before destroying Wayland clients.
    m_xwayland.reset();
    if (m_backgroundFrameTimer != nullptr) {
      wl_event_source_remove(m_backgroundFrameTimer);
      m_backgroundFrameTimer = nullptr;
    }
    for (wl_event_source*& source : m_signalSources) {
      if (source != nullptr) {
        wl_event_source_remove(source);
        source = nullptr;
      }
    }
    wl_display_destroy_clients(m_display);
    m_wineColorManager.reset();
    // Chrome components destroy scene nodes in their destructors, so they must go before the scene tree does; otherwise
    // the destructor body frees the nodes and the member destructors touch already-freed memory.
    m_quitConfirm.reset();
    m_cheatsheet.reset();
    m_configBanner.reset();
    m_scratchpadManager.reset();
    wlr_scene_node_destroy(&m_scene->tree.node);
    wlr_allocator_destroy(m_allocator);
    wlr_renderer_destroy(m_renderer);
    wlr_backend_destroy(m_backend);
    wl_display_destroy(m_display);
  }

  const wlr_image_description_v1_data* Server::surfaceImageDescription(wlr_surface* surface) const {
    if (const wlr_image_description_v1_data* description = wlr_surface_get_image_description_v1_data(surface)) {
      return description;
    }
    if (m_wineColorManager != nullptr) {
      if (const wlr_image_description_v1_data* description = m_wineColorManager->surfaceDescription(surface)) {
        return description;
      }
    }
    return nullptr;
  }

  const wlr_image_description_v1_data* Server::surfaceTreeHdrDescription(wlr_surface* surface) const {
    struct Context {
      const Server* server;
      const wlr_image_description_v1_data* description = nullptr;
    } context{.server = this};

    wlr_surface_for_each_surface(
        surface,
        [](wlr_surface* candidate, int, int, void* data) {
          auto* context = static_cast<Context*>(data);
          if (context->description != nullptr) {
            return;
          }
          const wlr_image_description_v1_data* description = context->server->surfaceImageDescription(candidate);
          if (description == nullptr) {
            return;
          }
          const bool pqBt2020 = description->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
              && description->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
          const WineColorManager* wine = context->server->wineColorManager();
          if (pqBt2020 || (wine != nullptr && wine->surfaceRequiresHdrOutput(candidate))) {
            context->description = description;
          }
        },
        &context
    );
    return context.description;
  }

  void Server::updateColorPreferences() {
    if (m_wineColorManager != nullptr) {
      m_wineColorManager->updatePreferredDescriptions();
    }
  }

  bool Server::start(const char* startupCmd) {
    const char* socket = wl_display_add_socket_auto(m_display);
    if (socket == nullptr) {
      wlr_log(WLR_ERROR, "failed to add Wayland socket");
      return false;
    }
    m_socketName = socket;

    if (!wlr_backend_start(m_backend)) {
      wlr_log(WLR_ERROR, "failed to start backend");
      return false;
    }

    // These are delivered through the event loop, not a signal handler, so the shutdown path is ordinary code. Note the
    // side effect: wl_event_loop_add_signal blocks the signal process-wide, and a blocked mask survives fork and exec,
    // so every fork below must clear it before handing off to a child.
    wl_event_loop* loop = wl_display_get_event_loop(m_display);
    m_signalSources[0] = wl_event_loop_add_signal(loop, SIGINT, onTerminateSignal, this);
    m_signalSources[1] = wl_event_loop_add_signal(loop, SIGTERM, onTerminateSignal, this);

    // Point new clients at us. Drop WAYLAND_SOCKET so children do not keep the
    // parent compositor connection (libwayland prefers it over WAYLAND_DISPLAY).
    setenv("WAYLAND_DISPLAY", m_socketName.c_str(), true);
    unsetenv("WAYLAND_SOCKET");
    kLog.info("running on WAYLAND_DISPLAY={}", m_socketName);

    // Keep clients on hidden workspaces receiving wl_surface.frame ticks
    // (see kBackgroundFrameIntervalMs and Server::onBackgroundFrameTimer).
    m_backgroundFrameTimer =
        wl_event_loop_add_timer(wl_display_get_event_loop(m_display), onBackgroundFrameTimer, this);
    if (m_backgroundFrameTimer != nullptr) {
      wl_event_source_timer_update(m_backgroundFrameTimer, kBackgroundFrameIntervalMs);
    }
    setenv(
        "UMBRIEL_SOCKET",
        (std::string(std::getenv("XDG_RUNTIME_DIR") ?: "") + "/umbriel-" + m_socketName + ".sock").c_str(), true
    );
    m_ipc = std::make_unique<Ipc>(*this, m_socketName);
    setenv("XDG_CURRENT_DESKTOP", "umbriel", 1);
    setenv("XDG_SESSION_DESKTOP", "umbriel", 1);
    setenv("XDG_SESSION_TYPE", "wayland", 1);

    // Export cursor settings so X11 clients (via xwayland-satellite) and
    // toolkit clients pick up the compositor's configured cursor.
    const auto& cursorCfg = config().input.cursor;
    const std::string cursorSize = std::to_string(cursorCfg.size);
    setenv("XCURSOR_SIZE", cursorSize.c_str(), 1);
    if (!cursorCfg.theme.empty()) {
      setenv("XCURSOR_THEME", cursorCfg.theme.c_str(), 1);
    }

    // Start xwayland-satellite before session services and autostart so X11
    // applications can connect. The supervisor applies configured values in
    // the child while this process retains its session-control environment.
    if (config().general.xwayland) {
      m_xwayland = std::make_unique<XwaylandSupervisor>(wl_display_get_event_loop(m_display), m_socketName);
      m_xwayland->start();
    }

    // Fork session synchronization before applying configured values locally.
    // Its child keeps the inherited PATH and bus addresses, while the target
    // receives the explicit configured assignments before it starts.
    if (!m_nested) {
      const std::string command = sessionEnvironmentCommand();
      spawn(command.c_str(), "session environment synchronization");
    }
    applyConfiguredEnvironment();
    if (startupCmd != nullptr) {
      spawn(startupCmd);
    }
    for (const std::string& command : config().general.autostart) {
      spawn(command.c_str());
    }
    m_configWatcher =
        std::make_unique<ConfigWatcher>(wl_display_get_event_loop(m_display), [this] { handleConfigReload(); });
    m_configBanner = std::make_unique<ConfigBanner>(*this, m_bannerTree);
    m_configWatcher->watch(configWatchPaths());
    showConfigDiagnostics();
    m_cheatsheet = std::make_unique<Cheatsheet>(*this, m_cheatsheetTree);
    if (config().general.showCheatsheet) {
      m_cheatsheet->showOnStartup();
    }
    m_quitConfirm = std::make_unique<QuitConfirm>(*this, m_quitConfirmTree);
    return true;
  }

  void Server::showConfigDiagnostics() { m_configBanner->show(configDiagnostics()); }

  void Server::markDirty(Dirty what) {
    m_dirty |= what;
    // Recording is only useful if a frame follows, so ask for one. Marking work
    // that nothing then flushes would simply never happen.
    for (const auto& output : m_outputs) {
      if (output->wlr() != nullptr) {
        wlr_output_schedule_frame(output->wlr());
      }
    }
  }

  void Server::relayoutBanner() {
    if (m_configBanner != nullptr) {
      m_configBanner->relayout();
    }
  }

  void Server::relayoutCheatsheet() {
    if (m_cheatsheet != nullptr) {
      m_cheatsheet->relayout();
    }
  }

  void Server::relayoutQuitConfirm() {
    if (m_quitConfirm != nullptr) {
      m_quitConfirm->relayout();
    }
  }

  int Server::onTerminateSignal(int signal, void* data) {
    kLog.info("received signal {}, shutting down", signal);
    static_cast<Server*>(data)->stop();
    return 0;
  }

  void Server::run() { wl_display_run(m_display); }

  void Server::stop() {
    m_stopping = true;
    wl_display_terminate(m_display);
  }

  uint32_t Server::modKey() const {
    if (!config().general.modKey) {
      return m_nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
    }
    switch (*config().general.modKey) {
    case ModifierKey::Super:
      return WLR_MODIFIER_LOGO;
    case ModifierKey::Alt:
      return WLR_MODIFIER_ALT;
    case ModifierKey::Control:
      return WLR_MODIFIER_CTRL;
    case ModifierKey::Shift:
      return WLR_MODIFIER_SHIFT;
    }
    return WLR_MODIFIER_LOGO;
  }

  std::string_view Server::modKeyName() const {
    if (!config().general.modKey) {
      return m_nested ? "Alt" : "Super";
    }
    switch (*config().general.modKey) {
    case ModifierKey::Super:
      return "Super";
    case ModifierKey::Alt:
      return "Alt";
    case ModifierKey::Control:
      return "Ctrl";
    case ModifierKey::Shift:
      return "Shift";
    }
    return "Super";
  }

  HintRect& Server::insertHint() {
    if (m_insertHint == nullptr) {
      m_insertHint = std::make_unique<HintRect>(*this, dragTree());
    }
    return *m_insertHint;
  }

  void Server::hideInsertHint() {
    if (m_insertHint != nullptr) {
      m_insertHint->hideImmediate();
    }
  }

  wlr_scene_tree* Server::shellLayerTree(uint32_t layer) const {
    if (layer >= kLayerCount) {
      return m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
    }
    return m_shellLayerTrees[layer];
  }

  void Server::spawn(const char* command, const char* description, bool withActivationToken) {
    if (m_socketName.empty()) {
      wlr_log(WLR_ERROR, "cannot spawn before the Wayland socket exists");
      return;
    }

    wlr_xdg_activation_token_v1* launchToken = nullptr;
    std::string launchTokenName;
    if (withActivationToken && m_xdgActivation != nullptr) {
      launchToken = wlr_xdg_activation_token_v1_create(m_xdgActivation);
      if (launchToken != nullptr) {
        trackActivationToken(launchToken, true);
        const char* name = wlr_xdg_activation_token_v1_get_name(launchToken);
        if (name != nullptr) {
          launchTokenName = name;
        }
      }
    }

    pid_t pid = fork();
    if (pid < 0) {
      if (launchToken != nullptr) {
        wlr_xdg_activation_token_v1_destroy(launchToken);
      }
      wlr_log(WLR_ERROR, "fork failed");
      return;
    }
    if (pid == 0) {
      resetChildSignalState();
      restoreFileDescriptorLimit();
      setenv("WAYLAND_DISPLAY", m_socketName.c_str(), 1);
      unsetenv("WAYLAND_SOCKET");
      if (m_xwayland != nullptr && !m_xwayland->display().empty()) {
        setenv("DISPLAY", m_xwayland->display().c_str(), 1);
      } else {
        // Avoid X11/XWayland fallback into the parent session.
        unsetenv("DISPLAY");
      }
      if (!launchTokenName.empty()) {
        setenv("XDG_ACTIVATION_TOKEN", launchTokenName.c_str(), 1);
        setenv("DESKTOP_STARTUP_ID", launchTokenName.c_str(), 1);
      } else {
        unsetenv("XDG_ACTIVATION_TOKEN");
        unsetenv("DESKTOP_STARTUP_ID");
      }
      execl("/bin/sh", "/bin/sh", "-c", command, nullptr);
      _exit(1);
    }

    wlr_log(
        WLR_INFO, "spawned '%s' on WAYLAND_DISPLAY=%s", description == nullptr ? command : description,
        m_socketName.c_str()
    );
  }

  void Server::updateSeatCapabilities() { m_seat->updateCapabilities(!m_keyboards.empty(), !m_touchDevices.empty()); }

  void Server::reconcileDynamicWorkspaces() {
    for (const auto& output : m_outputs) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        group->reconcileDynamic();
      }
    }
  }

  Server::CloseSnapshot::CloseSnapshot(
      Server& server, Output* output, wlr_scene_tree* tree, std::vector<BorderSnapshot> borders, int durationMs,
      const AnimationCurve& curve, std::string_view style
  )
      : m_server(&server), m_tree(tree), m_output(output), m_borders(std::move(borders)) {
    if (m_tree != nullptr) {
      m_origX = m_tree->node.x;
      m_origY = m_tree->node.y;
    }
    wlr_scene_node_for_each_buffer(
        &m_tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto* buffers = static_cast<std::vector<std::pair<wlr_scene_buffer*, float>>*>(data);
          buffers->emplace_back(buffer, buffer->opacity);
        },
        &m_buffers
    );
    m_alpha.snap(1.0);
    m_alpha.retarget(0.0, durationMs, curve);

    if (style == "slide") {
      m_posY.snap(m_origY);
      m_posY.retarget(m_origY + 80, durationMs, curve);
    }
  }

  Server::CloseSnapshot::~CloseSnapshot() {
    if (m_server != nullptr) {
      m_server->unregisterAnimatable(this);
    }
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
  }

  bool Server::CloseSnapshot::tickAnimations(uint64_t nowMsec) {
    const bool movedAlpha = m_alpha.tick(nowMsec);
    const bool movedY = m_posY.tick(nowMsec);

    if (!movedAlpha && !movedY) {
      return false;
    }
    // Overshooting curves can push this out of range; wlr_scene_buffer_set_opacity asserts opacity is in [0, 1].
    const auto alpha = std::clamp(static_cast<float>(m_alpha.current()), 0.0F, 1.0F);
    for (auto& [buffer, baseOpacity] : m_buffers) {
      wlr_scene_buffer_set_opacity(buffer, std::clamp(baseOpacity * alpha, 0.0F, 1.0F));
    }
    for (auto& border : m_borders) {
      float innerColor[4];
      float outerColor[4];
      premultiplied(innerColor, border.innerColor, alpha);
      premultiplied(outerColor, border.outerColor, alpha);
      wlr_scene_border_set_colors(border.node, innerColor, outerColor);
    }

    if (m_tree != nullptr && movedY) {
      wlr_scene_node_set_position(&m_tree->node, m_origX, static_cast<int>(std::lround(m_posY.current())));
    }
    return m_alpha.animating() || m_posY.animating();
  }

  void Server::registerAnimatable(Animatable* animatable) {
    if (animatable == nullptr) {
      return;
    }
    // Insert after every owner in an earlier or equal phase, so the registry
    // stays in phase order and registration order is preserved within a phase.
    const auto phase = animatable->animationPhase();
    const auto at = std::ranges::find_if(m_animatables, [phase](const Animatable* entry) {
      return entry->animationPhase() > phase;
    });
    m_animatables.insert(at, animatable);
  }

  void Server::unregisterAnimatable(Animatable* animatable) { std::erase(m_animatables, animatable); }

  bool Server::tickAnimations(uint64_t nowMsec) {
    if (nowMsec == m_lastAnimTickMsec) {
      return animationsActive();
    }
    m_lastAnimTickMsec = nowMsec;

    bool active = false;
    // Iterate a copy so a registration during the pass cannot invalidate the walk. Nothing unregisters mid-pass:
    // snapshots are reaped below rather than from their own tick, and no owner destroys another. The phase order is
    // what makes that hold, since finishing the overview calls focusView, which reorders the view registry after every
    // view has already ticked.
    m_animatablesScratch.assign(m_animatables.begin(), m_animatables.end());
    for (Animatable* owner : m_animatablesScratch) {
      active = owner->tickAnimations(nowMsec) || active;
    }

    // A snapshot only exists while fading out, so a settled one is finished.
    // Erasing destroys its scene tree and unregisters it.
    std::erase_if(m_closeSnapshots, [this](const std::unique_ptr<CloseSnapshot>& snap) {
      if (snap->hasActiveAnimations()) {
        return false;
      }
      unregisterAnimatable(snap.get());
      return true;
    });
    return active;
  }

  void Server::flushPendingViewOpacities() {
    for (const auto& view : m_registry.all()) {
      view->flushPendingEffectiveOpacity();
    }
  }

  bool Server::animationsActive() const {
    return std::ranges::any_of(m_animatables, [](const Animatable* owner) { return owner->hasActiveAnimations(); });
  }

  bool Server::animationsActiveFor(const Output* output) const {
    if (output == nullptr) {
      return false;
    }
    return std::ranges::any_of(m_animatables, [output](const Animatable* owner) {
      return owner->hasActiveAnimations() && owner->animatesOn(output);
    });
  }

  void Server::animateCloseSnapshot(
      Output* output, wlr_scene_tree* tree, std::vector<BorderSnapshot> borders,
      std::optional<CloseSnapshotOverrides> overrides
  ) {
    int durationMs = 0;
    AnimationCurve curve{.easing = Easing::Snappy};
    std::string style = "fade";
    if (overrides) {
      durationMs = overrides->durationMs;
      curve = overrides->curve;
      style = overrides->style;
    } else {
      const auto& animation = config().animation;
      const auto& close = animation.windowsOut;
      if (!animation.enabled || !close.enabled) {
        wlr_scene_node_destroy(&tree->node);
        return;
      }
      durationMs = close.durationMs;
      curve = close.curve;
      style = close.style;
    }
    if (durationMs <= 0) {
      wlr_scene_node_destroy(&tree->node);
      return;
    }

    auto snapshot = std::make_unique<CloseSnapshot>(*this, output, tree, std::move(borders), durationMs, curve, style);
    registerAnimatable(snapshot.get());
    m_closeSnapshots.push_back(std::move(snapshot));
  }

} // namespace umbriel
