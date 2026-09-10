#pragma once

#include "config/config.h"
#include "core/animation.h"
#include "layout/drop_target.h"
#include "overview/navigation.h"
#include "scene/hint_rect.h"
#include "scene/surface_blur.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_buffer;
struct wlr_scene_border;
struct wlr_scene_blur;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_pointer;

namespace umbriel {

  enum class KeybindAction;
  class LayerSurface;
  class Output;
  class Server;
  class View;
  class Workspace;
  class WorkspaceGroup;

  // Zoomed-out view of every workspace on every output, arranged as one vertical filmstrip per output. Clients are
  // never reconfigured: the real window trees are hidden and re-rendered as "cards", per-surface scene buffers sharing
  // the client textures and scaled by the scene graph. The overview is also an editor (click to focus, middle-click to
  // close, drag to relocate), so it owns pointer and keyboard input while open.
  class Overview : public Animatable {
  public:
    explicit Overview(Server& server);
    ~Overview();

    Overview(const Overview&) = delete;
    Overview& operator=(const Overview&) = delete;

    // Open, opening, or zooming back in.
    [[nodiscard]] bool active() const { return m_active; }
    // Open and not already zooming back in: pointer/keyboard edits still apply.
    [[nodiscard]] bool interactive() const { return m_active && !m_closing; }

    void toggle();
    void open();
    // Zoom back into each output's active workspace, restoring normal focus.
    void close();
    // Activate `workspace` (no slide, the real trees are hidden), focus `focus`
    // so its column reveal shares the closing zoom, then restore keyboard focus
    // once the zoom lands. Null lets normal refocus choose the target.
    void closeToWorkspace(Workspace* workspace, View* focus);
    // Instant teardown with no animation (session lock, config reload, output loss).
    void forceClose();

    // 4-finger swipe. `progress` is pre-clamped by the caller.
    void gestureUpdate(double progress);
    void gestureEnd(bool commitOpen);

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    // Advances the zoom and workspace-row animations; returns true while either is still running.
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    // The overview zooms every output at once.
    [[nodiscard]] bool animatesOn(const Output* /*output*/) const override { return true; }

    void onViewMapped(View* view);
    void onViewUnmapped(View* view);
    void onViewPinnedChanged(View* view);
    void onViewWorkspaceChanged(View* view);
    void onWorkspaceActivated(WorkspaceGroup* group);
    void onWorkspaceArranged(Workspace* workspace);
    void onWorkspaceInventoryChanged(WorkspaceGroup* group);
    void onFocusChanged();
    // Coalesce card geometry refreshes from view resize animation ticks. Views
    // advance before overlays, so the overview consumes the latest presented
    // size once per frame regardless of how many views resized together.
    void onViewPresentationChanged(View* view);
    void onOutputRemoved(Output* output);
    // A background- or bottom-layer surface on `output` mapped, unmapped, or changed layer: the mirrored stack has
    // to catch up while the real bottom layer is hidden.
    void onDesktopLayerChanged(Output* output);

    // Input entry points; called from Cursor/Keyboard while active.
    bool handleButton(uint32_t button, bool pressed, double lx, double ly);
    void handleMotion(double lx, double ly);
    bool handleAxisNotch(bool vertical, double direction, double lx, double ly);
    // Swipe and finger-scroll input both arrive as content-direction deltas and share one navigation lifetime; the
    // source only selects the travel distances and decides who may end the gesture. Scroll samples are combined at
    // the pointer frame.
    void beginNavigation(wlr_pointer* pointer, NavigationSource source, double lx, double ly);
    void updateNavigation(double dx, double dy, uint32_t timeMsec);
    void endNavigation(bool cancelled, uint32_t timeMsec, NavigationSource source);
    // Drop an unfinished gesture whatever its source, leaving the filmstrip to settle on the active workspace.
    void cancelNavigation();
    void handleTouchpadAxis(wlr_pointer* pointer, bool vertical, double delta, uint32_t timeMsec, double lx, double ly);
    void handleTouchpadFrame();
    bool handleFallbackKey(uint32_t keysym);
    // Clear pending badge input for directional focus while interactive. Configured
    // actions retain their regular handlers throughout the closing animation.
    bool handleKeybindAction(KeybindAction action);
    // Step the active workspace `delta` rows down the filmstrip on `output` (null: wherever the pointer is). Returns
    // false at either end. The wheel and middle-button drag use discrete steps;
    // touchpad navigation moves the rows continuously and selects on release.
    bool selectRelativeWorkspace(int delta, Output* output);
    [[nodiscard]] bool dragging() const { return m_dragCard != nullptr || m_middlePressed; }

  private:
    static void onNavigationDeviceDestroyed(wl_listener* listener, void* data);
    [[nodiscard]] Workspace* navigationWorkspace() const;
    OverviewNavigation m_navigation;
    Output* m_navigationOutput = nullptr;
    Workspace* m_navigationWorkspace = nullptr;
    wlr_pointer* m_navigationPointer = nullptr;
    wl_listener m_navigationDeviceDestroy{};
    NavigationSource m_navigationSource = NavigationSource::Scroll;
    bool m_navigationHorizontalWorkspaces = false;
    bool m_navigationStarted = false;
    double m_navigationStart = 0;
    double m_navigationScale = 1;
    bool m_navigationCentered = false;
    // Finger-scroll deltas accumulated since the last pointer frame, and whether that axis reported a stop.
    double m_scrollDx = 0;
    double m_scrollDy = 0;
    bool m_scrollStopX = false;
    bool m_scrollStopY = false;
    uint32_t m_scrollTime = 0;
    struct Card;
    struct OutputState;

    struct CardSurface {
      Card* card = nullptr;
      wlr_surface* surface = nullptr;
      wlr_scene_buffer* sourceBuffer = nullptr;
      wlr_scene_buffer* buffer = nullptr;
      int sx = 0;
      int sy = 0;
      bool isRoot = false;
      wl_listener commit{};
      wl_listener destroy{};
      wl_listener outputSample{};
      wl_listener frameDone{};
    };

    struct Card {
      Overview* overview = nullptr;
      OutputState* owner = nullptr;
      View* view = nullptr;
      size_t workspaceIndex = 0; // workspace index inside the output's group
      wlr_scene_tree* tree = nullptr;
      wlr_scene_border* border = nullptr;
      SurfaceBlur blur;
      std::vector<std::unique_ptr<CardSurface>> surfaces;
      wlr_box box{}; // content box in layout coordinates
      wlr_scene_tree* badge = nullptr;
      wlr_scene_rect* badgeRect = nullptr;
      wlr_scene_buffer* badgeText = nullptr;
      int badgeWidth = 0;
      int badgeHeight = 0;
      std::array<float, 4> badgeBackground{};
      std::string shortcut;
      size_t shortcutMatched = 0;
    };

    struct ShortcutAssignment {
      View* view = nullptr;
      std::string label;
    };

    // One surface of the output's mirrored stack: a background- or bottom-layer surface copied into every workspace
    // preview. `tree` is the layer surface's own scene tree, re-resolved every layout, and the source of both the
    // mirror geometry and its color state.
    struct DesktopSurface {
      Overview* overview = nullptr;
      OutputState* state = nullptr;
      wlr_surface* surface = nullptr;
      wlr_scene_tree* tree = nullptr;
      wl_listener commit{};
      wl_listener destroy{};
    };

    // One mirrored surface inside one row. Every copy paces its client: the real bottom layer is hidden while the
    // overview is open, so these buffers are the only place its surfaces are sampled.
    struct DesktopMirror {
      DesktopSurface* source = nullptr;
      wlr_scene_buffer* buffer = nullptr;
      wl_listener outputSample{};
      wl_listener frameDone{};
    };

    // Resolved stack entry, ordered bottom to top.
    struct DesktopEntry {
      wlr_surface* surface = nullptr;
      wlr_scene_tree* tree = nullptr;
    };

    // One workspace row's backdrop: the flat fill, plus one mirror per stack surface drawn over it when
    // `[overview] workspace_wallpaper` is on. The fill is what an output with an empty stack shows.
    struct WorkspaceBackground {
      wlr_scene_tree* tree = nullptr;
      wlr_scene_rect* fill = nullptr;
      std::vector<std::unique_ptr<DesktopMirror>> mirrors;
    };

    struct OutputState {
      Output* output = nullptr;
      wlr_scene_tree* tree = nullptr;
      wlr_scene_blur* backgroundBlur = nullptr;
      wlr_scene_rect* backgroundTint = nullptr;
      std::vector<WorkspaceBackground> workspaceBackgrounds;
      std::vector<std::unique_ptr<Card>> cards;
      std::vector<std::unique_ptr<DesktopSurface>> desktop;
      // Filmstrip position in workspace rows: 1.5 sits halfway between rows 1 and 2. Wheel steps, keyboard moves and
      // touchpad releases all animate this one value; a gesture in flight snaps it to follow the fingers.
      AnimatedValue rowScroll;
      size_t activeWorkspaceIndex = 0;
    };

    // Workspace preview placement for one output at the current progress. Previews
    // step along the output's workspace axis; the other axis stays centered.
    struct PreviewMetrics {
      wlr_box outputBox{};
      // Output box minus the layer-shell exclusive zones. Panels of the top and
      // overlay layers draw over the overview, so on-output is not the same as
      // on-screen for anything the overview wants readable.
      wlr_box usableBox{};
      double zoom = 1.0;
      WorkspaceAxis axis = WorkspaceAxis::Vertical;
      int previewW = 0;
      int previewH = 0;
      double baseX = 0;
      double baseY = 0;
      double gap = 0;
    };

    static void onCardSurfaceCommit(wl_listener* listener, void* data);
    static void onCardSurfaceDestroy(wl_listener* listener, void* data);
    static void onCardBufferOutputSample(wl_listener* listener, void* data);
    static void onCardBufferFrameDone(wl_listener* listener, void* data);
    static void addCardSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void syncCardSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void onDesktopSurfaceCommit(wl_listener* listener, void* data);
    static void onDesktopSurfaceDestroy(wl_listener* listener, void* data);
    static void onDesktopMirrorOutputSample(wl_listener* listener, void* data);
    static void onDesktopMirrorFrameDone(wl_listener* listener, void* data);

    // Preview scale for the current open or close progress, and the scale the previews rest at once open. Gesture
    // travel maps onto the settled layout, so it must not depend on how far the zoom has come.
    [[nodiscard]] double zoom() const;
    [[nodiscard]] static double settledZoom();
    [[nodiscard]] static bool
    previewMetrics(const OutputState& state, const Server& server, double zoom, PreviewMetrics& out);
    // The workspace preview's box in layout coordinates.
    [[nodiscard]] static wlr_box
    previewBox(const PreviewMetrics& metrics, double workspaceScroll, size_t workspaceIndex);

    bool beginPresentation();
    void buildState();
    void populateCards(OutputState& state);
    Card* createCard(OutputState& state, View* view, size_t workspaceIndex);
    void snapshotCardForClose(Card& card);
    void destroyCard(Card* card);
    static void syncCardBuffer(CardSurface& entry);
    void dropCard(View* view);
    void rebuildCard(View* view);
    [[nodiscard]] OutputState* stateFor(const Output* output);
    [[nodiscard]] OutputState* stateForWorkspace(const Workspace* workspace);
    [[nodiscard]] Card* findCard(const View* view);
    [[nodiscard]] WorkspaceBackground createWorkspaceBackground(OutputState& state) const;
    // Mapped background- and bottom-layer surfaces of `output`, in render order.
    void collectDesktopSurfaces(const Output& output, std::vector<DesktopEntry>& out) const;
    // Re-resolves the output's mirrored stack, rebuilding every row's mirrors when its surfaces changed.
    void refreshDesktop(OutputState& state);
    void clearDesktop(OutputState& state) const;
    void createRowMirrors(OutputState& state, WorkspaceBackground& background) const;
    // Points every mirror at its surface's committed buffer and copies its color state.
    void syncDesktopMirrors(const OutputState& state) const;
    [[nodiscard]] static bool desktopSourceBox(const DesktopSurface& source, wlr_box& out);

    void applyProgress();
    void layoutOutput(OutputState& state);
    void layoutCard(Card& card, const PreviewMetrics& metrics, double workspaceScroll, const View* liveTarget);
    // The window a focus or close action would act on right now: the focused view of the active workspace on the
    // output holding the cursor. Null when that workspace is empty, which is also when those actions do nothing.
    [[nodiscard]] View* liveTargetView() const;
    [[nodiscard]] std::array<float, 4> cardBorderColor(const Card& card, const View* liveTarget) const;
    void assignShortcuts();
    void renderCardShortcut(Card& card);
    bool handleShortcutKey(uint32_t keysym);
    void refreshShortcutMatches();
    void clearShortcutInput();
    void updateShortcutAssignments();

    void startAnimation(double target, bool closing);
    // Move one output's filmstrip onto row `target`. `releaseVelocity` is the speed a touchpad gesture left behind,
    // in rows per second, and is zero for every other caller.
    void animateRow(OutputState& state, double target, double releaseVelocity = 0);
    void finishAnimation();
    void beginClose(View* focus);
    void teardown();
    void scheduleFrames() const;

    [[nodiscard]] Card* cardAt(double lx, double ly);
    [[nodiscard]] Workspace*
    workspaceAtPoint(double lx, double ly, OutputState** outState, size_t* outIndex, bool extendScrollingAxis);
    [[nodiscard]] WorkspaceGroup*
    workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox);
    [[nodiscard]] Workspace* preferredWorkspace() const;
    void clearMiddlePress();

    void beginDrag();
    void updateDrag(double lx, double ly);
    void endDrag(bool drop);
    void syncWorkspaceRows(OutputState& state, WorkspaceGroup& group);
    void showDropHint(
        const wlr_box& worldBox, const PreviewMetrics& metrics, double workspaceScroll, size_t workspaceIndex,
        Output* output
    );
    void showWorkspaceInsertHint(Output* output, const wlr_box& box);
    void hideDropHint();

    Server* m_server = nullptr;
    wlr_scene_tree* m_tree = nullptr; // Server::overviewTree()
    std::unique_ptr<HintRect> m_dropHint;
    std::vector<std::unique_ptr<OutputState>> m_outputs;

    bool m_active = false;
    bool m_closing = false;
    double m_progress = 0;
    double m_targetProgress = 0;
    double m_progressFrom = 0;
    AnimatedValue m_zoomAnim;
    View* m_pendingFocus = nullptr;
    bool m_cardPresentationDirty = false;
    bool m_gestureOpenedHere = false;
    bool m_shortcutsDirty = true;
    std::string m_shortcutInput;
    std::vector<ShortcutAssignment> m_shortcutAssignments;
    size_t m_shortcutLabelCapacity = 0;
    // Output under the pointer, which is the output the live target resolves against.
    Output* m_pointerOutput = nullptr;

    Card* m_pressCard = nullptr;
    Workspace* m_pressWorkspace = nullptr;
    double m_pressX = 0;
    double m_pressY = 0;
    Card* m_middlePressCard = nullptr;
    Output* m_middleOutput = nullptr;
    double m_middlePressX = 0;
    double m_middlePressY = 0;
    // Travel along the pressed output's workspace axis since the last step.
    double m_middleAccum = 0;
    bool m_middlePressed = false;
    bool m_middleDragging = false;

    Card* m_dragCard = nullptr;
    double m_dragOffsetX = 0;
    double m_dragOffsetY = 0;
    Workspace* m_dragSourceWorkspace = nullptr;
    int m_dragSourceColumn = -1;
    int m_dragSourceRow = -1;
    std::optional<DropColumnWidth> m_dragSourceWidth;
    DropTarget m_drop{};
    WorkspaceGroup* m_dropWorkspaceGroup = nullptr;
    size_t m_dropWorkspaceIndex = 0;
  };

} // namespace umbriel
