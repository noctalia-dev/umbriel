#pragma once

#include <array>
#include <cstdint>

namespace umbriel {

  // A dragged window deforms as an elastic sheet pinned at the grab point and
  // relaxes back to rest after release. A 4x4 grid of point masses (kPoints,
  // row-major) carries the deformation; the shader samples it with the same
  // bicubic Bernstein weights used to pin the grab point here.
  class DragPhysics {
  public:
    static constexpr int kPoints = 16;
    // The bound on any mass's speed on either axis, in logical pixels per second.
    static constexpr float kMaxVelocity = 4000.0F;
    // Each axis's displacement, divided by that axis's extent, changes by at most this times the larger (u, v)
    // step. The shader's inverse lookup is a contraction with this factor.
    static constexpr float kContraction = 0.7F;
    using Sheet = std::array<std::array<float, 2>, kPoints>;
    // Spring sheet tuning, read from `[animation.windows_drag]`; the defaults
    // are the values the simulation shipped with. Stiffness pulls each mass
    // home, damping bleeds energy (wobble decay), coupling ties neighbouring
    // masses (sheet rigidity), pointer response scales how far a motion loads
    // the sheet, max displacement caps its excursion, and settle-after-pause
    // snaps the sheet to rest when rendering pauses that long (a frame-stall
    // guard, never triggered by ordinary frames). kPoints, kContraction,
    // the 240 Hz step and the settled thresholds stay fixed: the shader and
    // the frame pacing own them.
    struct Tuning {
      float stiffness = 36.0F;
      float damping = 6.5F;
      float coupling = 100.0F;
      float pointerResponse = 2.0F;
      float maxDisplacementPx = 200.0F;
      double settleAfterPauseS = 0.25;
    };
    // `grabX`/`grabY` are the grab point as fractions of the window (0-1). From rest the sheet starts a new episode
    // with `transitionId`, which comes from the shared transition-id source (never 0). A sheet still settling keeps
    // its deformation, motion and transition id, and is pinned at the new grab point; either way it runs with
    // `tuning` from the next call on.
    void begin(float width, float height, float grabX, float grabY, uint64_t transitionId, Tuning tuning);
    // The shipped tuning. A separate overload, not a default argument: Tuning's
    // default member initializers cannot be used inside DragPhysics's own
    // declaration, only in function bodies.
    void begin(float width, float height, float grabX, float grabY, uint64_t transitionId) {
      begin(width, height, grabX, grabY, transitionId, Tuning{});
    }
    // Carries the sheet onto a resized window or a moved grab point: displacements keep their size relative to the
    // window, and the new grab point is pinned.
    void resize(float width, float height, float grabX, float grabY);
    // The grab point as fractions of the box the sheet spans, for a pointer at (localX, localY); the box and the
    // pointer share one coordinate space.
    [[nodiscard]] static std::array<float, 2>
    grabIn(float boxX, float boxY, float boxWidth, float boxHeight, double localX, double localY);
    // Pointer delta in logical pixels since the previous call.
    void move(float dx, float dy);
    void release();
    // Advances by `seconds` at a fixed 240 Hz step. A rendering pause over the
    // tuned settle threshold (and well beyond the output's frame cadence)
    // settles immediately. Returns active().
    bool tick(double seconds, double cadenceS);
    // Cadence unknown (no output to read): the tuned pause alone decides.
    bool tick(double seconds) { return tick(seconds, 0.0); }
    [[nodiscard]] bool active() const { return m_active; }
    [[nodiscard]] bool grabbed() const { return m_grabbed; }
    [[nodiscard]] uint64_t transitionId() const { return m_transitionId; }
    // Displacements divided by the window size, as the shader expects.
    [[nodiscard]] Sheet normalizedDisplacement() const;
    // Largest displacement in logical pixels.
    [[nodiscard]] float maxDisplacement() const;
    // Largest speed of any mass on either axis, in logical pixels per second.
    [[nodiscard]] float maxVelocity() const;
    // The displacement at (u, v) of the window (0-1) in logical pixels, interpolated as the shader does.
    [[nodiscard]] std::array<float, 2> displacementAt(float u, float v) const;
    // The largest displacement constrain() allows on either axis, in logical pixels.
    [[nodiscard]] float displacementBound() const;

  private:
    void setGrab(float grabX, float grabY);
    void constrain();
    Tuning m_tuning{};
    Sheet m_displacement{};
    Sheet m_velocity{};
    std::array<float, kPoints> m_weights{};
    std::array<float, kPoints> m_drag{};
    float m_width = 1.0F;
    float m_height = 1.0F;
    double m_remainder = 0.0;
    // The time the released sheet has actually spent below the settle velocity,
    // sampled per integration step inside tick().
    double m_slowS = 0.0;
    uint64_t m_transitionId = 0;
    bool m_grabbed = false;
    bool m_active = false;
  };

} // namespace umbriel
