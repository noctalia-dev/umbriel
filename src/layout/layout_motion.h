#pragma once

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  // One member of a layout transition: the box it leaves and the box it arrives at. Every member of a transition is
  // interpolated with the same progress, so two members separated along an axis in both `from` and `to` never cross.
  struct MotionBox {
    wlr_box from{};
    wlr_box to{};
  };

  // Edges are interpolated independently in double and rounded with lround; width = right - left (may be 0). Rounding
  // edges rather than origin+size keeps two boxes that do not cross in reals from crossing after rounding.
  wlr_box interpolateBox(const wlr_box& from, const wlr_box& to, double progress);

  // Position and size on separate timelines: the origin follows `positionProgress` and the size `sizeProgress`. With
  // equal progresses this is interpolateBox(from, to, progress). Unequal ones can make neighbours overlap mid-way.
  wlr_box interpolateBox(const wlr_box& from, const wlr_box& to, double positionProgress, double sizeProgress);

  // True when some axis and order separates `a` and `b` on both sides of the transition, i.e. interpolating them with
  // a shared progress can never make them cross. False marks a pair whose side relation changes (a rearrangement).
  bool keepsSeparation(const MotionBox& a, const MotionBox& b);

} // namespace umbriel
