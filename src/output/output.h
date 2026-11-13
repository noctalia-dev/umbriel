#pragma once

#include "core/dirty.h"
#include "output/colorimetry.h"
#include "output/cursor_plane_pace.h"
#include "output/direct_scanout.h"
#include "output/frame_schedule.h"
#include "output/hdr_metadata.h"
#include "output/hdr_transition.h"
#include "scene/effect_audio.h"
#include "scene/effect_selection.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <wayland-server-core.h>

struct wlr_gamma_control_v1;
struct wlr_output;
struct wlr_output_state;
struct wlr_buffer;
struct wlr_output_layout_output;
struct wlr_scene_rect;
struct wlr_scene_output;
struct wlr_scene_optimized_blur;
struct wlr_scene_tree;
struct wlr_surface;

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  enum class FormatTier : uint8_t;
  enum class HdrMode;
  enum class SdrGamut;
  enum class SdrTransfer;
  class Server;
  class View;
  class WorkspaceGroup;
  struct OutputIdentity;
  struct OutputRule;

  class Output {
  public:
    static constexpr uint32_t kLayerCount = 4;

    Output(Server& server, wlr_output* output);
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    wlr_output* wlr() const { return m_output; }
    OutputIdentity identity() const;
    wlr_scene_output* sceneOutput() const { return m_sceneOutput; }
    // Create an opaque scene mask matching a pending configured state. The
    // caller owns the returned node and must keep it through the commit.
    wlr_scene_rect* createConfiguredFrameMask(const wlr_output_state& state) const;
    wlr_scene_tree* layerTree(uint32_t layer) const;
    // Popups stack above regular surfaces within the same shell layer.
    wlr_scene_tree* layerPopupTree(uint32_t layer) const;
    // Clipped roots for this output's window content. Every descendant is scissored to the output's layout box, which
    // is what keeps a scrolled or animating view from rendering on, or entering, a neighbouring output. Positioned at
    // the layout origin, so root-local coordinates are layout coordinates.
    wlr_scene_tree* viewRoot() const { return m_viewRoot; }
    wlr_scene_tree* fullscreenRoot() const { return m_fullscreenRoot; }
    wlr_scene_tree* pinnedRoot() const { return m_pinnedRoot; }
    // Full logical box at the live layout origin, or the last arranged origin while temporarily removed.
    wlr_box layoutBox() const;
    wlr_box usableArea() const;
    WorkspaceGroup* workspaceGroup() const { return m_workspaceGroup.get(); }
    // True from the moment any view starts entering fullscreen until its client
    // has committed the exit. Output-wide fullscreen policies such as HDR and
    // VRR must cover both transitions.
    bool hasFullscreenView(const View* ignored = nullptr) const;
    // Fullscreen stacking and top-layer keyboard priority share the same presentation state.
    bool fullscreenAboveTopLayer() const { return m_fullscreenAboveTopLayer; }

    void arrangeLayers();
    // Record that something on this output became stale; flushed at the top of the next frame. Schedules that frame, so
    // recording is always enough on its own: marking work that nothing then asks for would simply never happen.
    void markDirty(Dirty what);
    // Asks for a frame on behalf of persistent effects.
    void scheduleEffectFrame();
    void scheduleAudioFrame();
    // Pushes the effect capture policy and the screen and cursor presets to this output's scene. Detached while
    // effects are suspended; the instances are visible only while the output is enabled.
    void applyOutputEffects();
    EffectSlot& screenEffectSlot() { return m_screenEffectSlot; }
    const EffectSlot& screenEffectSlot() const { return m_screenEffectSlot; }
    // Captures are released only by a built frame: asks for one when the last frame kept effects out of a capture.
    void scheduleEffectCaptureRelease();
    // Drawn frames that advanced persistent effects' time.
    uint64_t effectFrames() const { return m_effectFrames; }
    // Seconds persistent effects on this output draw at. Advances on effect frames, follows the clock while nothing
    // here needs them, and holds the frozen instant while the clock is frozen.
    float effectSeconds() const { return m_effectSeconds; }
    // Effect instances on this output that need frames of their own.
    unsigned effectEligible() const;
    void onGammaChanged(wlr_gamma_control_v1* control);
    [[nodiscard]] bool applyOutputState();
    // Adopt a successfully committed wlr-output-management state in two
    // phases. Logical state changes first so callbacks cannot revive a
    // disabled output, then layout membership changes after transient UI has
    // been closed safely.
    void adoptOutputManagerEnabled(bool enabled);
    void applyOutputManagerLayout(int x, int y);
    // DPMS power is independent of logical enablement. A powered-off output
    // stays in the logical layout with its workspace and windows intact.
    [[nodiscard]] bool setPowered(bool powered);
    bool dpmsOff() const { return m_dpmsOff; }
    bool configuredEnabled() const;
    bool desktopEnabled() const { return m_desktopEnabled; }
    HdrMode hdrMode() const;
    bool hdrRequested() const;
    bool hdrActive() const;
    bool automaticHdrAvailable() const;
    bool bitDepthActive() const;
    const std::string& hdrFallbackReason() const { return m_hdrFallbackReason; }
    float configuredSdrWhite() const;
    float sdrWhite() const;
    SdrTransfer configuredSdrTransfer() const;
    SdrGamut configuredSdrGamut() const;
    // Configured characterization source: "none", "edid", or an ICC file path.
    std::string configuredColorProfile() const;
    // Resolved characterization source: "none" when nothing characterized the
    // output (including a configured file that fell back to an unusable EDID),
    // "edid", or the ICC file path that was applied.
    const std::string& resolvedColorProfile() const { return m_resolvedColorProfile; }
    // Active panel characterization (ICC > EDID), resolved independently of
    // sdr_gamut so diagnostics can report what was measured. Callers that put
    // it on the wire must gate on the rendering destination first — see
    // ColorManager::descriptionForOutput. Null when uncharacterized.
    const PanelColorimetry* panelColorimetry() const { return m_panelColorimetry ? &*m_panelColorimetry : nullptr; }
    // Raw ICC bytes behind get_information's icc_file advertising: the exact
    // file bytes for file characterizations, a synthesized matrix-shaper
    // profile for EDID ones, and nothing when inert or built without lcms2.
    const IccProfileStore& iccProfileStore() const { return m_iccProfile; }
    // The supplied transform frames must carry while SDR gamut mapping is
    // staged: the frame paths' sdrGamutStaged decision, exposed for frame
    // builders outside Output (the output-management test-frame path).
    wlr_color_transform* stagedSdrGamutTransform() const;
    // Content decode for a sampled panel curve, shared with the descriptions
    // that advertise it. Null when the curve is named or an exact power.
    const SharedColorTransform& panelDecodeTransform() const { return m_panelDecodeTransform; }
    void setRuntimeSdrWhite(float nits);
    int configuredBitDepth() const;
    const std::string& bitDepthFallbackReason() const { return m_bitDepthFallbackReason; }
    bool configuredDirectScanoutEnabled() const;
    bool configuredTearingAllowed() const;
    // Whether a workspace step wraps around the ends of this output's inventory.
    bool configuredCyclicWorkspaces() const;
    bool tearingRequested() const;
    bool lastCommitTearing() const { return m_lastCommitTearing; }
    const std::optional<uint32_t>& lastPresentationFlags() const { return m_lastPresentationFlags; }
    const std::optional<bool>& lastPresentationPresented() const { return m_lastPresentationPresented; }
    std::optional<bool> lastPresentationVsync() const;
    const std::string& tearingFallbackReason() const { return m_tearingFallbackReason; }
    // The latest scene buffer accepted by the backend, independently of the log's settling interval.
    const DirectScanoutState& directScanout() const { return m_directScanout.current(); }
    const DirectScanoutTracker& directScanoutTracker() const { return m_directScanout; }
    // Observe only the scene state submitted by this attempt. Commit events also cover successful child commits
    // when a grouped output transaction later fails on another backend.
    class SceneCommit {
    public:
      SceneCommit(Output& output, const wlr_output_state& state);
      ~SceneCommit();
      SceneCommit(const SceneCommit&) = delete;
      SceneCommit& operator=(const SceneCommit&) = delete;

    private:
      static void onCommit(wl_listener* listener, void* data);
      Output& m_output;
      wlr_buffer* m_buffer = nullptr;
      DirectScanoutState m_state;
      wl_listener m_commit{};
    };
    bool clientTearingHintAsync(const View* view) const;
    bool tearingEligible(View* view) const;
    View* tearingCandidate() const;
    void resetTearingState();
    void applyDirectScanoutConfig();
    void applyCursorConfig();
    // Compare the live hardware cursor against the snapshot and, when it
    // changed and a cursor-metadata consumer wants it, damage the box it left
    // and the box it entered, so the transition reaches a delivered frame.
    // Called once per frame, before the needs_frame test, so the frame already
    // running commits the damage. Single-threaded, non-blocking, bounded per
    // output.
    bool paceCursorPlaneTransition();
#ifdef UMBRIEL_HARNESS
    // Harness-only: synthesize the plane sample a headless backend cannot produce.
    void setSyntheticPlaneCursorForTest(
        double x, double y, bool visible, std::uintptr_t image, int width, int height, int hotspotX, int hotspotY
    );
#endif
    // True only for sessions needing separate cursor metadata
    bool needsCursorCapturePacing() const;
    // Re-evaluate fullscreen-controlled VRR after a view or workspace changes.
    void updateVrr();
    // Re-evaluate automatic HDR after surface color, fullscreen, visibility,
    // or output membership changes.
    void updateHdr();
    void forgetHdrView(const View* view);
    void markBlurBackgroundDirty();
    void scheduleFullFrame();
    // Commit this custom mode and arrange around it. `refreshMHz` paces frames; 0 keeps the backend's default.
    void applyMode(int width, int height, int refreshMHz = 0);
    void handleExternalConfigChange();
    // Tell one surface this output's scale (fractional + integer preferred buffer scale). Both wlroots calls dedup
    // internally, so re-notifying is free. Shaped as a wlr_surface_iterator_func_t so shell for_each helpers can walk a
    // whole surface tree with it; `data` is the Output*.
    static void notifySurfaceScaleIter(wlr_surface* surface, int sx, int sy, void* data);

  private:
    bool audioPending() const;
    // Flushed at the top of handleFrame, in Dirty declaration order: layer arrange defines the usable area, which the
    // layout depends on, which the chrome over it depends on.
    void flushDirty();

    Dirty m_dirty = Dirty::None;

    static void onFrame(wl_listener* listener, void* data);
    static void onRequestState(wl_listener* listener, void* data);
    static void onPresent(wl_listener* listener, void* data);
    static void onBackgroundUpdate(wl_listener* listener, void* data);
    static void onBackgroundPopupUpdate(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static int onFrameRetryTimer(void* data);
    static int onEffectFrameTimer(void* data);
    static int onDirectScanoutTimer(void* data);

    void handleFrame();
    void updateFullscreenStack();
    bool fullscreenCoversOutput() const;
    void handleRequestState(void* data);
    void handlePresent(void* data);
    void handleDestroy();
    // Full configuration stages geometry. Frame-bound HDR transitions reuse
    // the live geometry and carry a freshly rendered scene buffer.
    [[nodiscard]] bool applyConfiguredState(bool stageGeometry = true);
    bool autoHdrEligible(const View* view) const;
    View* findAutoHdrCandidate() const;
    bool configuredVrrEnabled() const;
    void setHdrFallbackReason(std::string_view reason);
    void setBitDepthFallbackReason(std::string_view reason);
    void updateSceneSdrWhite();
    void updateSceneHdrWhitePoint(wlr_gamma_control_v1* control);
    void armFrameRetry();
    DirectScanoutState builtDirectScanout() const;
    void recordDirectScanout(const DirectScanoutState& state, uint32_t commitSeq);
    void armEffectFrame(uint64_t nowMsec);
    // Render locks other than this output's animation lock.
    int externalRenderLocks() const;
    // A capture holding `captureLocks` must see frames without in-place effects.
    bool effectCapturePending(int captureLocks) const;
    bool captureExclusionPending(int captureLocks) const;
    void disarmEffectFrame();
    wlr_output_layout_output* addToLayout();
    void arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive);
    void updateOptimizedBlur(const wlr_box& fullArea);
    void updateHdrAvailability(bool activationRejected = false);
    // Characterize the panel from the configured source (ICC file > EDID) and
    // cache the SDR gamut transform. `force` re-reads the profile; HDR
    // transitions skip the re-read when the source is unchanged.
    void updateSdrGamutTransform(const OutputRule* rule, bool force);

    Server* m_server = nullptr;
    wlr_output* m_output = nullptr;
    float m_defaultScale = 1.0F;
    std::optional<HdrStaticMetadata> m_hdrStaticMetadata;
    std::optional<float> m_runtimeSdrWhite;
    // Cached SDR gamut transform, staged as the scene's supplied transform
    // while a characterization is active and sdr_gamut is "srgb".
    ColorTransformPtr m_sdrGamutTransform;
    // Cached panel characterization feeding protocol descriptions and
    // diagnostics. Populated for both sdr_gamut policies ("native" stages no
    // transform at all but the panel is still characterized).
    std::optional<PanelColorimetry> m_panelColorimetry;
    // Raw ICC bytes advertised alongside that characterization via
    // get_information's icc_file (see iccProfileStore()).
    IccProfileStore m_iccProfile;
    // Content decode for a sampled panel curve, shared with the descriptions
    // that advertise it and held by any scene buffer tagged with one.
    SharedColorTransform m_panelDecodeTransform;
    std::string m_sdrGamutSource;
    std::string m_resolvedColorProfile = "none";
    bool m_sdrGamutResolved = false;
    wlr_scene_output* m_sceneOutput = nullptr;
    wlr_scene_tree* m_layerTrees[kLayerCount]{};
    wlr_scene_tree* m_layerPopupTrees[kLayerCount]{};
    wlr_scene_tree* m_viewRoot = nullptr;
    wlr_scene_tree* m_fullscreenRoot = nullptr;
    wlr_scene_tree* m_pinnedRoot = nullptr;
    wlr_scene_optimized_blur* m_optimizedBlur = nullptr;
    std::unique_ptr<WorkspaceGroup> m_workspaceGroup;
    wlr_box m_localUsableArea{};
    wlr_box m_loggedUsableArea{};
    int m_arrangedLayoutX = 0;
    int m_arrangedLayoutY = 0;

    bool m_inFrame = false;
    bool m_handlingFrame = false;
    bool m_hasDeferredMode = false;
    bool m_gammaDirty = false;
    bool m_vrrPending = false; // a VRR change waiting for the next frame commit
    bool m_softwareCursorLocked = false;
    bool m_animationRenderLocked = false;
    bool m_fullscreenAboveTopLayer = false;
    CursorPlaneState m_lastCursorPlane{};
#ifdef UMBRIEL_HARNESS
    std::optional<CursorPlaneState> m_syntheticPlaneCursor;
#endif
    bool m_desktopEnabled = true;
    bool m_dpmsOff = false;
    bool m_modeFallbackWarned = false;
    std::optional<FormatTier> m_vrrDroppedTier;
    bool m_fullscreenHdrRequested = false;
    HdrTransition m_hdrTransition;
    // Retain an activation rejection until an explicit output reconfiguration
    // probes support again. Ordinary SDR frames must not re-advertise it.
    bool m_hdrAvailable = false;
    bool m_lastCommitTearing = false;
    bool m_trackingPresentation = false;
    // Cadence of the last presented frame, which places this output's animation samples on its vblanks. 0 while
    // unknown, including under adaptive sync, whose refresh varies.
    uint64_t m_lastPresentNs = 0;
    uint64_t m_refreshNs = 0;
    bool m_appliedConfiguredScale = false;
    wl_event_source* m_frameRetryTimer = nullptr;
    wl_event_source* m_effectFrameTimer = nullptr;
    wl_event_source* m_directScanoutTimer = nullptr;
    DirectScanoutTracker m_directScanout;
    uint64_t m_lastEffectFrameMsec = 0;
    bool m_effectFrameDue = false;
    bool m_effectFrameArmed = false;
    EffectSlot m_screenEffectSlot;
    EffectAudio m_effectAudio{}; // bands and availability latched at the last effect frame
    uint64_t m_effectFrames = 0;
    float m_effectSeconds = 0.0F;
    bool m_outputEffectsTimed = false; // a visible screen or cursor instance here reads umbriel_time
    char m_cursorEffectOwner{};        // ledger identity of the cursor slot; the screen slot uses `this`
    bool m_effectCaptureBuilt = false; // the last built frame had an effect capture pending
    View* m_autoHdrOwner = nullptr;
    std::string m_hdrFallbackReason;
    std::string m_tearingFallbackReason;
    std::string m_bitDepthFallbackReason;
    std::optional<bool> m_lastPresentationPresented;
    std::optional<uint32_t> m_lastPresentationFlags;
    uint32_t m_trackedPresentationCommitSeq = 0;
#ifdef TRACY_ENABLE
    const char* m_tracyIntervalPlot = nullptr;
    const char* m_tracyVblankPlot = nullptr;
    int64_t m_tracyLastPresentNs = 0;
    unsigned m_tracyLastPresentSeq = 0;
#endif
    TearingCommitRecovery m_tearingRecovery;
    int m_deferredWidth = 0;
    int m_deferredHeight = 0;
    int m_deferredRefresh = 0;

    wl_listener m_frame{};
    wl_listener m_requestState{};
    wl_listener m_present{};
    wl_listener m_backgroundUpdate{};
    wl_listener m_backgroundPopupUpdate{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
