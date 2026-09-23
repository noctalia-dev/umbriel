#pragma once

#include "input/swipe_tracker.h"

#include <algorithm>
#include <cmath>

namespace umbriel {

  // Physics shared by the touchpad gestures that settle on a step: the three-finger workspace switch, the four-finger
  // overview open and close, and the filmstrip inside the overview. A gesture accumulates raw finger travel and, on
  // release, projects where that travel would coast to a stop under SwipeTracker's deceleration before rounding the
  // projection onto the nearest step. Distance and release speed therefore decide the landing together instead of
  // being weighed against separate thresholds, so a quick flick carries as far as it looks like it should and a slow
  // drag that stops before letting go falls back where it started.
  class GesturePhysics {
  public:
    // Travel in either direction before a gesture commits to one axis. The same number GNOME Shell and niri use.
    static constexpr double kAxisLock = 16.0;

    // Past either end of a step the fingers keep moving while the content lags further and further behind: the extra
    // travel approaches `limit` steps without ever reaching it, and the derivative of that curve is how much of the
    // visible speed a release still carries. The same numbers niri uses, so a gesture that runs into an end feels the
    // same in both.
    static constexpr double kOverscrollStiffness = 0.5;
    static constexpr double kOverscrollLimit = 0.05;

    // Damped position: unchanged inside [minimum, maximum], approaching `limit` steps past either end outside it.
    [[nodiscard]] static double rubberBand(double position, double minimum, double maximum, double limit) {
      const double clamped = std::clamp(position, minimum, maximum);
      return clamped + std::copysign(overscroll(std::abs(position - clamped), limit), position - clamped);
    }

    // How much of the finger's travel still reaches the content at `position`, as a fraction. A release carries its
    // speed through the same factor, so letting go while pushed past an end cannot fling the step.
    [[nodiscard]] static double rubberBandDerivative(double position, double minimum, double maximum, double limit) {
      if (minimum <= position && position <= maximum) {
        return 1.0;
      }
      const double excess = std::abs(position - std::clamp(position, minimum, maximum));
      const double denominator = 1.0 + kOverscrollStiffness * excess / limit;
      return kOverscrollStiffness / (denominator * denominator);
    }

    // The step a release lands on: where the projection comes to rest, clamped to the steps that exist, then rounded
    // onto one of them.
    [[nodiscard]] static int stepTarget(double projected, int minimum, int maximum) {
      const double clamped = std::clamp(projected, static_cast<double>(minimum), static_cast<double>(maximum));
      return static_cast<int>(std::lround(clamped));
    }

    // Reduce the effect of the overview zoom on touchpad travel, rather than multiplying sensitivity by the full
    // inverse zoom.
    [[nodiscard]] static double zoomScale(double zoom) { return 1.0 / (1.0 + (zoom - 1.0) / 2.5); }

    // What a released gesture settles on. Nothing here is a threshold: the projection decides the landing, so a short
    // flick and a long drag that end in the same place land differently only because of how fast they were going.
    struct StepRelease {
      int target = 0;      // Step the projection lands on.
      double position = 0; // Damped position at the release, in steps.
      double velocity = 0; // Speed the settle still carries, in steps per second.
    };

    // `origin` is where the gesture started, in steps: zero for a switch, which starts from where it already is, and
    // the progress it began from for the overview. `minimum` and `maximum` are the whole steps that exist. The caller
    // feeds the tracker a zero-delta sample at the release time first, so idle time before letting go bleeds off the
    // speed.
    [[nodiscard]] static StepRelease
    release(const SwipeTracker& tracker, double unitsPerStep, double origin, double minimum, double maximum) {
      const double travelled = origin + tracker.pos() / unitsPerStep;
      const double position = rubberBand(travelled, minimum, maximum, kOverscrollLimit);
      const double projected =
          rubberBand(origin + tracker.projectedEndPos() / unitsPerStep, minimum, maximum, kOverscrollLimit);
      return {
          .target = stepTarget(projected, static_cast<int>(minimum), static_cast<int>(maximum)),
          .position = position,
          // Damped travel moves the content slower than the fingers do, so the release keeps the speed that was
          // visible.
          .velocity =
              tracker.velocity() / unitsPerStep * rubberBandDerivative(position, minimum, maximum, kOverscrollLimit),
      };
    }

  private:
    // Extra steps a position ends up outside its range, for `excess` steps of finger travel past the end.
    [[nodiscard]] static double overscroll(double excess, double limit) {
      return limit * (1.0 - 1.0 / (1.0 + kOverscrollStiffness * excess / limit));
    }
  };

} // namespace umbriel
