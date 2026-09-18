#pragma once

#include <cstdint>
#include <optional>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  // A visible opening transition survives an unrelated unanimated arrange mark coalesced into the same frame.
  [[nodiscard]] bool lifecycleArrangeShouldAnimate(bool requestedAnimation, bool openingPending);

  // An unanimated arrange still leaves a position animation alone when it is already headed to the authoritative
  // layout target. A different target remains an intentional snap.
  [[nodiscard]] bool layoutPositionShouldSnap(bool requestedAnimation, bool animatingToTarget);

  // Move `subject` with the nearest edge of `anchorFrom` as that anchor becomes `anchorTo`. The subject keeps its
  // size and edge gap, while its cross-axis offset follows the anchor. Adjacent layout rectangles therefore remain
  // disjoint throughout equal-curve interpolation.
  [[nodiscard]] std::optional<wlr_box>
  edgeLockedTarget(const wlr_box& subject, const wlr_box& anchorFrom, const wlr_box& anchorTo);

  // As above, but place a differently sized presentation of `subject`. This is used while an opening tile shrinks
  // from its initial client size to its assigned layout size.
  [[nodiscard]] std::optional<wlr_box> edgeLockedTargetSized(
      const wlr_box& subject, const wlr_box& anchorFrom, const wlr_box& anchorTo, int targetWidth, int targetHeight
  );

  // Squared empty-space distance between rectangles. Intersecting rectangles have distance zero.
  [[nodiscard]] std::int64_t boxDistanceSquared(const wlr_box& first, const wlr_box& second);

  // Area shared by two rectangles. Disjoint rectangles have area zero.
  [[nodiscard]] std::int64_t boxIntersectionArea(const wlr_box& first, const wlr_box& second);

} // namespace umbriel
