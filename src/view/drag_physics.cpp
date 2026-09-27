#include "view/drag_physics.h"

#include <algorithm>
#include <cmath>

namespace umbriel {
  namespace {
    // Spring sheet tuning: stiffness pulls each mass home, coupling ties it to its neighbours, damping
    // bleeds energy, pointer response scales how far a motion loads the sheet.
    constexpr float kStiffness = 36.0F;
    constexpr float kCoupling = 100.0F;
    constexpr float kDamping = 6.5F;
    constexpr float kPointerResponse = 2.0F;
    constexpr double kStep = 1.0 / 240.0;
    constexpr double kSettleAfterPause = 0.25;
    constexpr float kMaxDisplacementPx = 200.0F;
    // Below these the sheet reads as at rest, and settling ends the frame stream.
    constexpr float kSettledDisplacement = 0.25F;
    constexpr float kSettledVelocity = 2.5F;
    // A pointer delta beyond this cannot come from real input; `move()` clamps to it so a single huge,
    // finite delta cannot overflow to inf before `constrain()` gets a chance to bound the result.
    constexpr float kMaxDelta = 1.0e6F;

    // Cubic Bernstein weights at t, the same weights the shader uses.
    std::array<float, 4> bernstein(float t) {
      const float s = 1 - t;
      return {s * s * s, 3 * t * s * s, 3 * t * t * s, t * t * t};
    }

    float largestComponent(const DragPhysics::Sheet& sheet) {
      float largest = 0;
      for (const auto& point : sheet) {
        largest = std::max({largest, std::abs(point[0]), std::abs(point[1])});
      }
      return largest;
    }
  } // namespace

  void DragPhysics::begin(float width, float height, float grabX, float grabY, uint64_t transitionId) {
    // Non-finite input leaves the sheet untouched; a 0x0 window is valid (floored to 1x1).
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(grabX) || !std::isfinite(grabY)) {
      return;
    }
    if (m_active) {
      m_grabbed = true;
      resize(width, height, grabX, grabY);
      return;
    }
    *this = DragPhysics{};
    m_width = std::max(width, 1.0F);
    m_height = std::max(height, 1.0F);
    m_transitionId = transitionId;
    m_grabbed = true;
    setGrab(grabX, grabY);
  }

  void DragPhysics::resize(float width, float height, float grabX, float grabY) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(grabX) || !std::isfinite(grabY)) {
      return;
    }
    width = std::max(width, 1.0F);
    height = std::max(height, 1.0F);
    const float scale[2] = {width / m_width, height / m_height};
    for (int i = 0; i < kPoints; ++i) {
      for (int axis = 0; axis < 2; ++axis) {
        m_displacement[i][axis] *= scale[axis];
        m_velocity[i][axis] *= scale[axis];
      }
    }
    m_width = width;
    m_height = height;
    setGrab(grabX, grabY);
    constrain();
  }

  std::array<float, 2>
  DragPhysics::grabIn(float boxX, float boxY, float boxWidth, float boxHeight, double localX, double localY) {
    return {
        static_cast<float>((localX - boxX) / std::max(boxWidth, 1.0F)),
        static_cast<float>((localY - boxY) / std::max(boxHeight, 1.0F))
    };
  }

  void DragPhysics::setGrab(float grabX, float grabY) {
    // One bicubic Bernstein surface couples the whole window; the shader
    // interpolates with the same weights, so the pin lands exactly on the pointer.
    const float u = std::clamp(grabX, 0.0F, 1.0F);
    const float v = std::clamp(grabY, 0.0F, 1.0F);
    const std::array<float, 4> horizontal = bernstein(u);
    const std::array<float, 4> vertical = bernstein(v);
    for (int i = 0; i < kPoints; ++i) {
      m_weights[i] = horizontal[i % 4] * vertical[i / 4];
    }
    // Pointer response falls off with distance from the grab, so a move loads the whole sheet; normalised
    // so the weighted sum at the grab point itself stays zero.
    const float x = u * 3, y = v * 3;
    float anchor = 0;
    for (int i = 0; i < kPoints; ++i) {
      const float dx = (static_cast<float>(i % 4) - x) / 3, dy = (static_cast<float>(i / 4) - y) / 3;
      m_drag[i] = std::exp(-(dx * dx + dy * dy) / 0.35F);
      anchor += m_weights[i] * m_drag[i];
    }
    for (int i = 0; i < kPoints; ++i) {
      m_drag[i] = 1 - m_drag[i] / anchor;
    }
  }

  void DragPhysics::constrain() {
    if (m_grabbed) {
      float sum = 0, position[2] = {0, 0}, velocity[2] = {0, 0};
      for (int i = 0; i < kPoints; ++i) {
        sum += m_weights[i] * m_weights[i];
        for (int axis = 0; axis < 2; ++axis) {
          position[axis] += m_weights[i] * m_displacement[i][axis];
          velocity[axis] += m_weights[i] * m_velocity[i][axis];
        }
      }
      // Pin the interpolated grab point exactly, even between grid vertices.
      for (int i = 0; i < kPoints; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
          m_displacement[i][axis] -= m_weights[i] * position[axis] / sum;
          m_velocity[i][axis] -= m_weights[i] * velocity[axis] / sum;
        }
      }
    }
    // Bound each axis's adjacent-mass slope so the shader's inverse lookup stays a contraction (no folding),
    // then its excursion and speed. The pin is linear in each axis, so a per-axis scale keeps it exact.
    float ratio[2] = {1, 1};
    for (int axis = 0; axis < 2; ++axis) {
      float horizontal = 0, vertical = 0;
      const float extent = axis == 0 ? m_width : m_height;
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const int i = y * 4 + x;
          if (x < 3)
            horizontal = std::max(horizontal, std::abs(m_displacement[i + 1][axis] - m_displacement[i][axis]));
          if (y < 3)
            vertical = std::max(vertical, std::abs(m_displacement[i + 4][axis] - m_displacement[i][axis]));
        }
      }
      ratio[axis] = std::max(ratio[axis], 3 * (horizontal + vertical) / (extent * kContraction));
      const float maxDisplacement = std::min(kMaxDisplacementPx, extent / 5);
      for (int i = 0; i < kPoints; ++i) {
        ratio[axis] = std::max(ratio[axis], std::abs(m_displacement[i][axis]) / maxDisplacement);
        ratio[axis] = std::max(ratio[axis], std::abs(m_velocity[i][axis]) / kMaxVelocity);
      }
    }
    for (int axis = 0; axis < 2; ++axis) {
      if (ratio[axis] > 1) {
        for (auto& point : m_displacement) {
          point[axis] /= ratio[axis];
        }
        for (auto& point : m_velocity) {
          point[axis] /= ratio[axis];
        }
      }
    }
  }

  void DragPhysics::move(float dx, float dy) {
    if (!m_grabbed || !std::isfinite(dx) || !std::isfinite(dy) || (dx == 0 && dy == 0)) {
      return;
    }
    dx = std::clamp(dx, -kMaxDelta, kMaxDelta);
    dy = std::clamp(dy, -kMaxDelta, kMaxDelta);
    for (int i = 0; i < kPoints; ++i) {
      m_displacement[i][0] -= kPointerResponse * dx * m_drag[i];
      m_displacement[i][1] -= kPointerResponse * dy * m_drag[i];
    }
    constrain();
    m_active = true;
  }

  void DragPhysics::release() { m_grabbed = false; }

  bool DragPhysics::tick(double seconds) {
    if (!m_active || !std::isfinite(seconds) || seconds <= 0) {
      return m_active;
    }
    if (seconds > kSettleAfterPause) {
      m_displacement = {};
      m_velocity = {};
      m_remainder = 0;
      m_active = false;
      return false;
    }
    m_remainder += seconds;
    while (m_remainder + 1e-12 >= kStep) {
      m_remainder -= kStep;
      Sheet acceleration{};
      for (int i = 0; i < kPoints; ++i) {
        const int neighbours[4] = {
            i % 4 > 0 ? i - 1 : -1, i % 4 < 3 ? i + 1 : -1, i >= 4 ? i - 4 : -1, i < 12 ? i + 4 : -1
        };
        for (int axis = 0; axis < 2; ++axis) {
          acceleration[i][axis] = -kStiffness * m_displacement[i][axis] - kDamping * m_velocity[i][axis];
          for (const int n : neighbours) {
            if (n >= 0) {
              acceleration[i][axis] += kCoupling * (m_displacement[n][axis] - m_displacement[i][axis]);
            }
          }
        }
      }
      for (int i = 0; i < kPoints; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
          m_velocity[i][axis] += acceleration[i][axis] * static_cast<float>(kStep);
          m_displacement[i][axis] += m_velocity[i][axis] * static_cast<float>(kStep);
        }
      }
      constrain();
    }
    m_active = false;
    for (int i = 0; i < kPoints; ++i) {
      for (int axis = 0; axis < 2; ++axis) {
        m_active |= std::abs(m_displacement[i][axis]) > kSettledDisplacement
            || std::abs(m_velocity[i][axis]) > kSettledVelocity;
      }
    }
    if (!m_active) {
      m_displacement = {};
      m_velocity = {};
      m_remainder = 0;
    }
    return m_active;
  }

  DragPhysics::Sheet DragPhysics::normalizedDisplacement() const {
    Sheet sheet{};
    for (int i = 0; i < kPoints; ++i) {
      sheet[i][0] = m_displacement[i][0] / m_width;
      sheet[i][1] = m_displacement[i][1] / m_height;
    }
    return sheet;
  }

  std::array<float, 2> DragPhysics::displacementAt(float u, float v) const {
    const std::array<float, 4> horizontal = bernstein(std::clamp(u, 0.0F, 1.0F));
    const std::array<float, 4> vertical = bernstein(std::clamp(v, 0.0F, 1.0F));
    std::array<float, 2> displacement{};
    for (int i = 0; i < kPoints; ++i) {
      const float weight = horizontal[i % 4] * vertical[i / 4];
      displacement[0] += weight * m_displacement[i][0];
      displacement[1] += weight * m_displacement[i][1];
    }
    return displacement;
  }

  float DragPhysics::displacementBound() const { return std::min(kMaxDisplacementPx, std::max(m_width, m_height) / 5); }

  float DragPhysics::maxDisplacement() const { return largestComponent(m_displacement); }

  float DragPhysics::maxVelocity() const { return largestComponent(m_velocity); }

} // namespace umbriel
