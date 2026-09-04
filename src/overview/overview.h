#pragma once

#include "core/animation.h"
#include "layout/drop_target.h"
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
    bool handleFallbackKey(uint32_t keysym);
    // Clear pending badge input for directional focus while interactive. Configured
    // actions retain their regular handlers throughout the closing animation.
    bool handleKeybindAction(KeybindAction action);
    // Step the active workspace `delta` rows down the filmstrip on `output` (null: wherever the pointer is). Returns
    // false at either end. The wheel, the middle-button drag and the three-finger swipe arrive here: while the
    // overview is up the real trees are hidden, so there is nothing to slide and switching is a discrete step rather
    // than the animated transition it is outside.
    bool selectRelativeWorkspace(int delta, Output* output);
    [[nodiscard]] bool dragging() const { return m_dragCard != nullptr || m_middlePressed; }

  private:
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
      size_t row = 0; // workspace index inside the output's group
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
      double rowScroll = 0;
      double rowFrom = 0;
      double rowTo = 0;
    };

    // Row placement for one output at the current progress.
    struct RowMetrics {
      wlr_box outputBox{};
      double zoom = 1.0;
      int rowX = 0;
      int rowW = 0;
      int rowH = 0;
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

    [[nodiscard]] double zoom() const;
    [[nodiscard]] static bool rowMetrics(const OutputState& state, const Server& server, double zoom, RowMetrics& out);
    [[nodiscard]] static int rowTop(const RowMetrics& metrics, double rowScroll, size_t row);

    bool beginPresentation();
    void buildState();
    void populateCards(OutputState& state);
    Card* createCard(OutputState& state, View* view, size_t row);
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
    void layoutCard(Card& card, const RowMetrics& metrics, double rowScroll, const View* liveTarget);
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
    void startRowAnimation();
    void finishAnimation();
    void beginClose(View* focus);
    void teardown();
    void scheduleFrames() const;

    [[nodiscard]] Card* cardAt(double lx, double ly);
    [[nodiscard]] Workspace* rowAt(double lx, double ly, OutputState** outState, size_t* outRow, bool extendHorizontal);
    [[nodiscard]] WorkspaceGroup*
    workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox);
    [[nodiscard]] Workspace* preferredWorkspace() const;
    void clearMiddlePress();

    void beginDrag();
    void updateDrag(double lx, double ly);
    void endDrag(bool drop);
    void syncWorkspaceRows(OutputState& state, WorkspaceGroup& group);
    void showDropHint(const wlr_box& worldBox, const RowMetrics& metrics, double rowScroll, size_t row, Output* output);
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
    AnimatedValue m_rowAnim;
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
    double m_middleAccumY = 0;
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
