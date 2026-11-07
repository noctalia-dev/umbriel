#pragma once
#include "scene/color.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace umbriel {

  class Output;

  // A fresh id for one animation transition, never 0 (reserved for "no transition") and never repeated
  // for the process lifetime. Shared by every transition source, including drag physics.
  uint64_t nextAnimationTransitionId();

  struct AnimationTransition {
    uint64_t id;
    std::array<float, 4> seed;
  };
  // Identity and random seed for native transitions, including directly controlled gestures.
  [[nodiscard]] AnimationTransition beginAnimationTransition();

  enum class Easing : uint8_t {
    Linear,
    EaseInSine,
    EaseOutSine,
    EaseInOutSine,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseInQuart,
    EaseOutQuart,
    EaseInOutQuart,
    EaseInQuint,
    EaseOutQuint,
    EaseInOutQuint,
    EaseInExpo,
    EaseOutExpo,
    EaseInOutExpo,
    EaseInCirc,
    EaseOutCirc,
    EaseInOutCirc,
    EaseInBack,
    EaseOutBack,
    EaseInOutBack,
    EaseInElastic,
    EaseOutElastic,
    EaseInOutElastic,
    EaseInBounce,
    EaseOutBounce,
    EaseInOutBounce,
    Snappy,
    CustomBezier,
    Spring
  };

  struct BezierCurve {
    double x1 = 0.05;
    double y1 = 0.9;
    double x2 = 0.1;
    double y2 = 1.05;
    bool operator==(const BezierCurve&) const = default;
  };

  struct SpringConfig {
    double damping = 0.75;
    double stiffness = 100.0;
    double mass = 1.0;
    bool operator==(const SpringConfig&) const = default;
  };

  // Where a minimizing window collapses to: its own centre, its top-left corner, or the bottom centre of the output's
  // usable area. Spelled out in config as "center", "top_left", and "bottom_center".
  enum class MinimizeAnchor : uint8_t {
    Center,
    TopLeft,
    BottomCenter,
  };

  struct AnimationCurve {
    Easing easing = Easing::EaseOutCubic;
    BezierCurve bezier{};
    SpringConfig spring{};
    bool operator==(const AnimationCurve&) const = default;
  };

  // Standalone math and evaluation functions
  double solveCubicBezier(double x1, double y1, double x2, double y2, double x);
  // Wall-clock length of a spring curve: the time a unit step from rest needs to settle within
  // kSpringSettleEpsilon of its target. Stiffness and mass set that timescale, damping only the shape, so a spring
  // curve owns its duration and the configured duration_ms does not apply to it. Clamped to the duration_ms range.
  int springDurationMs(const SpringConfig& config);
  double solveSpringPhysics(
      double from, double to, double velocity, double elapsedSec, const SpringConfig& config,
      double* outVelocity = nullptr
  );
  // Upper bound on the displacement a damped spring can reach from its current position and velocity. The bound is
  // expressed in the same units as current and target and follows from the spring's remaining mechanical energy.
  double springDisplacementBound(double current, double target, double velocity, const SpringConfig& config);
  double applyEasing(const AnimationCurve& curve, double progress);
  inline double evaluateCurve(const AnimationCurve& curve, double progress) { return applyEasing(curve, progress); }

  // Logical layout geometry must remain between its endpoints and advance in one direction. Curves that reverse or
  // overshoot are projected onto cumulative travel, so their timing remains visible for the full configured duration
  // without letting tiled boxes cross a protected boundary. Already bounded monotonic curves are evaluated directly.
  class MonotonicEasing {
  public:
    MonotonicEasing();
    explicit MonotonicEasing(const AnimationCurve& curve);

    void reset(const AnimationCurve& curve);
    double value(double linearProgress) const;

  private:
    static constexpr std::size_t kSampleCount = 512;

    AnimationCurve m_curve{.easing = Easing::Linear};
    std::array<double, kSampleCount + 1> m_progress{};
    bool m_direct = true;
  };

  // Registry for named animation curves (supporting custom named beziers and springs)
  class CurveRegistry {
  public:
    static void registerCurve(std::string_view name, const AnimationCurve& curve);
    static void registerBezier(std::string_view name, double x1, double y1, double x2, double y2);
    static void
    registerSpring(std::string_view name, double damping = 0.75, double stiffness = 100.0, double mass = 1.0);
    static std::optional<AnimationCurve> lookup(std::string_view name);
    static AnimationCurve
    get(std::string_view name, const AnimationCurve& fallback = AnimationCurve{.easing = Easing::EaseOutCubic});
    static bool has(std::string_view name);
    static bool unregisterCurve(std::string_view name);
    static void resetToDefaults();
    static std::optional<AnimationCurve> parse(std::string_view str);
  };

  // Owners tick in phase order, and the order is load-bearing. Finishing an overview animation calls Server::focusView,
  // which moves the focused view to the front of the view registry; views must therefore be done with their pass before
  // any overlay runs. Within a phase the order does not matter.
  enum class AnimationPhase : uint8_t {
    Views,
    Workspaces,
    Overlays,
  };

  // Anything the central frame tick advances. Registering with the Server is the only thing an owner has to do; the
  // three traversals (advance, is-anything-running, is-anything-running-for-this-output) all derive from this.
  class Animatable {
  public:
    Animatable() = default;
    virtual ~Animatable() = default;
    Animatable(const Animatable&) = delete;
    Animatable& operator=(const Animatable&) = delete;

    virtual AnimationPhase animationPhase() const = 0;
    // Advance to `nowMsec`; true while anything is still running.
    virtual bool tickAnimations(double nowMsec) = 0;
    virtual bool hasActiveAnimations() const = 0;
    // Whether `output` has to keep scheduling frames for this owner. An owner spanning every output (the overview)
    // answers true for all of them; one with no output yet answers false for all.
    virtual bool animatesOn(const Output* output) const = 0;
  };

  // Identifies a Server::CloseSnapshot for as long as it fades; 0 is never assigned.
  using CloseSnapshotId = uint64_t;
  constexpr CloseSnapshotId kInvalidCloseSnapshot = 0;

  // A single animatable scalar owned by the animated object. The owner ticks it from the central Server tick and reads
  // current() to drive its scene state. Retargeting mid-flight restarts the curve from the current value, so there is
  // no cancel bookkeeping and no snapping when a target changes.
  class AnimatedValue {
  public:
    AnimatedValue() = default;
    explicit AnimatedValue(double initialValue)
        : m_from(initialValue), m_target(initialValue), m_current(initialValue) {}

    // current = target = value; stops animating.
    void snap(double value);

    // Retargeting: always animates, even when `to` equals current(), so completion is always
    // observable via a final tick. Restarts smoothly from current with the full duration. A spring curve replaces
    // `durationMs` with its own springDurationMs(), because its parameters already define the timescale.
    void retarget(double to, int durationMs, Easing easing = Easing::EaseOutCubic);
    void retarget(double to, int durationMs, const AnimationCurve& curve);
    void retarget(double to, int durationMs, std::string_view curveName);
    void retargetBezier(double to, int durationMs, double x1, double y1, double x2, double y2);
    void retargetSpring(double to, double damping = 0.75, double stiffness = 100.0);
    // Physics spring: continues from the current value with `initialVelocity` in units per second and settles when
    // the solver reaches the target. Retargeting runs the same solver, but from rest over a precomputed duration.
    void settleSpring(double to, const SpringConfig& spring, double initialVelocity = 0.0);
    // Stop a physics spring once its remaining energy cannot leave the projected target's pixel rounding cell. Returns
    // true only when the spring was finished. The solver remains untouched while any visible motion is still possible.
    [[nodiscard]] bool finishSpringTail(double pixelsPerUnit);
    // Shift from, current and target by the same amount, keeping any motion intact. For a coordinate space that is
    // renumbered under a running animation.
    void translate(double delta);

    // Advances the value. Returns true when the value was animating at entry (i.e. the owner must apply current()). The
    // call that reaches the target returns true and leaves animating() false, so owners detect completion as (tick(now)
    // && !animating()).
    bool tick(double nowMsec);

    double current() const { return m_current; }
    double target() const { return m_target; }
    double from() const { return m_from; }
    bool animating() const { return m_animating; }
    const AnimationCurve& curve() const { return m_curve; }
    double velocity() const { return m_velocity; }
    double progress() const;
    uint64_t durationMs() const { return m_durationMsec; }
    uint64_t transitionId() const { return m_transitionId; }
    const std::array<float, 4>& shaderSeed() const { return m_shaderSeed; }

  private:
    double m_from = 0.0;
    double m_target = 0.0;
    double m_current = 0.0;
    double m_velocity = 0.0;
    double m_initialVelocity = 0.0;
    double m_progress = 1.0; // linear fraction as of the last tick(); 1.0 when never animating
    double m_startMsec = 0;  // 0 = clock starts on the first tick
    uint64_t m_durationMsec = 1;
    uint64_t m_transitionId = 0;
    std::array<float, 4> m_shaderSeed{};
    AnimationCurve m_curve{.easing = Easing::EaseOutCubic};
    bool m_animating = false;
    // Set by settleSpring(): tick() runs the physics solver instead of the duration curve.
    bool m_physics = false;
  };

  // Helper class for animating 4-channel float RGBA colors smoothly with arbitrary curves.
  // Perfect for animated border transitions and unfocused window dimming.
  class AnimatedColor {
  public:
    AnimatedColor() = default;
    explicit AnimatedColor(const std::array<float, 4>& initialColor)
        : m_from(initialColor), m_target(initialColor), m_current(initialColor), m_fromOkLab(srgbToOkLab(initialColor)),
          m_targetOkLab(m_fromOkLab) {}
    AnimatedColor(float r, float g, float b, float a = 1.0F) : AnimatedColor(std::array<float, 4>{r, g, b, a}) {}

    // Snap immediately to target color
    void snap(const std::array<float, 4>& color);
    void snap(float r, float g, float b, float a = 1.0F);

    // Retargeting
    void retarget(const std::array<float, 4>& to, int durationMs, Easing easing = Easing::EaseOutCubic);
    void retarget(const std::array<float, 4>& to, int durationMs, const AnimationCurve& curve);
    void retarget(const std::array<float, 4>& to, int durationMs, std::string_view curveName);
    void retarget(float r, float g, float b, float a, int durationMs, const AnimationCurve& curve = AnimationCurve{});
    void retargetBezier(const std::array<float, 4>& to, int durationMs, double x1, double y1, double x2, double y2);
    void retargetSpring(const std::array<float, 4>& to, double damping = 0.75, double stiffness = 100.0);

    // Advances the color animation
    bool tick(double nowMsec);

    const std::array<float, 4>& current() const { return m_current; }
    void current(float out[4]) const;

    const std::array<float, 4>& target() const { return m_target; }
    const std::array<float, 4>& from() const { return m_from; }
    bool animating() const { return m_animating; }
    const AnimationCurve& curve() const { return m_curve; }
    double progress() const;
    uint64_t durationMs() const { return m_durationMsec; }
    uint64_t transitionId() const { return m_transitionId; }
    const std::array<float, 4>& shaderSeed() const { return m_shaderSeed; }

    float r() const { return m_current[0]; }
    float g() const { return m_current[1]; }
    float b() const { return m_current[2]; }
    float a() const { return m_current[3]; }

  private:
    std::array<float, 4> m_from{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 4> m_target{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 4> m_current{0.0F, 0.0F, 0.0F, 1.0F};
    OkLab m_fromOkLab;
    OkLab m_targetOkLab;
    double m_progress = 1.0; // linear fraction as of the last tick(); 1.0 when never animating
    double m_startMsec = 0;
    uint64_t m_durationMsec = 1;
    uint64_t m_transitionId = 0;
    std::array<float, 4> m_shaderSeed{};
    AnimationCurve m_curve{.easing = Easing::EaseOutCubic};
    bool m_animating = false;
  };

} // namespace umbriel
