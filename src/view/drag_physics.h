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
    // `grabX`/`grabY` are the grab point as fractions of the window (0-1). From rest the sheet starts a new episode
    // with `transitionId`, which comes from the shared transition-id source (never 0). A sheet still settling keeps
    // its deformation, motion and transition id, and is pinned at the new grab point.
    void begin(float width, float height, float grabX, float grabY, uint64_t transitionId);
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
    // Advances by `seconds` at a fixed 240 Hz step. A pause over 250 ms settles immediately. Returns active().
    bool tick(double seconds);
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
    Sheet m_displacement{};
    Sheet m_velocity{};
    std::array<float, kPoints> m_weights{};
    std::array<float, kPoints> m_drag{};
    float m_width = 1.0F;
    float m_height = 1.0F;
    double m_remainder = 0.0;
    uint64_t m_transitionId = 0;
    bool m_grabbed = false;
    bool m_active = false;
  };

} // namespace umbriel
