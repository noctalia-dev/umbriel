#include "core/animation.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <sys/random.h>
#include <unistd.h>
#include <unordered_map>

namespace umbriel {

  namespace {
    constexpr double kPi = std::numbers::pi;
    // Energy the unit step may keep and still count as settled, in target units. Matches the residual
    // solveSpringPhysics itself treats as arrived.
    constexpr double kSpringSettleEpsilon = 1e-4;
    // A spring's derived duration stays inside the duration_ms range a configured event accepts.
    constexpr int kSpringMinDurationMs = 1;
    constexpr int kSpringMaxDurationMs = 10000;

    struct AnimationTransition {
      uint64_t id;
      std::array<float, 4> seed;
    };

    [[nodiscard]] uint64_t mixRandom(uint64_t value) {
      value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
      value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
      return value ^ (value >> 31);
    }

    [[nodiscard]] uint64_t animationRandomSalt() {
      uint64_t salt = 0;
      if (getrandom(&salt, sizeof(salt), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(salt))) {
        salt = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
            ^ static_cast<uint64_t>(getpid())
            ^ reinterpret_cast<uintptr_t>(&salt);
      }
      return mixRandom(salt);
    }

    [[nodiscard]] AnimationTransition beginAnimationTransition() {
      static std::atomic<uint64_t> nextId{1};
      static const uint64_t salt = animationRandomSalt();
      uint64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
      if (id == 0) {
        id = nextId.fetch_add(1, std::memory_order_relaxed);
      }
      AnimationTransition transition{.id = id, .seed = {}};
      for (std::size_t channel = 0; channel < transition.seed.size(); ++channel) {
        const uint64_t value = mixRandom(salt ^ mixRandom(id + UINT64_C(0x9e3779b97f4a7c15) * (channel + 1)));
        transition.seed[channel] = static_cast<float>(value >> 40) * (1.0F / 16777216.0F);
      }
      return transition;
    }

    [[nodiscard]] std::string normalizeName(std::string_view name) {
      std::string out;
      out.reserve(name.size());
      for (char ch : name) {
        if (ch != '_' && ch != '-' && ch != ' ') {
          out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
      }
      return out;
    }

    class CurveRegistryImpl {
    public:
      CurveRegistryImpl() { initDefaults(); }

      void reset() {
        std::unique_lock lock(m_mutex);
        m_curves.clear();
        populateDefaults();
      }

      void registerCurve(std::string_view name, const AnimationCurve& curve) {
        std::unique_lock lock(m_mutex);
        m_curves[normalizeName(name)] = curve;
      }

      [[nodiscard]] std::optional<AnimationCurve> lookup(std::string_view name) const {
        std::shared_lock lock(m_mutex);
        const auto it = m_curves.find(normalizeName(name));
        if (it != m_curves.end()) {
          return it->second;
        }
        return std::nullopt;
      }

      bool unregisterCurve(std::string_view name) {
        std::unique_lock lock(m_mutex);
        return m_curves.erase(normalizeName(name)) > 0;
      }

      [[nodiscard]] bool has(std::string_view name) const {
        std::shared_lock lock(m_mutex);
        return m_curves.contains(normalizeName(name));
      }

      [[nodiscard]] std::optional<AnimationCurve> parse(std::string_view str) const {
        std::string value(str);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
          value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
          value.pop_back();
        }
        if (value.empty()) {
          return std::nullopt;
        }
        if (auto curve = lookup(value)) {
          return curve;
        }

        const auto fullyConsumed = [](const std::string& input, int consumed) {
          return consumed > 0
              && std::ranges::all_of(std::string_view(input).substr(static_cast<size_t>(consumed)), [](char ch) {
                   return std::isspace(static_cast<unsigned char>(ch));
                 });
        };

        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        int consumed = 0;
        if (std::sscanf(value.c_str(), " %lf , %lf , %lf , %lf %n", &x1, &y1, &x2, &y2, &consumed) == 4
            && fullyConsumed(value, consumed)
            && std::isfinite(x1)
            && std::isfinite(y1)
            && std::isfinite(x2)
            && std::isfinite(y2)
            && x1 >= 0.0
            && x1 <= 1.0
            && x2 >= 0.0
            && x2 <= 1.0) {
          return AnimationCurve{.easing = Easing::CustomBezier, .bezier = {x1, y1, x2, y2}};
        }

        constexpr std::string_view kSpringPrefix = "spring:";
        if (!value.starts_with(kSpringPrefix)) {
          return std::nullopt;
        }
        const std::string parameters = value.substr(kSpringPrefix.size());
        double damping = 0.0;
        double stiffness = 0.0;
        consumed = 0;
        if (std::sscanf(parameters.c_str(), " %lf , %lf %n", &damping, &stiffness, &consumed) != 2
            || !fullyConsumed(parameters, consumed)
            || !std::isfinite(damping)
            || !std::isfinite(stiffness)
            || damping < 0.01
            || damping > 5.0
            || stiffness < 1.0
            || stiffness > 10000.0) {
          return std::nullopt;
        }
        return AnimationCurve{
            .easing = Easing::Spring, .spring = {.damping = damping, .stiffness = stiffness, .mass = 1.0}
        };
      }

    private:
      void initDefaults() {
        std::unique_lock lock(m_mutex);
        populateDefaults();
      }

      void populateDefaults() {
        // Standard Easing Presets
        m_curves["linear"] = AnimationCurve{.easing = Easing::Linear};

        // Sine
        m_curves["easeinsine"] = AnimationCurve{.easing = Easing::EaseInSine};
        m_curves["easeoutsine"] = AnimationCurve{.easing = Easing::EaseOutSine};
        m_curves["easeinoutsine"] = AnimationCurve{.easing = Easing::EaseInOutSine};

        // Quad
        m_curves["easeinquad"] = AnimationCurve{.easing = Easing::EaseInQuad};
        m_curves["easeoutquad"] = AnimationCurve{.easing = Easing::EaseOutQuad};
        m_curves["easeinoutquad"] = AnimationCurve{.easing = Easing::EaseInOutQuad};
        m_curves["quad"] = AnimationCurve{.easing = Easing::EaseOutQuad};

        // Cubic
        m_curves["easeincubic"] = AnimationCurve{.easing = Easing::EaseInCubic};
        m_curves["easeoutcubic"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easeinoutcubic"] = AnimationCurve{.easing = Easing::EaseInOutCubic};
        m_curves["cubic"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["ease"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easein"] = AnimationCurve{.easing = Easing::EaseInCubic};
        m_curves["easeout"] = AnimationCurve{.easing = Easing::EaseOutCubic};
        m_curves["easeinout"] = AnimationCurve{.easing = Easing::EaseInOutCubic};

        // Quart
        m_curves["easeinquart"] = AnimationCurve{.easing = Easing::EaseInQuart};
        m_curves["easeoutquart"] = AnimationCurve{.easing = Easing::EaseOutQuart};
        m_curves["easeinoutquart"] = AnimationCurve{.easing = Easing::EaseInOutQuart};
        m_curves["quart"] = AnimationCurve{.easing = Easing::EaseOutQuart};

        // Quint
        m_curves["easeinquint"] = AnimationCurve{.easing = Easing::EaseInQuint};
        m_curves["easeoutquint"] = AnimationCurve{.easing = Easing::EaseOutQuint};
        m_curves["easeinoutquint"] = AnimationCurve{.easing = Easing::EaseInOutQuint};
        m_curves["quint"] = AnimationCurve{.easing = Easing::EaseOutQuint};

        // Expo
        m_curves["easeinexpo"] = AnimationCurve{.easing = Easing::EaseInExpo};
        m_curves["easeoutexpo"] = AnimationCurve{.easing = Easing::EaseOutExpo};
        m_curves["easeinoutexpo"] = AnimationCurve{.easing = Easing::EaseInOutExpo};
        m_curves["expo"] = AnimationCurve{.easing = Easing::EaseOutExpo};

        // Circ
        m_curves["easeincirc"] = AnimationCurve{.easing = Easing::EaseInCirc};
        m_curves["easeoutcirc"] = AnimationCurve{.easing = Easing::EaseOutCirc};
        m_curves["easeinoutcirc"] = AnimationCurve{.easing = Easing::EaseInOutCirc};
        m_curves["circ"] = AnimationCurve{.easing = Easing::EaseOutCirc};

        // Back
        m_curves["easeinback"] = AnimationCurve{.easing = Easing::EaseInBack};
        m_curves["easeoutback"] = AnimationCurve{.easing = Easing::EaseOutBack};
        m_curves["easeinoutback"] = AnimationCurve{.easing = Easing::EaseInOutBack};
        m_curves["back"] = AnimationCurve{.easing = Easing::EaseOutBack};
        m_curves["overshoot"] = AnimationCurve{.easing = Easing::EaseOutBack};

        // Elastic
        m_curves["easeinelastic"] = AnimationCurve{.easing = Easing::EaseInElastic};
        m_curves["easeoutelastic"] = AnimationCurve{.easing = Easing::EaseOutElastic};
        m_curves["easeinoutelastic"] = AnimationCurve{.easing = Easing::EaseInOutElastic};
        m_curves["elastic"] = AnimationCurve{.easing = Easing::EaseOutElastic};

        // Bounce
        m_curves["easeinbounce"] = AnimationCurve{.easing = Easing::EaseInBounce};
        m_curves["easeoutbounce"] = AnimationCurve{.easing = Easing::EaseOutBounce};
        m_curves["easeinoutbounce"] = AnimationCurve{.easing = Easing::EaseInOutBounce};
        m_curves["bounce"] = AnimationCurve{.easing = Easing::EaseOutBounce};

        // Default built-in animation curves
        m_curves["snappy"] = AnimationCurve{.easing = Easing::Snappy};
        m_curves["default"] = AnimationCurve{.easing = Easing::Snappy};
        m_curves["easeoutquint"] = AnimationCurve{.easing = Easing::CustomBezier, .bezier = {0.23, 1.0, 0.32, 1.0}};

        // Standard Named Springs
        m_curves["defaultspring"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.75, .stiffness = 100.0, .mass = 1.0}};
        m_curves["bouncy"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.5, .stiffness = 120.0, .mass = 1.0}};
        m_curves["smooth"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.9, .stiffness = 90.0, .mass = 1.0}};
        m_curves["stiff"] =
            AnimationCurve{.easing = Easing::Spring, .spring = {.damping = 0.8, .stiffness = 200.0, .mass = 1.0}};
      }

      mutable std::shared_mutex m_mutex;
      std::unordered_map<std::string, AnimationCurve> m_curves;
    };

    CurveRegistryImpl& registryImpl() {
      static CurveRegistryImpl impl;
      return impl;
    }
  } // namespace

  double solveCubicBezier(double x1, double y1, double x2, double y2, double x) {
    if (x <= 0.0) {
      return 0.0;
    }
    if (x >= 1.0) {
      return 1.0;
    }
    if (!std::isfinite(x)) {
      return 0.0;
    }

    const double cx1 = std::clamp(x1, 0.0, 1.0);
    const double cx2 = std::clamp(x2, 0.0, 1.0);

    const double cx = 3.0 * cx1;
    const double bx = 3.0 * (cx2 - cx1) - cx;
    const double ax = 1.0 - cx - bx;

    const double cy = 3.0 * y1;
    const double by = 3.0 * (y2 - y1) - cy;
    const double ay = 1.0 - cy - by;

    auto evalX = [ax, bx, cx](double t) noexcept { return ((ax * t + bx) * t + cx) * t; };
    auto evalY = [ay, by, cy](double t) noexcept { return ((ay * t + by) * t + cy) * t; };
    auto evalDx = [ax, bx, cx](double t) noexcept { return (3.0 * ax * t + 2.0 * bx) * t + cx; };

    // Newton-Raphson iteration (fast quadratic convergence)
    double t = x;
    for (int i = 0; i < 8; ++i) {
      const double currentX = evalX(t) - x;
      if (std::abs(currentX) < 1e-7) {
        return evalY(t);
      }
      const double dX = evalDx(t);
      if (std::abs(dX) < 1e-6) {
        break;
      }
      const double nextT = t - currentX / dX;
      if (nextT < 0.0 || nextT > 1.0) {
        break;
      }
      t = nextT;
    }

    // Bounded bisection fallback (guaranteed monotonic convergence)
    double minT = 0.0;
    double maxT = 1.0;
    t = x;
    for (int i = 0; i < 12; ++i) {
      const double guessX = evalX(t);
      if (std::abs(guessX - x) < 1e-7) {
        return evalY(t);
      }
      if (x > guessX) {
        minT = t;
      } else {
        maxT = t;
      }
      t = 0.5 * (minT + maxT);
    }
    return evalY(t);
  }

  int springDurationMs(const SpringConfig& config) {
    thread_local SpringConfig cachedConfig{};
    thread_local int cachedMs = 0;
    if (cachedMs != 0 && config == cachedConfig) {
      return cachedMs;
    }

    const double mass = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double stiffness = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    // Remaining mechanical energy of the unit step, which decays monotonically, so the settle time is bisectable.
    const auto remaining = [&config, mass, stiffness](double seconds) {
      double velocity = 0.0;
      const double position = solveSpringPhysics(0.0, 1.0, 0.0, seconds, config, &velocity);
      return std::hypot(1.0 - position, velocity * std::sqrt(mass / stiffness));
    };

    constexpr double kMaxSeconds = static_cast<double>(kSpringMaxDurationMs) / 1000.0;
    double settled = std::min(kMaxSeconds, std::sqrt(mass / stiffness));
    while (settled < kMaxSeconds && remaining(settled) > kSpringSettleEpsilon) {
      settled = std::min(kMaxSeconds, settled * 2.0);
    }
    int durationMs = kSpringMaxDurationMs;
    if (remaining(settled) <= kSpringSettleEpsilon) {
      double unsettled = 0.0;
      for (int i = 0; i < 40; ++i) {
        const double middle = 0.5 * (unsettled + settled);
        if (remaining(middle) > kSpringSettleEpsilon) {
          unsettled = middle;
        } else {
          settled = middle;
        }
      }
      durationMs = static_cast<int>(std::ceil(settled * 1000.0));
    }

    cachedConfig = config;
    cachedMs = std::clamp(durationMs, kSpringMinDurationMs, kSpringMaxDurationMs);
    return cachedMs;
  }

  double solveSpringPhysics(
      double from, double to, double velocity, double elapsedSec, const SpringConfig& config, double* outVelocity
  ) {
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(velocity)) {
      if (outVelocity != nullptr) {
        *outVelocity = 0.0;
      }
      return to;
    }
    if (elapsedSec <= 0.0) {
      if (outVelocity != nullptr) {
        *outVelocity = velocity;
      }
      return from;
    }

    const double m = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double k = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    const double w0 = std::sqrt(k / m);
    const double zeta = std::max(0.0, std::isfinite(config.damping) ? config.damping : 0.75);
    const double beta = zeta * w0;
    const double x0 = from - to;
    const double v0 = velocity;
    const double t = elapsedSec;
    const double env = std::exp(-beta * t);

    double posOffset = 0.0;
    double vel = 0.0;

    if (std::abs(zeta - 1.0) < 1e-5) {
      // Critically damped
      posOffset = env * (x0 + (v0 + w0 * x0) * t);
      vel = env * (v0 - (v0 + w0 * x0) * w0 * t);
    } else if (zeta < 1.0) {
      // Underdamped
      const double wd = w0 * std::sqrt(1.0 - zeta * zeta);
      const double sinVal = std::sin(wd * t);
      const double cosVal = std::cos(wd * t);
      const double c2 = (v0 + beta * x0) / wd;
      posOffset = env * (x0 * cosVal + c2 * sinVal);
      vel = env * ((v0 * cosVal) - (x0 * wd + beta * c2) * sinVal);
    } else {
      // Overdamped
      const double wd = w0 * std::sqrt(zeta * zeta - 1.0);
      const double sinhVal = std::sinh(wd * t);
      const double coshVal = std::cosh(wd * t);
      const double c2 = (v0 + beta * x0) / wd;
      posOffset = env * (x0 * coshVal + c2 * sinhVal);
      vel = env * (v0 * coshVal + (x0 * wd - beta * c2) * sinhVal);
    }

    if (std::abs(posOffset) < 1e-4 && std::abs(vel) < 1e-4) {
      if (outVelocity != nullptr) {
        *outVelocity = 0.0;
      }
      return to;
    }

    if (outVelocity != nullptr) {
      *outVelocity = vel;
    }
    return to + posOffset;
  }

  double springDisplacementBound(double current, double target, double velocity, const SpringConfig& config) {
    if (!std::isfinite(current) || !std::isfinite(target) || !std::isfinite(velocity)) {
      return std::numeric_limits<double>::infinity();
    }
    const double mass = std::max(1e-4, std::isfinite(config.mass) ? config.mass : 1.0);
    const double stiffness = std::max(1e-4, std::isfinite(config.stiffness) ? config.stiffness : 100.0);
    return std::hypot(current - target, velocity * std::sqrt(mass / stiffness));
  }

  double applyEasing(const AnimationCurve& curve, double progress) {
    const double linear = std::clamp(progress, 0.0, 1.0);

    switch (curve.easing) {
    case Easing::Linear:
      return linear;

    // Sine
    case Easing::EaseInSine:
      return 1.0 - std::cos((linear * kPi) / 2.0);
    case Easing::EaseOutSine:
      return std::sin((linear * kPi) / 2.0);
    case Easing::EaseInOutSine:
      return -(std::cos(kPi * linear) - 1.0) / 2.0;

    // Quad
    case Easing::EaseInQuad:
      return linear * linear;
    case Easing::EaseOutQuad: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining;
    }
    case Easing::EaseInOutQuad:
      return linear < 0.5 ? 2.0 * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 2) / 2.0;

    // Cubic
    case Easing::EaseInCubic:
      return linear * linear * linear;
    case Easing::EaseOutCubic: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining;
    }
    case Easing::EaseInOutCubic:
      return linear < 0.5 ? 4.0 * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 3) / 2.0;

    // Quart
    case Easing::EaseInQuart:
      return linear * linear * linear * linear;
    case Easing::EaseOutQuart: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining * remaining;
    }
    case Easing::EaseInOutQuart:
      return linear < 0.5 ? 8.0 * linear * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 4) / 2.0;

    // Quint
    case Easing::EaseInQuint:
      return std::pow(linear, 5);
    case Easing::EaseOutQuint: {
      const double remaining = 1.0 - linear;
      return 1.0 - remaining * remaining * remaining * remaining * remaining;
    }
    case Easing::EaseInOutQuint:
      return linear < 0.5 ? 16.0 * std::pow(linear, 5) : 1.0 - std::pow(-2.0 * linear + 2.0, 5) / 2.0;

    // Expo
    case Easing::EaseInExpo:
      return linear <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * linear - 10.0);
    case Easing::EaseOutExpo:
      return linear >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * linear);
    case Easing::EaseInOutExpo:
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return linear < 0.5 ? std::pow(2.0, 20.0 * linear - 10.0) / 2.0
                          : (2.0 - std::pow(2.0, -20.0 * linear + 10.0)) / 2.0;

    // Circ
    case Easing::EaseInCirc:
      return 1.0 - std::sqrt(1.0 - std::pow(linear, 2));
    case Easing::EaseOutCirc:
      return std::sqrt(1.0 - std::pow(linear - 1.0, 2));
    case Easing::EaseInOutCirc:
      return linear < 0.5 ? (1.0 - std::sqrt(1.0 - std::pow(2.0 * linear, 2))) / 2.0
                          : (std::sqrt(1.0 - std::pow(-2.0 * linear + 2.0, 2)) + 1.0) / 2.0;

    // Back
    case Easing::EaseInBack: {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      return c3 * linear * linear * linear - c1 * linear * linear;
    }
    case Easing::EaseOutBack: {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      const double t = linear - 1.0;
      return 1.0 + c3 * t * t * t + c1 * t * t;
    }
    case Easing::EaseInOutBack: {
      constexpr double c1 = 1.70158 * 1.525;
      constexpr double c2 = c1 + 1.0;
      return linear < 0.5 ? (std::pow(2.0 * linear, 2) * ((c2 + 1.0) * 2.0 * linear - c2)) / 2.0
                          : (std::pow(2.0 * linear - 2.0, 2) * ((c2 + 1.0) * (2.0 * linear - 2.0) + c2) + 2.0) / 2.0;
    }

    // Elastic
    case Easing::EaseInElastic: {
      constexpr double c4 = (2.0 * kPi) / 3.0;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return -std::pow(2.0, 10.0 * linear - 10.0) * std::sin((linear * 10.0 - 10.75) * c4);
    }
    case Easing::EaseOutElastic: {
      constexpr double c4 = (2.0 * kPi) / 3.0;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return std::pow(2.0, -10.0 * linear) * std::sin((linear * 10.0 - 0.75) * c4) + 1.0;
    }
    case Easing::EaseInOutElastic: {
      constexpr double c5 = (2.0 * kPi) / 4.5;
      if (linear <= 0.0) {
        return 0.0;
      }
      if (linear >= 1.0) {
        return 1.0;
      }
      return linear < 0.5
          ? -(std::pow(2.0, 20.0 * linear - 10.0) * std::sin((20.0 * linear - 11.125) * c5)) / 2.0
          : (std::pow(2.0, -20.0 * linear + 10.0) * std::sin((20.0 * linear - 11.125) * c5)) / 2.0 + 1.0;
    }

    // Bounce
    case Easing::EaseInBounce: {
      const auto easeOutBounce = [](double t) {
        constexpr double n1 = 7.5625;
        constexpr double d1 = 2.75;
        if (t < 1.0 / d1) {
          return n1 * t * t;
        }
        if (t < 2.0 / d1) {
          t -= 1.5 / d1;
          return n1 * t * t + 0.75;
        }
        if (t < 2.5 / d1) {
          t -= 2.25 / d1;
          return n1 * t * t + 0.9375;
        }
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
      };
      return 1.0 - easeOutBounce(1.0 - linear);
    }
    case Easing::EaseOutBounce: {
      constexpr double n1 = 7.5625;
      constexpr double d1 = 2.75;
      double t = linear;
      if (t < 1.0 / d1) {
        return n1 * t * t;
      }
      if (t < 2.0 / d1) {
        t -= 1.5 / d1;
        return n1 * t * t + 0.75;
      }
      if (t < 2.5 / d1) {
        t -= 2.25 / d1;
        return n1 * t * t + 0.9375;
      }
      t -= 2.625 / d1;
      return n1 * t * t + 0.984375;
    }
    case Easing::EaseInOutBounce: {
      const auto easeOutBounce = [](double t) {
        constexpr double n1 = 7.5625;
        constexpr double d1 = 2.75;
        if (t < 1.0 / d1) {
          return n1 * t * t;
        }
        if (t < 2.0 / d1) {
          t -= 1.5 / d1;
          return n1 * t * t + 0.75;
        }
        if (t < 2.5 / d1) {
          t -= 2.25 / d1;
          return n1 * t * t + 0.9375;
        }
        t -= 2.625 / d1;
        return n1 * t * t + 0.984375;
      };
      return linear < 0.5 ? (1.0 - easeOutBounce(1.0 - 2.0 * linear)) / 2.0
                          : (1.0 + easeOutBounce(2.0 * linear - 1.0)) / 2.0;
    }

    case Easing::Snappy:
      return solveCubicBezier(0.05, 0.9, 0.1, 1.05, linear);

    case Easing::CustomBezier:
      return solveCubicBezier(curve.bezier.x1, curve.bezier.y1, curve.bezier.x2, curve.bezier.y2, linear);

    case Easing::Spring: {
      // Physics over the spring's own settle time, so damping shapes the response while stiffness and mass set how
      // long it takes. Normalized time would cancel both, leaving damping as the only observable parameter.
      const double seconds = linear * static_cast<double>(springDurationMs(curve.spring)) / 1000.0;
      return solveSpringPhysics(0.0, 1.0, 0.0, seconds, curve.spring);
    }
    }

    return linear;
  }

  MonotonicEasing::MonotonicEasing() { reset(AnimationCurve{.easing = Easing::Linear}); }

  MonotonicEasing::MonotonicEasing(const AnimationCurve& curve) { reset(curve); }

  void MonotonicEasing::reset(const AnimationCurve& curve) {
    m_curve = curve;
    std::array<double, kSampleCount + 1> raw{};
    raw.front() = 0.0;
    raw.back() = 1.0;

    m_direct = true;
    double previous = raw.front();
    for (std::size_t i = 1; i < kSampleCount; ++i) {
      const double progress = static_cast<double>(i) / static_cast<double>(kSampleCount);
      const double current = evaluateCurve(curve, progress);
      if (!std::isfinite(current)) {
        m_curve = AnimationCurve{.easing = Easing::Linear};
        m_direct = true;
        return;
      }
      raw[i] = current;
      if (current < previous || current < 0.0 || current > 1.0) {
        m_direct = false;
      }
      previous = current;
    }
    if (raw.back() < previous) {
      m_direct = false;
    }
    if (m_direct) {
      return;
    }

    // Scale before subtracting so even extreme, but finite, custom Bezier control points cannot overflow a delta.
    double scale = 1.0;
    for (const double sample : raw) {
      scale = std::max(scale, std::abs(sample));
    }
    m_progress.front() = 0.0;
    for (std::size_t i = 1; i <= kSampleCount; ++i) {
      const double delta = std::abs(raw[i] / scale - raw[i - 1] / scale);
      m_progress[i] = m_progress[i - 1] + delta;
    }
    const double total = m_progress.back();
    if (!std::isfinite(total) || total <= std::numeric_limits<double>::epsilon()) {
      m_curve = AnimationCurve{.easing = Easing::Linear};
      m_direct = true;
      return;
    }
    for (double& sample : m_progress) {
      sample /= total;
    }
    m_progress.front() = 0.0;
    m_progress.back() = 1.0;
  }

  double MonotonicEasing::value(double linearProgress) const {
    const double linear = std::clamp(linearProgress, 0.0, 1.0);
    if (linear <= 0.0) {
      return 0.0;
    }
    if (linear >= 1.0) {
      return 1.0;
    }
    if (m_direct) {
      return std::clamp(evaluateCurve(m_curve, linear), 0.0, 1.0);
    }

    const double sample = linear * static_cast<double>(kSampleCount);
    const std::size_t lower = static_cast<std::size_t>(sample);
    const double fraction = sample - static_cast<double>(lower);
    return std::lerp(m_progress[lower], m_progress[lower + 1], fraction);
  }

  // CurveRegistry methods
  void CurveRegistry::registerCurve(std::string_view name, const AnimationCurve& curve) {
    registryImpl().registerCurve(name, curve);
  }

  void CurveRegistry::registerBezier(std::string_view name, double x1, double y1, double x2, double y2) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    registryImpl().registerCurve(name, c);
  }

  void CurveRegistry::registerSpring(std::string_view name, double damping, double stiffness, double mass) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, mass};
    registryImpl().registerCurve(name, c);
  }

  std::optional<AnimationCurve> CurveRegistry::lookup(std::string_view name) { return registryImpl().lookup(name); }

  AnimationCurve CurveRegistry::get(std::string_view name, const AnimationCurve& fallback) {
    return lookup(name).value_or(fallback);
  }

  bool CurveRegistry::has(std::string_view name) { return registryImpl().has(name); }

  bool CurveRegistry::unregisterCurve(std::string_view name) { return registryImpl().unregisterCurve(name); }

  void CurveRegistry::resetToDefaults() { registryImpl().reset(); }

  std::optional<AnimationCurve> CurveRegistry::parse(std::string_view str) { return registryImpl().parse(str); }

  // AnimatedValue
  void AnimatedValue::snap(double value) {
    m_from = value;
    m_target = value;
    m_current = value;
    m_velocity = 0.0;
    m_initialVelocity = 0.0;
    m_progress = 1.0;
    m_startMsec = 0;
    m_animating = false;
    m_physics = false;
  }

  void AnimatedValue::retarget(double to, int durationMs, Easing easing) {
    retarget(to, durationMs, AnimationCurve{.easing = easing});
  }

  void AnimatedValue::retarget(double to, int durationMs, const AnimationCurve& curve) {
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_durationMsec = curve.easing == Easing::Spring ? static_cast<uint64_t>(springDurationMs(curve.spring))
                                                    : static_cast<uint64_t>(std::max(1, durationMs));
    m_curve = curve;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
    m_physics = false;
    m_initialVelocity = 0.0;
  }

  void AnimatedValue::retarget(double to, int durationMs, std::string_view curveName) {
    retarget(to, durationMs, CurveRegistry::get(curveName));
  }

  void AnimatedValue::retargetBezier(double to, int durationMs, double x1, double y1, double x2, double y2) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    retarget(to, durationMs, c);
  }

  void AnimatedValue::retargetSpring(double to, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0};
    retarget(to, 0, c);
  }

  void AnimatedValue::settleSpring(double to, const SpringConfig& spring, double initialVelocity) {
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_curve = AnimationCurve{.easing = Easing::Spring, .spring = spring};
    m_initialVelocity = initialVelocity;
    m_velocity = initialVelocity;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
    m_physics = true;
  }

  bool AnimatedValue::finishSpringTail(double pixelsPerUnit) {
    if (!m_animating || !m_physics || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0) {
      return false;
    }
    const double targetPixels = m_target * pixelsPerUnit;
    if (!std::isfinite(targetPixels)) {
      return false;
    }
    const double roundedTarget = std::round(targetPixels);
    const double roundingMargin = 0.5 - std::abs(targetPixels - roundedTarget);
    const double remaining = springDisplacementBound(m_current, m_target, m_velocity, m_curve.spring) * pixelsPerUnit;
    if (roundingMargin <= 0.0 || remaining >= roundingMargin) {
      return false;
    }
    snap(m_target);
    return true;
  }

  void AnimatedValue::translate(double delta) {
    m_from += delta;
    m_target += delta;
    m_current += delta;
  }

  double AnimatedValue::progress() const { return m_progress; }

  bool AnimatedValue::tick(uint64_t nowMsec) {
    if (!m_animating) {
      return false;
    }
    if (m_startMsec == 0) {
      m_startMsec = nowMsec;
    }

    const uint64_t elapsed = nowMsec - std::min(nowMsec, m_startMsec);

    if (m_physics) {
      double velocity = 0.0;
      m_current = solveSpringPhysics(
          m_from, m_target, m_initialVelocity, static_cast<double>(elapsed) / 1000.0, m_curve.spring, &velocity
      );
      m_velocity = velocity;
      const double span = m_target - m_from;
      m_progress = span != 0.0 ? std::clamp((m_current - m_from) / span, 0.0, 1.0) : 1.0;
      if (m_current == m_target && velocity == 0.0) {
        m_progress = 1.0;
        m_animating = false;
      }
      return true;
    }

    const double linear = std::clamp(static_cast<double>(elapsed) / static_cast<double>(m_durationMsec), 0.0, 1.0);
    m_progress = linear;

    const double prevCurrent = m_current;

    if (linear >= 1.0) {
      m_current = m_target;
      m_velocity = 0.0;
      m_animating = false;
      return true;
    }

    const double eased = applyEasing(m_curve, linear);
    m_current = m_from + (m_target - m_from) * eased;

    const double dtSec = std::max(0.001, static_cast<double>(elapsed) / 1000.0);
    m_velocity = (m_current - prevCurrent) / dtSec;

    return true;
  }

  // AnimatedColor
  void AnimatedColor::snap(const std::array<float, 4>& color) {
    m_from = color;
    m_target = color;
    m_current = color;
    m_fromOkLab = srgbToOkLab(color);
    m_targetOkLab = m_fromOkLab;
    m_progress = 1.0;
    m_startMsec = 0;
    m_animating = false;
  }

  void AnimatedColor::snap(float r, float g, float b, float a) { snap(std::array<float, 4>{r, g, b, a}); }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, Easing easing) {
    retarget(to, durationMs, AnimationCurve{.easing = easing});
  }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, const AnimationCurve& curve) {
    const AnimationTransition transition = beginAnimationTransition();
    m_transitionId = transition.id;
    m_shaderSeed = transition.seed;
    m_from = m_current;
    m_target = to;
    m_fromOkLab = srgbToOkLab(m_from);
    m_targetOkLab = srgbToOkLab(m_target);
    m_durationMsec = curve.easing == Easing::Spring ? static_cast<uint64_t>(springDurationMs(curve.spring))
                                                    : static_cast<uint64_t>(std::max(1, durationMs));
    m_curve = curve;
    m_startMsec = 0;
    m_progress = 0.0;
    m_animating = true;
  }

  void AnimatedColor::retarget(const std::array<float, 4>& to, int durationMs, std::string_view curveName) {
    retarget(to, durationMs, CurveRegistry::get(curveName));
  }

  void AnimatedColor::retarget(float r, float g, float b, float a, int durationMs, const AnimationCurve& curve) {
    retarget(std::array<float, 4>{r, g, b, a}, durationMs, curve);
  }

  void AnimatedColor::retargetBezier(
      const std::array<float, 4>& to, int durationMs, double x1, double y1, double x2, double y2
  ) {
    AnimationCurve c;
    c.easing = Easing::CustomBezier;
    c.bezier = {x1, y1, x2, y2};
    retarget(to, durationMs, c);
  }

  void AnimatedColor::retargetSpring(const std::array<float, 4>& to, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0};
    retarget(to, 0, c);
  }

  double AnimatedColor::progress() const { return m_progress; }

  bool AnimatedColor::tick(uint64_t nowMsec) {
    if (!m_animating) {
      return false;
    }
    if (m_startMsec == 0) {
      m_startMsec = nowMsec;
    }

    const uint64_t elapsed = nowMsec - std::min(nowMsec, m_startMsec);
    const double linear = std::clamp(static_cast<double>(elapsed) / static_cast<double>(m_durationMsec), 0.0, 1.0);
    m_progress = linear;

    if (linear >= 1.0) {
      m_current = m_target;
      m_animating = false;
      return true;
    }

    const auto eased = static_cast<float>(applyEasing(m_curve, linear));
    const OkLab interpolated = interpolateOkLab(m_fromOkLab, m_targetOkLab, eased);
    const float alpha = std::lerp(m_from[3], m_target[3], eased);
    m_current = okLabToSrgb(interpolated, alpha);

    return true;
  }

  void AnimatedColor::current(float out[4]) const {
    out[0] = m_current[0];
    out[1] = m_current[1];
    out[2] = m_current[2];
    out[3] = m_current[3];
  }

} // namespace umbriel
