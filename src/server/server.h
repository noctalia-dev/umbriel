#pragma once
#include "core/animation.h"
#include "core/dirty.h"
#include "input/modifier_tap.h"
#include "scene/border_rect.h"
#include "server/focus.h"
#include "view/registry.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_box;
struct wlr_compositor;
struct wlr_color_manager_v1;
struct wlr_image_description_v1_data;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1;
struct wlr_export_dmabuf_manager_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_input_device;
struct wlr_layer_shell_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_gestures_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_session;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct wlr_surface;
struct wlr_tablet;
struct wlr_tablet_manager_v2;
struct wlr_tablet_pad;
struct wlr_tablet_v2_tablet;
struct wlr_tablet_v2_tablet_pad;
struct wlr_tearing_control_manager_v1;
struct wlr_xdg_activation_v1;
struct wlr_xdg_activation_token_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_shell;
struct wlr_server_decoration_manager;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_output_manager_v1;
struct wlr_output_configuration_v1;
struct wlr_scene_tree;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_virtual_pointer_manager_v1;
struct wlr_virtual_pointer_v1;

namespace umbriel {
  struct ConfigEffects;

  // Slow tick that ferries wl_surface.frame callbacks to toplevels that are mapped but not on the active workspace.
  // wlroots' scene helper only walks enabled scene nodes, so a hidden view otherwise never receives another frame_done
  // and any client that gates its game/network loop on the frame callback stalls until it becomes visible again
  // (Overwatch under Proton-CachyOS times out its server heartbeat within ~30 s of alt-tab). 10 Hz keeps game logic and
  // networking alive at negligible cost.
  inline constexpr int kBackgroundFrameIntervalMs = 100;

  enum class WheelDirection;
  struct Keybind;

  class Cursor;
  class FocusManager;
  class XwaylandSupervisor;
  class ConfigWatcher;
  class InputMethodRelay;
  class Gestures;
  class WineColorManager;
  class HintRect;
  class Keyboard;
  class LayerSurface;
  class Output;
  class Overview;
  class Seat;
  class SessionLock;
  class View;
  class Workspace;
  class WorkspaceGroup;
  class ScratchpadManager;
  class ConfigBanner;
  class Cheatsheet;
  class QuitConfirm;
  class Ipc;

  class Server {
  public:
    static constexpr uint32_t kLayerCount = 4;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const char* startupCmd = nullptr);
    void run();
    void stop();
    [[nodiscard]] bool stopping() const { return m_stopping; }

    [[nodiscard]] wl_display* display() const { return m_display; }
    [[nodiscard]] wlr_backend* backend() const { return m_backend; }
    [[nodiscard]] wlr_session* session() const { return m_session; }
    [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }
    [[nodiscard]] wlr_allocator* allocator() const { return m_allocator; }
    [[nodiscard]] wlr_scene* scene() const { return m_scene; }
    [[nodiscard]] wlr_color_manager_v1* colorManager() const { return m_colorManager; }
    [[nodiscard]] wlr_export_dmabuf_manager_v1* exportDmabufManager() const { return m_exportDmabufManager; }
    [[nodiscard]] wlr_tearing_control_manager_v1* tearingControlManager() const { return m_tearingControlManager; }
    [[nodiscard]] WineColorManager* wineColorManager() const { return m_wineColorManager.get(); }
    [[nodiscard]] const wlr_image_description_v1_data* surfaceImageDescription(wlr_surface* surface) const;
    [[nodiscard]] const wlr_image_description_v1_data* surfaceTreeHdrDescription(wlr_surface* surface) const;
    void updateColorPreferences();
    [[nodiscard]] wlr_scene_tree* xdgTree() const { return m_xdgTree; }
    [[nodiscard]] wlr_scene_tree* scratchpadTree() const { return m_scratchpadTree; }
    [[nodiscard]] wlr_scene_tree* scratchpadShadowTree() const { return m_scratchpadShadowTree; }
    [[nodiscard]] ScratchpadManager* scratchpadManager() const { return m_scratchpadManager.get(); }
    // Between layer-shell background and bottom: overview wallpaper blur renders
    // before bottom-layer widgets so they remain sharp.
    [[nodiscard]] wlr_scene_tree* overviewBlurTree() const { return m_overviewBlurTree; }
    // Between windows and the drag/insert-hint tree: overview cards render here
    // while the real window trees are disabled.
    [[nodiscard]] wlr_scene_tree* overviewTree() const { return m_overviewTree; }
    [[nodiscard]] Overview* overview() const { return m_overview.get(); }
    [[nodiscard]] ConfigBanner* configBanner() const { return m_configBanner.get(); }
    [[nodiscard]] Cheatsheet* cheatsheet() const { return m_cheatsheet.get(); }
    [[nodiscard]] QuitConfirm* quitConfirm() const { return m_quitConfirm.get(); }
    [[nodiscard]] wlr_scene_tree* dragShadowTree() const { return m_dragShadowTree; }
    // Above xdg windows, below layer-shell top/overlay (drag/drop insert hint).
    [[nodiscard]] wlr_scene_tree* dragTree() const { return m_dragTree; }
    // Parent for wl_data_device drag icons; moved to the cursor while a drag is active.
    [[nodiscard]] wlr_scene_tree* dragIconTree() const { return m_dragIconTree; }
    // Above top panels, below overlay/lock (fullscreen xdg views).
    [[nodiscard]] wlr_scene_tree* fullscreenTree() const { return m_fullscreenTree; }
    [[nodiscard]] wlr_scene_tree* pinnedTree() const { return m_pinnedTree; }
    [[nodiscard]] wlr_scene_tree* pinnedShadowTree() const { return m_pinnedShadowTree; }
    [[nodiscard]] wlr_scene_tree* imPopupTree() const { return m_imPopupTree; }
    [[nodiscard]] wlr_scene_tree* lockTree() const { return m_lockTree; }
    [[nodiscard]] wlr_scene_tree* shellLayerTree(uint32_t layer) const;
    [[nodiscard]] wlr_output_layout* outputLayout() const { return m_outputLayout; }
    [[nodiscard]] wlr_scene_output_layout* sceneLayout() const { return m_sceneLayout; }
    [[nodiscard]] InputMethodRelay* inputMethodRelay() const { return m_inputMethodRelay.get(); }
    [[nodiscard]] Seat* seat() const { return m_seat.get(); }
    [[nodiscard]] Cursor* cursor() const { return m_cursor.get(); }
    // Central animation tick: advances every registered owner once per msec and
    // reports whether anything is still animating.
    bool tickAnimations(uint64_t nowMsec);
    void flushPendingViewOpacities();
    [[nodiscard]] bool animationsActive() const;
    [[nodiscard]] bool animationsActiveFor(const Output* output) const;
    // Owners register themselves for the frame tick. The registry is kept in phase order, so the three traversals above
    // never re-state which owners exist or in what order they run.
    void registerAnimatable(Animatable* animatable);
    void unregisterAnimatable(Animatable* animatable);
    [[nodiscard]] HintRect& insertHint();
    void hideInsertHint();
    [[nodiscard]] SessionLock* sessionLock() const { return m_sessionLock.get(); }
    [[nodiscard]] bool sessionLocked() const { return m_sessionLocked; }
    [[nodiscard]] const std::string& activeSubmap() const {
      static const std::string empty;
      return m_activeSubmaps.empty() ? empty : m_activeSubmaps.back();
    }
    // Distinct from activeSubmap(): the stack can hold an entry whose name is
    // empty, and the submap-reset action keys off depth, not the name.
    [[nodiscard]] bool inSubmap() const { return !m_activeSubmaps.empty(); }
    void pushSubmap(const std::string& name);
    void popSubmap();
    [[nodiscard]] wlr_foreign_toplevel_manager_v1* foreignToplevelManager() const { return m_foreignToplevelManager; }
    [[nodiscard]] wlr_ext_foreign_toplevel_list_v1* extForeignToplevelList() const { return m_extForeignToplevelList; }
    [[nodiscard]] wlr_ext_workspace_manager_v1* workspaceManager() const { return m_workspaceManager; }
    [[nodiscard]] wlr_gamma_control_manager_v1* gammaManager() const { return m_gammaManager; }
    [[nodiscard]] wlr_pointer_constraints_v1* pointerConstraints() const { return m_pointerConstraints; }
    [[nodiscard]] wlr_relative_pointer_manager_v1* relativePointerManager() const { return m_relativePointerManager; }
    [[nodiscard]] wlr_pointer_gestures_v1* pointerGestures() const { return m_pointerGestures; }
    [[nodiscard]] wlr_virtual_pointer_manager_v1* virtualPointerManager() const { return m_virtualPointerManager; }
    [[nodiscard]] wlr_tablet_manager_v2* tabletManager() const { return m_tabletManager; }
    [[nodiscard]] Gestures* gestures() const { return m_gestures.get(); }
    [[nodiscard]] bool nested() const { return m_nested; }
    [[nodiscard]] uint32_t modKey() const;
    [[nodiscard]] std::string_view modKeyName() const;

    // Read-only iteration over the registries. These replace the friend list: callers can walk the registry without
    // reaching into Server's internals or being able to add to and remove from it.
    [[nodiscard]] std::span<const std::unique_ptr<Output>> outputs() const { return m_outputs; }
    [[nodiscard]] std::span<const std::unique_ptr<View>> views() const { return m_registry.all(); }
    [[nodiscard]] ViewRegistry& registry() { return m_registry; }
    [[nodiscard]] std::span<const std::unique_ptr<LayerSurface>> layerSurfaces() const { return m_layerSurfaces; }
    [[nodiscard]] bool isXwaylandSurface(const wlr_surface* surface) const;

    // Runs a parsed action. Shared by the keybind path and the IPC `msg` command.
    bool executeKeybindAction(const Keybind& bind, std::string* error = nullptr);
    // Record that something server-wide became stale. The work happens once, in a fixed order, at the top of the next
    // frame (see Output::flushDirty). Schedules a frame on every output, so recording is always enough.
    void markDirty(Dirty what);
    // Hand the pending set to the flush and clear it.
    [[nodiscard]] Dirty takeDirty() {
      const Dirty pending = m_dirty;
      m_dirty = Dirty::None;
      return pending;
    }
    void relayoutBanner();
    void relayoutCheatsheet();
    void relayoutQuitConfirm();
    void spawn(const char* command, const char* description = nullptr);
    void handleConfigReload();
    // Re-evaluate application idle inhibitors after a surface's presentation
    // visibility changes.
    void updateIdleInhibit();
    // Lock the next XKB group on every physical keyboard. False when no keyboard
    // has a second layout to switch to.
    bool cycleKeyboardLayout();

    struct KeyboardLayoutState {
      std::vector<std::string> names;
      uint32_t currentIndex = 0;
    };
    // The layout names and effective group index of the first physical keyboard, the same device keyboardLayoutState
    // callers observe cycling. Nullopt when no physical keyboard exists.
    [[nodiscard]] std::optional<KeyboardLayoutState> keyboardLayoutState() const;
    // Fires the IPC keyboard-layout event when the effective group of a physical keyboard actually changed. Config
    // reloads re-send the current state even when the group did not move.
    void notifyKeyboardLayoutIpc();
    void notifyOverviewChanged();
    // Coalesced windows-event notification: at most one idle callback per frame
    // regardless of how many window-list-relevant changes happened.
    void scheduleIpcWindowsEvent();

    // Focus lives in FocusManager; these forward so call sites that already
    // hold a Server do not need a second reference.
    void focusView(View* view, FocusReason reason = FocusReason::Startup) { m_focus.focusView(view, reason); }
    void restoreActivatedViewKeyboardFocus() { m_focus.restoreActivatedViewKeyboardFocus(); }
    View* viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer = nullptr) {
      return m_focus.viewAt(lx, ly, surface, sx, sy, layer);
    }
    std::optional<Keybind> handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers);
    // The bind this press would fire, without running it. Null when nothing
    // matches (locked sessions match nothing, matching handleKeybind).
    [[nodiscard]] const Keybind* matchKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) const;
    // Combined state across keyboard devices. Media and brightness keys are
    // commonly exposed by a separate device from the held modifier keys.
    [[nodiscard]] uint32_t keyboardModifiers() const;
    bool handleWheelBind(WheelDirection direction, uint32_t modifiers);
    // Null when no bind matched or its action declined; otherwise the bind that
    // ran, so the caller can tell which action consumed the press.
    std::optional<Keybind> handleMouseBind(uint32_t button, uint32_t modifiers);
    bool handleVtSwitch(uint32_t keysym, uint32_t modifiers);
    void armModifierTap(const void* source, uint32_t keycode, std::span<const uint32_t> keysyms, uint32_t modifiers);
    [[nodiscard]] std::optional<Keybind> releaseModifierTap(const void* source, uint32_t keycode);
    void cancelModifierTap() { m_modifierTap.cancel(); }
    void arrangeLayers(wlr_output* output);
    // Re-send every surface's output scale (views, layer surfaces, popups). Run after anything that changes an output's
    // scale or moves surfaces between outputs outside setWorkspace; the notifies dedup, so extra calls are cheap.
    void refreshSurfaceScales();
    // Re-evaluate each output's policy after keyboard focus or a dynamic window rule changes.
    void refreshOutputPolicies();
    [[nodiscard]] wlr_output* preferredOutput() const;
    [[nodiscard]] Output* outputFromWlr(wlr_output* output) const;
    [[nodiscard]] Output* outputFromName(const std::string& name) const;
    [[nodiscard]] wlr_box usableAreaAt(double lx, double ly) const;
    void updateOutputManagerConfig();
    // Recompute the mapping for every tablet: focused window, focused output, named output, or full layout. Called at
    // the start of every tablet event so dynamic targets reflect current focus without signal hooks.
    void remapTablets();
    // The tablet-v2 handle for a wlroots tablet, or nullptr when unknown.
    [[nodiscard]] wlr_tablet_v2_tablet* tabletV2FromWlr(const wlr_tablet* tablet) const;

    // Create and destroy headless outputs at runtime, which is how a test drives a monitor being unplugged and coming
    // back. Returns the new output's name, or an empty string with `error` set.
    std::string createHeadlessOutput(const std::string& name, std::string* error);
    bool destroyOutput(const std::string& name, std::string* error);

    void removeOutput(Output* output);
    void reassignOutputViews(Output* source, Output* destination);
    void scheduleDisplacedViewRestore();
    void removeKeyboard(Keyboard* keyboard);
    void removeView(View* view);
    void removeLayerSurface(LayerSurface* layerSurface, wlr_output* output);
    void removeSessionLock(SessionLock* lock);
    void unlockSession();
    void raiseLockTree();
    void updateLockBlank();
    void updateBackdrop();
    // Continuations such as releases and repeats still reset the idle timer, but only a new press or motion may wake
    // outputs that were powered off through a DPMS action.
    void notifyIdleActivity();
    void notifyInputActivity();
    // Input wake applies only when every configured output is powered off. A named DPMS action therefore remains in
    // effect while another configured output is still awake.
    void wakeDpmsOutputs();
    void refocus(Output* preferred = nullptr) { m_focus.refocus(preferred); }
    void reconcileDynamicWorkspaces();
    void clearKeyboardFocus() { m_focus.clearKeyboardFocus(); }
    void deactivateViews(View* except = nullptr) { m_focus.deactivateViews(except); }
    [[nodiscard]] LayerSurface* exclusiveKeyboardLayer() const { return m_focus.exclusiveKeyboardLayer(); }
    // Lets a caller other than View's own close animation drive the snapshot with its own duration/curve/style.
    struct CloseSnapshotOverrides {
      int durationMs = 0;
      AnimationCurve curve{.easing = Easing::EaseOutCubic};
      std::string style = "fade";
    };
    void animateCloseSnapshot(
        Output* output, wlr_scene_tree* tree, std::vector<BorderSnapshot> borders,
        std::optional<CloseSnapshotOverrides> overrides = std::nullopt
    );

  private:
    static void onNewOutput(wl_listener* listener, void* data);
    static void onNewInput(wl_listener* listener, void* data);
    static void onNewXdgToplevel(wl_listener* listener, void* data);
    static void onNewXdgPopup(wl_listener* listener, void* data);
    static void onNewXdgDecoration(wl_listener* listener, void* data);
    static void onNewLayerSurface(wl_listener* listener, void* data);
    static void onNewSessionLock(wl_listener* listener, void* data);
    static void onNewPointerConstraint(wl_listener* listener, void* data);
    static void onNewVirtualKeyboard(wl_listener* listener, void* data);
    static void onNewVirtualPointer(wl_listener* listener, void* data);
    static void onVirtualPointerDestroy(wl_listener* listener, void* data);
    static void onNewIdleInhibitor(wl_listener* listener, void* data);
    static void onIdleInhibitorDestroy(wl_listener* listener, void* data);
    static void onNewActivationToken(wl_listener* listener, void* data);
    static void onActivationTokenDestroy(wl_listener* listener, void* data);
    static void onRequestActivate(wl_listener* listener, void* data);
    static void onWorkspaceCommit(wl_listener* listener, void* data);
    static void onSetGamma(wl_listener* listener, void* data);
    static void onPointerDestroy(wl_listener* listener, void* data);
    static void onTouchDestroy(wl_listener* listener, void* data);
    static void onTabletDestroy(wl_listener* listener, void* data);
    static void onTabletPadDestroy(wl_listener* listener, void* data);
    static void onTabletPadButton(wl_listener* listener, void* data);
    static void onTabletPadRing(wl_listener* listener, void* data);
    static void onTabletPadStrip(wl_listener* listener, void* data);
    static void onPadKeyboardFocusChange(wl_listener* listener, void* data);
    static void onOutputManagerApply(wl_listener* listener, void* data);
    static void onOutputManagerTest(wl_listener* listener, void* data);
    static void onOutputLayoutChange(wl_listener* listener, void* data);
    static void onToplevelCaptureRequest(wl_listener* listener, void* data);
    static void onRendererLost(wl_listener* listener, void* data);
    static int onBackgroundFrameTimer(void* data);
    static int onTerminateSignal(int signal, void* data);
    static void onIpcWindowsIdle(void* data);
    static void onDisplacedRestoreIdle(void* data);

    void addOutput(wlr_output* output);
    void addKeyboard(wlr_input_device* device);
    void addPointer(wlr_input_device* device);
    void addTouch(wlr_input_device* device);
    struct TabletDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wlr_tablet_v2_tablet* v2 = nullptr;
      wl_listener destroy{};
    };
    struct TabletPadDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wlr_tablet_v2_tablet_pad* v2 = nullptr;
      // The tablet this pad is paired with for enter/leave delivery; may be null.
      TabletDevice* tablet = nullptr;
      // Surface currently sent pad enter; the paired tablet's focused surface.
      wlr_surface* enteredSurface = nullptr;
      wl_listener destroy{};
      wl_listener button{};
      wl_listener ring{};
      wl_listener strip{};
    };
    void addTablet(wlr_input_device* device);
    void addTabletPad(wlr_input_device* device);
    void applyTabletConfig(TabletDevice& tablet);
    void applyTabletPadConfig(TabletPadDevice& pad);
    void pairTabletPads();
    // The output holding keyboard focus, or nullptr when focus is not on any
    // output (no surface, or an unmapped workspace).
    [[nodiscard]] Output* focusedOutput() const;
    void updateSeatCapabilities();
    void beginSessionLock(wlr_session_lock_v1* lock);
    void recreateRenderer();
    void applyConfig(const ConfigEffects& effects);
    void showConfigDiagnostics();
    void clearNormalFocus() { m_focus.clearNormalFocus(); }
    void setLockBlankEnabled(bool enabled);
    void handleWorkspaceCommit(void* data);
    [[nodiscard]] Workspace* workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const;
    void applyOutputManagerConfig(wlr_output_configuration_v1* config, bool testOnly);
    void restoreDisplacedViews();
    [[nodiscard]] WorkspaceGroup* workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const;

    struct IdleInhibitorWatch {
      Server* server = nullptr;
      wl_listener destroy{};
    };
    struct PointerDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wl_listener destroy{};
    };
    struct TouchDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wl_listener destroy{};
    };
    struct VirtualPointerDevice {
      Server* server = nullptr;
      wlr_virtual_pointer_v1* vpointer = nullptr;
      wl_listener destroy{};
    };
    struct ActivationTokenWatch {
      std::chrono::steady_clock::time_point createdAt;
      wl_listener destroy{};
    };

    wl_display* m_display = nullptr;
    wlr_backend* m_backend = nullptr;
    wlr_session* m_session = nullptr;
    wlr_renderer* m_renderer = nullptr;
    wlr_allocator* m_allocator = nullptr;
    wlr_compositor* m_compositor = nullptr;
    wlr_output_layout* m_outputLayout = nullptr;
    wlr_scene* m_scene = nullptr;
    wlr_color_manager_v1* m_colorManager = nullptr;
    std::unique_ptr<WineColorManager> m_wineColorManager;
    wlr_scene_output_layout* m_sceneLayout = nullptr;
    wlr_xdg_shell* m_xdgShell = nullptr;
    wlr_xdg_decoration_manager_v1* m_xdgDecorationManager = nullptr;
    wlr_server_decoration_manager* m_serverDecorationManager = nullptr;
    wlr_layer_shell_v1* m_layerShell = nullptr;
    wlr_foreign_toplevel_manager_v1* m_foreignToplevelManager = nullptr;
    wlr_ext_foreign_toplevel_list_v1* m_extForeignToplevelList = nullptr;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1* m_toplevelCaptureSourceManager = nullptr;
    wlr_export_dmabuf_manager_v1* m_exportDmabufManager = nullptr;
    wlr_tearing_control_manager_v1* m_tearingControlManager = nullptr;
    wlr_ext_workspace_manager_v1* m_workspaceManager = nullptr;
    wlr_session_lock_manager_v1* m_sessionLockManager = nullptr;
    wlr_pointer_constraints_v1* m_pointerConstraints = nullptr;
    wlr_relative_pointer_manager_v1* m_relativePointerManager = nullptr;
    wlr_pointer_gestures_v1* m_pointerGestures = nullptr;
    wlr_virtual_keyboard_manager_v1* m_virtualKeyboardManager = nullptr;
    wlr_virtual_pointer_manager_v1* m_virtualPointerManager = nullptr;
    wlr_tablet_manager_v2* m_tabletManager = nullptr;
    wlr_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
    wlr_idle_notifier_v1* m_idleNotifier = nullptr;
    wlr_xdg_activation_v1* m_xdgActivation = nullptr;
    wlr_gamma_control_manager_v1* m_gammaManager = nullptr;
    wlr_output_manager_v1* m_outputManager = nullptr;
    wlr_scene_tree* m_shellLayerTrees[kLayerCount]{};
    wlr_scene_tree* m_xdgTree = nullptr;
    wlr_scene_tree* m_scratchpadTree = nullptr;
    wlr_scene_tree* m_scratchpadShadowTree = nullptr;
    wlr_scene_tree* m_scratchpadContentTree = nullptr;
    wlr_scene_tree* m_overviewBlurTree = nullptr;
    wlr_scene_tree* m_overviewTree = nullptr;
    wlr_scene_tree* m_dragShadowTree = nullptr;
    wlr_scene_tree* m_dragTree = nullptr;
    wlr_scene_tree* m_dragIconTree = nullptr;
    wlr_scene_tree* m_fullscreenTree = nullptr;
    wlr_scene_tree* m_pinnedShadowTree = nullptr;
    wlr_scene_tree* m_pinnedTree = nullptr;
    wlr_scene_tree* m_imPopupTree = nullptr;
    wlr_scene_tree* m_lockTree = nullptr;
    wlr_scene_rect* m_lockBlank = nullptr;
    wlr_scene_rect* m_backdrop = nullptr;
    bool m_sessionLocked = false;
    std::vector<std::string> m_activeSubmaps;
    // Same-msec dedupe: several outputs can call tickAnimations per vblank.
    uint64_t m_lastAnimTickMsec = 0;

    // A fading copy of a closed window's scene tree. Owns that tree and destroys
    // it once the fade completes.
    class CloseSnapshot : public Animatable {
    public:
      CloseSnapshot(
          Server& server, Output* output, wlr_scene_tree* tree, std::vector<BorderSnapshot> borders, int durationMs,
          const AnimationCurve& curve, std::string_view style
      );
      ~CloseSnapshot() override;

      [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
      bool tickAnimations(uint64_t nowMsec) override;
      [[nodiscard]] bool hasActiveAnimations() const override { return m_alpha.animating() || m_posY.animating(); }
      [[nodiscard]] bool animatesOn(const Output* output) const override { return m_output == output; }

    private:
      Server* m_server = nullptr;
      wlr_scene_tree* m_tree = nullptr;
      Output* m_output = nullptr;
      AnimatedValue m_alpha;
      AnimatedValue m_posY;
      int m_origX = 0;
      int m_origY = 0;
      std::vector<std::pair<wlr_scene_buffer*, float>> m_buffers;
      std::vector<BorderSnapshot> m_borders;
    };
    // unique_ptr because the registry holds raw pointers to these: a vector of
    // values would move them out from under it on reallocation.
    std::vector<std::unique_ptr<CloseSnapshot>> m_closeSnapshots;
    std::vector<Animatable*> m_animatables;
    std::vector<Animatable*> m_animatablesScratch;

    std::unique_ptr<Seat> m_seat;
    std::unique_ptr<InputMethodRelay> m_inputMethodRelay;
    std::unique_ptr<Cursor> m_cursor;
    std::unique_ptr<Gestures> m_gestures;
    std::unique_ptr<SessionLock> m_sessionLock;
    std::unique_ptr<Overview> m_overview;
    std::unique_ptr<ScratchpadManager> m_scratchpadManager;
    std::unique_ptr<HintRect> m_insertHint;
    std::unique_ptr<ConfigWatcher> m_configWatcher;
    std::unique_ptr<Ipc> m_ipc;
    wlr_scene_tree* m_bannerTree = nullptr;
    std::unique_ptr<ConfigBanner> m_configBanner;
    wlr_scene_tree* m_cheatsheetTree = nullptr;
    std::unique_ptr<Cheatsheet> m_cheatsheet;
    wlr_scene_tree* m_quitConfirmTree = nullptr;
    std::unique_ptr<QuitConfirm> m_quitConfirm;

    bool m_nested = false;
    bool m_stopping = false;
    std::string m_socketName;

    std::unique_ptr<XwaylandSupervisor> m_xwayland;
    wl_event_source* m_backgroundFrameTimer = nullptr;
    // Non-null while a windows-event idle callback is pending. The idle source
    // removes itself when it runs, so a non-null pointer means "already queued".
    wl_event_source* m_ipcWindowsIdle = nullptr;
    wl_event_source* m_displacedRestoreIdle = nullptr;
    struct DisplacedWorkspaceSelection {
      std::string outputName;
      std::string workspaceName;
      size_t workspaceIndex = 0;
    };
    // Output objects do not survive physical hotplug, so selected workspaces
    // wait here until the output with the same stable name returns.
    std::vector<DisplacedWorkspaceSelection> m_displacedWorkspaceSelections;
    // Name to give the next output the backend hands over, set while createHeadlessOutput is adding one.
    std::string m_pendingOutputName;
    // SIGINT / SIGTERM, delivered on the event loop rather than in a signal
    // handler, so shutdown runs ordinary code instead of async-signal-safe code.
    wl_event_source* m_signalSources[2]{};

    wl_listener m_newOutput{};
    wl_listener m_newInput{};
    wl_listener m_newXdgToplevel{};
    wl_listener m_newXdgPopup{};
    wl_listener m_newXdgDecoration{};
    wl_listener m_newLayerSurface{};
    wl_listener m_newSessionLock{};
    wl_listener m_newPointerConstraint{};
    wl_listener m_newVirtualKeyboard{};
    wl_listener m_newVirtualPointer{};
    wl_listener m_newIdleInhibitor{};
    wl_listener m_newActivationToken{};
    wl_listener m_requestActivate{};
    wl_listener m_workspaceCommit{};
    wl_listener m_setGamma{};
    wl_listener m_outputManagerApply{};
    wl_listener m_outputManagerTest{};
    wl_listener m_outputLayoutChange{};
    wl_listener m_toplevelCaptureRequest{};
    wl_listener m_rendererLost{};
    wl_listener m_padKeyboardFocusChange{};

    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<std::unique_ptr<Keyboard>> m_keyboards;
    ModifierTapState m_modifierTap;
    std::vector<std::unique_ptr<PointerDevice>> m_pointers;
    std::vector<std::unique_ptr<TouchDevice>> m_touchDevices;
    std::vector<std::unique_ptr<TabletDevice>> m_tabletDevices;
    std::vector<std::unique_ptr<TabletPadDevice>> m_tabletPads;
    std::vector<std::unique_ptr<VirtualPointerDevice>> m_virtualPointers;
    Dirty m_dirty = Dirty::None;
    ViewRegistry m_registry;
    FocusManager m_focus{*this};
    std::vector<std::unique_ptr<LayerSurface>> m_layerSurfaces;
  };

} // namespace umbriel
