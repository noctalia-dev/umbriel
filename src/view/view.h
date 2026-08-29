#pragma once
#include "config/config.h"
#include "core/animation.h"
#include "scene/node.h"
#include "view/decoration.h"
#include "view/deferred_unfullscreen.h"
#include "view/floating.h"
#include "view/presentation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_image_capture_source_v1;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_output;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_surface;
struct wlr_xdg_popup;
struct wlr_xdg_toplevel;

namespace umbriel {

  class Server;
  class ScratchpadManager;
  class WineColorManager;
  class Workspace;
  struct ResolvedWindowRule;

  class View : public SceneNode, public Animatable {
  public:
    View(Server& server, wlr_xdg_toplevel* toplevel);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    [[nodiscard]] wlr_xdg_toplevel* toplevel() const { return m_toplevel; }
    [[nodiscard]] wlr_scene_tree* sceneTree() const { return m_sceneTree; }
    [[nodiscard]] wlr_scene_tree* captureTree() const;
    [[nodiscard]] bool mapped() const { return m_mapped; }
    [[nodiscard]] bool xwayland() const { return m_xwayland; }
    [[nodiscard]] Workspace* workspace() const { return m_workspace; }
    // The output currently presenting this view. Unassigned views follow the
    // preferred output until they are attached to a workspace.
    [[nodiscard]] Output* currentOutput() const;
    // Effective optional window-rule override used by tearing diagnostics.
    [[nodiscard]] std::optional<bool> tearingRuleOverride();
    [[nodiscard]] bool onActiveWorkspace() const { return m_onActiveWorkspace; }
    [[nodiscard]] bool tiled() const { return m_tiled; }
    [[nodiscard]] bool floating() const { return !m_tiled; }
    [[nodiscard]] bool pinned() const { return m_pinned; }
    // True while an unfullscreen configure with size 0x0 is unacknowledged;
    // Workspace::arrange must not impose the column size yet.
    [[nodiscard]] bool awaitingUnfullscreenSize() const { return m_pendingUnfullscreenSize; }
    [[nodiscard]] bool maximizedToEdges() const { return m_maximizedToEdges; }
    // Fullscreen for layout purposes: a view inside the unfullscreen grace keeps its fullscreen slot and presentation
    // so the strip does not reflow (and no resize leaks) while the client decides how to respond.
    [[nodiscard]] bool layoutFullscreen() const;
    [[nodiscard]] bool urgent() const { return m_urgent; }
    // The window id the ext-foreign-toplevel protocol hands to clients, which the IPC surface reuses verbatim for its
    // own window identity. Null only while no ext handle exists (the handle lives for the whole map lifetime).
    [[nodiscard]] const char* extForeignIdentifier() const {
      return m_extForeign != nullptr ? m_extForeign->identifier : nullptr;
    }
    // Seat-global activation, tracked independently of the per-workspace focus
    // state the IPC `focused` field reports.
    [[nodiscard]] bool activated() const { return m_activated; }
    [[nodiscard]] int presentedWidth(const wlr_box& target) const;
    [[nodiscard]] int presentedHeight(const wlr_box& target) const;
    // Canonical box currently presented by this view. The normal scene and overview cards both project this state, so
    // position and size transitions cannot diverge between the two render paths.
    [[nodiscard]] const wlr_box& presentedBox() const { return m_presentedBox; }
    [[nodiscard]] float presentedOpacity() const { return effectiveOpacity(); }
    [[nodiscard]] wlr_scene_tree* homeTree() const;
    // The toplevel view owning `surface` after walking xdg popup parents, or
    // nullptr when the surface is not under a view (layer surfaces, cursors).
    static View* fromSurface(wlr_surface* surface);

    // Mechanism only: applies seat keyboard, activation chrome, and raise. Policy lives in Server::focusView; do not
    // call from input/event code. `withKeyboard` is false while overview owns the seat: chrome and activation still
    // update, the keyboard enter is deferred to the close.
    void applySeatFocus(bool withKeyboard = true);
    void setForeignActivated(bool activated);
    void setUrgent(bool urgent);
    // Focus ring only. Public alongside setForeignActivated because both are
    // activation chrome the focus manager drives from outside.
    void setBorderFocused(bool focused);
    void setWorkspace(Workspace* workspace, bool attachToLayout = true);
    // A move the user asked for: the view belongs where it lands, and any displaced home is dropped.
    void moveToWorkspace(Workspace* workspace, bool attachToLayout = true);
    void detachWorkspace();

    // The output, workspace, and layout member to restore after output loss.
    // A still-live refuge output records its native members before displaced
    // windows enter, so its own layout cannot be polluted by the evacuation.
    // The snapshot itself never owns a View pointer.
    struct DisplacedHome {
      std::string outputName;
      std::string workspaceName;
      std::shared_ptr<const LayoutSnapshot> layoutSnapshot;
      LayoutMemberId layoutMember = 0;
      std::optional<LayoutMode> layoutModeOverride;
      // Position relative to the full logical output. Unlike the ordinary
      // usable-area memory, this stays stable while a returning panel has not
      // recreated its exclusive zone yet.
      std::optional<std::array<double, 2>> floatingOutputPosition;
      uint64_t configGeneration = 0;
      // True while this view is still at home and only carries a clean
      // snapshot taken before foreign refugees entered its layout.
      bool layoutProtectionOnly = false;
    };
    [[nodiscard]] const std::optional<DisplacedHome>& displacedHome() const { return m_displacedHome; }
    void markDisplaced(DisplacedHome home) { m_displacedHome = std::move(home); }
    void clearDisplaced() { m_displacedHome.reset(); }

    void setOnActiveWorkspace(bool active);
    void setScratchpadBorder(bool scratchpad);
    void animateTo(int x, int y);
    void setPosition(int x, int y);
    // The authoritative layout position: where the window's slot is, not where its scene node happens to be
    // mid-animation. Workspace slides and arrange reflows move nodes without touching the animation targets, so window
    // listings that order by position must read these instead.
    [[nodiscard]] int layoutTargetX() const { return static_cast<int>(std::lround(m_posX.target())); }
    [[nodiscard]] int layoutTargetY() const { return static_cast<int>(std::lround(m_posY.target())); }
    // Move the scene nodes without touching the position animation: an
    // interactive drag tracks the pointer 1:1 and owns the position itself.
    void setDragPosition(int x, int y);
    // Keep at least clamp(size / 4, 10, 75) pixels per axis on-screen.
    void clampFloatingPosition();
    // Record the floating position as a fraction of the current usable area,
    // so a cross-output move can land the window proportionally. No-op when tiled.
    void rememberFloatingPosition();
    // Re-anchor into the (new) workspace output's usable area. Temporary fallback placement can preserve the saved
    // fraction so a displaced view still remembers its real home geometry. No-op when tiled.
    void restoreFloatingPosition(bool rememberRestored = true);
    // Center the floating window on its output's usable area. False when not floating.
    bool centerFloating();
    // Animate the presented size toward a layout-assigned size. Called by Workspace::arrange when it configures the
    // client, so the animation owns the presented size before the clip can report the final size.
    void beginResizeAnimation(int width, int height, bool allowFullscreen = false);
    // Apply the presentation state derived from the view's layout box `target`: fullscreen backdrop, presented size and
    // surface crop, borders, shadow and blur. Containment on the view's own output is the job of the output's clipped
    // scene roots, so nothing here depends on where the output edges are.
    void applyPresentation(const wlr_box& target);
    // Re-derive the surface clip from committed state alone (tile geometry when tiled, none otherwise), for the paths
    // that leave presentation-driven cropping behind: map, drag entry, drag drop.
    void resetSurfaceClip();
    void setFadeAlpha(float alpha);
    void cancelFadeAnimation();
    void cancelPositionAnimation();
    void snapPosition(int x, int y);
    void animateFadeTo(float toAlpha, int durationMs, const AnimationCurve& curve);
    [[nodiscard]] const ViewPresentation& presentation() const { return m_presentation; }
    // Size/position to the full output and drop tile clips (exclusive zones do not apply).
    void applyFullscreenLayout(bool animate = false);
    // Compositor-driven fullscreen toggle (keybind); client requests use handleRequestFullscreen.
    void toggleFullscreen();
    void applyDeferredUnfullscreen();
    void setMaximizedToEdges(bool maximized);
    void toggleMaximizedToEdges();
    // Detach from the scrolling layout (float) or re-insert as a tiled column.
    void setFloating(bool floating, bool focus = true);
    void toggleFloating();
    void togglePinned();
    // Restore the global pinned scene layer after temporary drag reparenting.
    void restorePinnedSceneParent();
    // Enable/disable the view's scene tree and its shadow container together.
    void setNodeEnabled(bool enabled);
    void raiseToTop();
    // Create or destroy the shadow container in the given workspace shadow layer.
    void reparentShadow(wlr_scene_tree* shadowLayer);
    // Advances this view's animations; returns true while any is still running.
    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Views; }
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;

  private:
    friend class Cursor;
    friend class ScratchpadManager;
    friend class WineColorManager;
    friend class Output;
    friend class Server;
    friend class Popup;
    friend class Overview;

    struct OpacitySurfaceWatch {
      View* view = nullptr;
      wlr_surface* surface = nullptr;
      wl_listener commit{};
      wl_listener newSubsurface{};
      wl_listener destroy{};
    };

    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onCommit(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onRequestMove(wl_listener* listener, void* data);
    static void onRequestResize(wl_listener* listener, void* data);
    static void onRequestMaximize(wl_listener* listener, void* data);
    static void onAcceptClientMaximizeRequests(void* data);
    static void onRequestFullscreen(wl_listener* listener, void* data);
    static void onSetTitle(wl_listener* listener, void* data);
    static void onSetAppId(wl_listener* listener, void* data);
    static void onForeignActivate(wl_listener* listener, void* data);
    static void onForeignClose(wl_listener* listener, void* data);
    static void onForeignDestroy(wl_listener* listener, void* data);
    static void onExtForeignDestroy(wl_listener* listener, void* data);
    static void onOpacitySurfaceCommit(wl_listener* listener, void* data);
    static void onOpacitySurfaceNewSubsurface(wl_listener* listener, void* data);
    static void onOpacitySurfaceDestroy(wl_listener* listener, void* data);

    static void onCaptureSourceDestroy(wl_listener* listener, void* data);
    void handleMap();
    void handleUnmap();
    void handleCommit(bool reconfigureOpeningState = false);
    void handleDestroy();
    void handleRequestMove();
    void handleRequestResize(void* data);
    void handleRequestMaximize();
    void setMaximized(bool maximized);
    void handleRequestFullscreen();
    void setFullscreen(bool fullscreen);
    void handleSetTitle();
    void handleSetAppId();
    void handleForeignActivate();
    void handleForeignClose();
    void handleForeignDestroy();
    void handleExtForeignDestroy();
    void handleCaptureSourceDestroy();
    void updateBorderGeometry();
    void updateBorderGeometry(int contentWidth, int contentHeight);
    void applyCornerRadius();
    void reloadBackdropColor() { m_presentation.reloadBackdropColor(); }
    void refreshConfigChrome();
    void updateBlur();
    void updateBlur(int contentWidth, int contentHeight);
    [[nodiscard]] SurfaceBlurOptions blurOptions() const { return m_decoration.blurOptions(); }
    [[nodiscard]] SurfaceBlurOptions popupBlurOptions() const { return m_decoration.popupBlurOptions(); }
    void updateShadow();
    void updateShadow(int contentWidth, int contentHeight);
    // Show the borders and refresh everything derived from them. The four steps are always wanted together: a ring that
    // exists but was never given a geometry draws at 0x0, and the surface radius and effects follow the ring.
    void showDecorations(bool enabled);
    // Record the dimensions currently rendered by the scene. Client geometry can lag a layout configure, so
    // presentation consumers must not infer their size independently from the committed geometry.
    void trackPresentedSize(int width, int height);
    // Re-apply compositor-owned opacity to surface buffers. Fullscreen bypasses window-rule opacity, while fades,
    // drag opacity, focus dimming, and client-provided alpha remain active.
    [[nodiscard]] float effectiveOpacity() const;
    void applyEffectiveOpacity();
    void flushPendingEffectiveOpacity();
    void watchOpacitySurfaceTree(wlr_surface* root);
    void watchOpacitySurface(wlr_surface* surface);
    void clearOpacitySurfaceWatches();
    void beginCloseAnimation();
    void applyPresentedSize();
    // Scale-then-crop presentation of the primary buffer during a size
    // animation; surfaceClip is the visible presented region in surface coords.
    void applyPresentedCrop(const wlr_box& content, const wlr_box& surfaceClip);
    // Undo applyPresentedCrop/size-anim buffer state when the animation ends.
    void resetPresentedSurface();
    // Shared tail of a finished/cancelled size animation: settle the presented
    // size on the committed geometry and refresh the derived chrome.
    void finishSizeAnimation();
    [[nodiscard]] bool sizeAnimating() const { return m_presentation.animating(); }
    // True while the border ring exists and is showing. Fullscreen keeps the
    // tree but disables it, so the pointer alone does not answer this.
    [[nodiscard]] bool decorated() const;
    // Border thickness actually being drawn, 0 when undecorated.
    [[nodiscard]] int borderInset() const;
    // Radius to round the surface itself by: a fullscreen window is square even
    // though its borders are only hidden, not destroyed.
    [[nodiscard]] int surfaceRadius() const;
    // True while an interactive grab owns this view's size, so the layout must
    // not animate it (the drag tracks the pointer 1:1).
    [[nodiscard]] bool sizeGrabActive() const;
    // Cursor owns when a drag starts and ends; View owns the scene invariants
    // for the temporary global presentation and its resting presentation.
    void enterDragPresentation();
    void restoreHomePresentation();
    // Kick the owning output so an animation started outside a frame gets ticked.
    void scheduleFrame();
    void cancelSizeAnimation();
    void updateFullscreenPresentation(int width, int height);
    // Apply subsurface clip to the toplevel surface only, not xdg popup children.
    void setSurfaceTreeClip(const wlr_box* clip);
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Push the current output's scale to every surface of this toplevel (fractional-scale + preferred buffer scale,
    // popups included). Clients like xwayland-satellite size and map pointer coordinates by this, so it must follow the
    // view across outputs and track scale changes.
    void notifyOutputScale();
    // Keep floats visually at the last requested size while client geometry lags.
    void syncFloatingSurfaceClip();
    void requestFloatingSize(int width, int height);
    void beginFloatingResize(uint32_t edges);
    void resizeFloating(int width, int height);
    void finishFloatingResize();
    void syncFloatingResizePosition();
    void adoptFloatingClientSize();
    [[nodiscard]] wlr_box floatingUsableArea() const;
    void placeInUsableArea(const std::optional<WindowPosition>& position = std::nullopt);
    void setPinned(bool pinned, bool focus);
    void updateForeignIdentity();
    void updateForeignState();
    void enterForeignOutput();
    void enterForeignOutput(Output* output);
    void leaveForeignOutput();
    void applyWindowRules(const ResolvedWindowRule& initiallyApplied);
    bool attachToAvailableWorkspace(const ResolvedWindowRule& rule);
    // `resolved` lets a caller that already resolved the rules pass them in. Rule resolution runs every regex in the
    // config, and applyDynamicRules is reached on focus changes and on every title change, so resolving twice per pass
    // is work a terminal that retitles per command pays repeatedly.
    void applyDynamicRules(const ResolvedWindowRule* resolved = nullptr);
    // Window rules, resolved at most once per (config, app-id, title, focus). Resolution runs every rule's regexes, and
    // it is reached on focus changes and on every title change; a terminal that retitles per command would otherwise
    // pay the whole rule set on each one. All four inputs are part of the key: `match.is_focused` makes focus a
    // matching criterion, not just a consumer of the result.
    [[nodiscard]] const ResolvedWindowRule& resolvedRules();

    // Cache for resolvedRules(); m_rulesGeneration 0 means never resolved.
    ResolvedWindowRule m_rules;
    uint64_t m_rulesGeneration = 0;
    std::string m_rulesAppId;
    std::string m_rulesTitle;
    bool m_rulesFocused = false;
    // One-shot effects already applied at map. Late identity resolution only
    // reapplies a field when its resolved value changes.
    ResolvedWindowRule m_initialRules;

    Server* m_server = nullptr;
    wlr_xdg_toplevel* m_toplevel = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;
    // A separate scene containing only client-owned surfaces. Window capture
    // must never sample the composited desktop behind translucent content.
    wlr_scene* m_captureScene = nullptr;
    ViewDecoration m_decoration;
    ViewPresentation m_presentation;
    wlr_box m_presentedBox{};
    wlr_foreign_toplevel_handle_v1* m_foreign = nullptr;
    wlr_ext_foreign_toplevel_handle_v1* m_extForeign = nullptr;
    wlr_output* m_foreignOutput = nullptr;
    wlr_ext_image_capture_source_v1* m_captureSource = nullptr;
    Workspace* m_workspace = nullptr;
    std::optional<DisplacedHome> m_displacedHome;

    bool m_mapped = false;
    // Saved client state commonly requests maximization while the surface is
    // opening. Layout policy owns that transition; later requests are valid.
    bool m_acceptClientMaximizeRequests = false;
    wl_event_source* m_acceptClientMaximizeIdle = nullptr;
    bool m_xwayland = false;
    // False until the first setPosition/animateTo places the node; the initial
    // placement snaps (avoids animating from the default (0,0) world origin).
    bool m_positioned = false;
    bool m_tiled = false;
    bool m_maximizedToEdges = false;
    bool m_restoreMaximizedToEdges = false;
    wlr_box m_fullscreenRestoreBox{};
    bool m_hasFullscreenRestoreBox = false;
    bool m_pinned = false;
    bool m_restoreTiledAfterUnpin = false;
    // Set when a float toggle drops fullscreen: re-tiling restores fullscreen BEFORE the layout attach, so the client
    // never receives a transient column-sized configure (game engines latch it for input mapping and go dead outside
    // it). Cleared whenever fullscreen is left by any other path, so a client that chose windowed mode while floating
    // re-tiles as a regular column.
    bool m_refullscreenOnTile = false;
    // Set while an unfullscreen configure with size 0x0 is in flight: the layout must not impose the column size until
    // the client commits its non-fullscreen state (or re-requests fullscreen, avoiding any resize).
    bool m_pendingUnfullscreenSize = false;
    // 0 until the first frame tick after arming; the grace deadline counts
    // from there so a stalled frame clock cannot expire it instantly.
    uint64_t m_unfullscreenGraceStartMsec = 0;
    // Inactive client unfullscreen requests wait briefly for xdg or foreign activation. Any later client request or
    // compositor-driven fullscreen change clears the parked request.
    DeferredUnfullscreen m_deferredUnfullscreen;
    // Geometry at unfullscreen time; a commit with a different geometry means
    // the client accepted windowed mode and the grace can end early.
    wlr_box m_unfullscreenGeometry{};
    bool m_onActiveWorkspace = false;
    bool m_scratchpadBorder = false;
    bool m_urgent = false;
    bool m_activated = false;
    AnimatedValue m_posX;
    AnimatedValue m_posY;
    AnimatedValue m_fade;
    AnimatedColor m_borderColorAnim;
    AnimatedValue m_focusDim{1.0};
    float m_fadeAlpha = 1.0F;
    bool m_borderFocusedState = false;
    bool m_focusDimInitialized = false;
    // Window rules: unsettled means title was empty at map, so a later
    // handleSetTitle re-applies all rule effects one more time.
    bool m_initialRulesSettled = false;
    float m_ruleOpacity = 1.0F;
    float m_dragOpacity = 1.0F;
    // wlroots restores a committed scene buffer to the client-provided alpha. Root and subsurface watches set this so
    // compositor-managed opacity is restored on the frame after every scene helper commit listener has run.
    bool m_effectiveOpacityCommitPending = false;
    std::vector<std::unique_ptr<OpacitySurfaceWatch>> m_opacitySurfaceWatches;
    bool m_hasMaximizeRestoreBox = false;
    wlr_box m_maximizeRestoreBox{};
    FloatingGeometry m_floating;

    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_commit{};
    wl_listener m_destroy{};
    wl_listener m_requestMove{};
    wl_listener m_requestResize{};
    wl_listener m_requestMaximize{};
    wl_listener m_requestFullscreen{};
    wl_listener m_setTitle{};
    wl_listener m_setAppId{};
    wl_listener m_foreignActivate{};
    wl_listener m_foreignClose{};
    wl_listener m_foreignDestroy{};
    wl_listener m_extForeignDestroy{};
    wl_listener m_captureSourceDestroy{};
  };

} // namespace umbriel
