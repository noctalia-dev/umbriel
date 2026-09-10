#include "core/animation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <unordered_map>

namespace umbriel {

  namespace {
    constexpr double kPi = std::numbers::pi;

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
            || stiffness > 1000.0) {
          return std::nullopt;
        }
        return AnimationCurve{
            .easing = Easing::Spring,
            .spring = {.damping = damping, .stiffness = stiffness, .mass = 1.0, .initialVelocity = 0.0}
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
        m_curves["defaultspring"] = AnimationCurve{
            .easing = Easing::Spring,
            .spring = {.damping = 0.75, .stiffness = 100.0, .mass = 1.0, .initialVelocity = 0.0}
        };
        m_curves["bouncy"] = AnimationCurve{
            .easing = Easing::Spring,
            .spring = {.damping = 0.5, .stiffness = 120.0, .mass = 1.0, .initialVelocity = 0.0}
        };
        m_curves["smooth"] = AnimationCurve{
            .easing = Easing::Spring, .spring = {.damping = 0.9, .stiffness = 90.0, .mass = 1.0, .initialVelocity = 0.0}
        };
        m_curves["stiff"] = AnimationCurve{
            .easing = Easing::Spring,
            .spring = {.damping = 0.8, .stiffness = 200.0, .mass = 1.0, .initialVelocity = 0.0}
        };
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

  double solveSpring(double damping, double stiffness, double linear) {
    if (linear <= 0.0) {
      return 0.0;
    }
    if (linear >= 1.0) {
      return 1.0;
    }
    constexpr double m = 1.0;
    const double k = std::max(0.1, stiffness);
    const double w0 = std::sqrt(k / m);
    const double zeta = std::clamp(damping, 0.001, 5.0);
    const double t = linear * 6.0 / w0;

    if (zeta < 0.9999) {
      const double wd = w0 * std::sqrt(1.0 - zeta * zeta);
      return 1.0
          - std::exp(-zeta * w0 * t) * (std::cos(wd * t) + (zeta / std::sqrt(1.0 - zeta * zeta)) * std::sin(wd * t));
    }
    if (zeta <= 1.0001) {
      return 1.0 - std::exp(-w0 * t) * (1.0 + w0 * t);
    }
    const double wd = w0 * std::sqrt(zeta * zeta - 1.0);
    return 1.0
        - std::exp(-zeta * w0 * t) * (std::cosh(wd * t) + (zeta / std::sqrt(zeta * zeta - 1.0)) * std::sinh(wd * t));
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
      vel = env * ((v0 * coshVal) + (c2 * wd - beta * x0) * sinhVal - beta * c2 * coshVal);
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

    case Easing::Spring:
      return solveSpring(curve.spring.damping, curve.spring.stiffness, linear);
    }

    return linear;
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
    c.spring = {damping, stiffness, mass, 0.0};
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
    m_from = m_current;
    m_target = to;
    m_durationMsec = static_cast<uint64_t>(std::max(1, durationMs));
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

  void AnimatedValue::retargetSpring(double to, int durationMs, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0, 0.0};
    retarget(to, durationMs, c);
  }

  void AnimatedValue::settleSpring(double to, const SpringConfig& spring, double initialVelocity) {
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
    m_from = m_current;
    m_target = to;
    m_fromOkLab = srgbToOkLab(m_from);
    m_targetOkLab = srgbToOkLab(m_target);
    m_durationMsec = static_cast<uint64_t>(std::max(1, durationMs));
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

  void AnimatedColor::retargetSpring(const std::array<float, 4>& to, int durationMs, double damping, double stiffness) {
    AnimationCurve c;
    c.easing = Easing::Spring;
    c.spring = {damping, stiffness, 1.0, 0.0};
    retarget(to, durationMs, c);
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
