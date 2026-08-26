#pragma once

#include "core/dirty.h"
#include "output/frame_schedule.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <wayland-server-core.h>

struct wlr_gamma_control_v1;
struct wlr_output;
struct wlr_output_layout_output;
struct wlr_scene_output;
struct wlr_scene_optimized_blur;
struct wlr_scene_tree;
struct wlr_surface;

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  enum class HdrMode;
  class Server;
  class View;
  class WorkspaceGroup;

  class Output {
  public:
    static constexpr uint32_t kLayerCount = 4;

    Output(Server& server, wlr_output* output);
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    [[nodiscard]] wlr_output* wlr() const { return m_output; }
    [[nodiscard]] wlr_scene_output* sceneOutput() const { return m_sceneOutput; }
    [[nodiscard]] wlr_scene_tree* layerTree(uint32_t layer) const;
    [[nodiscard]] wlr_scene_tree* popupTree() const { return m_popupTree; }
    // Clipped roots for this output's window content. Every descendant is scissored to the output's layout box, which
    // is what keeps a scrolled or animating view from rendering on, or entering, a neighbouring output. Positioned at
    // the layout origin, so root-local coordinates are layout coordinates.
    [[nodiscard]] wlr_scene_tree* viewRoot() const { return m_viewRoot; }
    [[nodiscard]] wlr_scene_tree* fullscreenRoot() const { return m_fullscreenRoot; }
    [[nodiscard]] wlr_scene_tree* pinnedRoot() const { return m_pinnedRoot; }
    [[nodiscard]] wlr_scene_tree* pinnedShadowRoot() const { return m_pinnedShadowRoot; }
    [[nodiscard]] wlr_box usableArea() const { return m_usableArea; }
    [[nodiscard]] WorkspaceGroup* workspaceGroup() const { return m_workspaceGroup.get(); }
    // True from the moment a view starts entering fullscreen until its client
    // has committed the exit. Consumers such as hot corners must not act over
    // fullscreen content during either transition.
    [[nodiscard]] bool hasFullscreenView(const View* ignored = nullptr) const;

    void arrangeLayers();
    // Record that something on this output became stale; flushed at the top of the next frame. Schedules that frame, so
    // recording is always enough on its own: marking work that nothing then asks for would simply never happen.
    void markDirty(Dirty what);
    void onGammaChanged(wlr_gamma_control_v1* control);
    void applyOutputState();
    // DPMS power is independent of configured enablement. A powered-off output
    // stays in the logical layout with its workspace and windows intact.
    [[nodiscard]] bool setPowered(bool powered);
    [[nodiscard]] bool dpmsOff() const { return m_dpmsOff; }
    [[nodiscard]] bool configuredEnabled() const;
    [[nodiscard]] HdrMode hdrMode() const;
    [[nodiscard]] bool hdrRequested() const;
    [[nodiscard]] bool hdrActive() const;
    [[nodiscard]] const std::string& hdrFallbackReason() const { return m_hdrFallbackReason; }
    [[nodiscard]] float configuredSdrWhite() const;
    [[nodiscard]] bool configuredTearingAllowed() const;
    [[nodiscard]] bool tearingRequested() const;
    [[nodiscard]] bool lastCommitTearing() const { return m_lastCommitTearing; }
    [[nodiscard]] const std::optional<uint32_t>& lastPresentationFlags() const { return m_lastPresentationFlags; }
    [[nodiscard]] const std::optional<bool>& lastPresentationPresented() const { return m_lastPresentationPresented; }
    [[nodiscard]] std::optional<bool> lastPresentationVsync() const;
    [[nodiscard]] const std::string& tearingFallbackReason() const { return m_tearingFallbackReason; }
    [[nodiscard]] bool clientTearingHintAsync(const View* view) const;
    [[nodiscard]] bool tearingEligible(View* view) const;
    [[nodiscard]] View* tearingCandidate() const;
    void resetTearingState();
    void applyCursorConfig();
    // Re-evaluate fullscreen-controlled VRR after a view or workspace changes.
    void updateVrr();
    // Re-evaluate automatic HDR after surface color, fullscreen, visibility,
    // or output membership changes.
    void updateHdr();
    void forgetHdrView(const View* view);
    void markBlurBackgroundDirty();
    void handleExternalConfigChange();
    // Tell one surface this output's scale (fractional + integer preferred buffer scale). Both wlroots calls dedup
    // internally, so re-notifying is free. Shaped as a wlr_surface_iterator_func_t so shell for_each helpers can walk a
    // whole surface tree with it; `data` is the Output*.
    static void notifySurfaceScaleIter(wlr_surface* surface, int sx, int sy, void* data);

  private:
    // Flushed at the top of handleFrame, in Dirty declaration order: layer arrange defines the usable area, which the
    // layout depends on, which the chrome over it depends on.
    void flushDirty();

    Dirty m_dirty = Dirty::None;

    static void onFrame(wl_listener* listener, void* data);
    static void onRequestState(wl_listener* listener, void* data);
    static void onPresent(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static int onFrameRetryTimer(void* data);

    void handleFrame();
    void handleRequestState(void* data);
    void handlePresent(void* data);
    void handleDestroy();
    void applyMode(int width, int height);
    [[nodiscard]] bool applyConfiguredState();
    [[nodiscard]] bool autoHdrEligible(const View* view) const;
    [[nodiscard]] View* findAutoHdrCandidate() const;
    [[nodiscard]] bool configuredVrrEnabled() const;
    void setHdrFallbackReason(std::string_view reason);
    void updateSceneSdrWhite();
    void rejectGammaControl(wlr_gamma_control_v1* control);
    void armFrameRetry();
    wlr_output_layout_output* addToLayout();
    void arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive);
    void updateOptimizedBlur(const wlr_box& fullArea);

    Server* m_server = nullptr;
    wlr_output* m_output = nullptr;
    wlr_scene_output* m_sceneOutput = nullptr;
    wlr_scene_tree* m_layerTrees[kLayerCount]{};
    wlr_scene_tree* m_popupTree = nullptr;
    wlr_scene_tree* m_viewRoot = nullptr;
    wlr_scene_tree* m_fullscreenRoot = nullptr;
    wlr_scene_tree* m_pinnedRoot = nullptr;
    wlr_scene_tree* m_pinnedShadowRoot = nullptr;
    wlr_scene_optimized_blur* m_optimizedBlur = nullptr;
    std::unique_ptr<WorkspaceGroup> m_workspaceGroup;
    wlr_box m_usableArea{};

    bool m_inFrame = false;
    bool m_hasDeferredMode = false;
    bool m_gammaDirty = false;
    bool m_softwareCursorLocked = false;
    bool m_animationRenderLocked = false;
    bool m_dpmsOff = false;
    bool m_hdrGammaWarningLogged = false;
    bool m_fullscreenHdrRequested = false;
    bool m_lastHdrRequested = false;
    bool m_lastCommitTearing = false;
    bool m_trackingPresentation = false;
    wl_event_source* m_frameRetryTimer = nullptr;
    View* m_autoHdrOwner = nullptr;
    std::string m_hdrFallbackReason;
    std::string m_tearingFallbackReason;
    std::optional<bool> m_lastPresentationPresented;
    std::optional<uint32_t> m_lastPresentationFlags;
    uint32_t m_trackedPresentationCommitSeq = 0;
    TearingCommitRecovery m_tearingRecovery;
    int m_deferredWidth = 0;
    int m_deferredHeight = 0;

    wl_listener m_frame{};
    wl_listener m_requestState{};
    wl_listener m_present{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
