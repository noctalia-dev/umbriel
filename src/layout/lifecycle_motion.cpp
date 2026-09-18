#include "layout/lifecycle_motion.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace umbriel {

  bool lifecycleArrangeShouldAnimate(bool requestedAnimation, bool openingPending) {
    return requestedAnimation || openingPending;
  }

  bool layoutPositionShouldSnap(bool requestedAnimation, bool animatingToTarget) {
    return !requestedAnimation && !animatingToTarget;
  }

  namespace {

    enum class Edge {
      Left,
      Right,
      Top,
      Bottom,
    };

    struct Candidate {
      Edge edge;
      int gap;
    };

    int overlap(int firstStart, int firstExtent, int secondStart, int secondExtent) {
      return std::min(firstStart + firstExtent, secondStart + secondExtent) - std::max(firstStart, secondStart);
    }

  } // namespace

  std::optional<wlr_box> edgeLockedTargetSized(
      const wlr_box& subject, const wlr_box& anchorFrom, const wlr_box& anchorTo, int targetWidth, int targetHeight
  ) {
    if (subject.width <= 0
        || subject.height <= 0
        || anchorFrom.width <= 0
        || anchorFrom.height <= 0
        || anchorTo.width <= 0
        || anchorTo.height <= 0
        || targetWidth <= 0
        || targetHeight <= 0) {
      return std::nullopt;
    }

    std::array<Candidate, 4> candidates{};
    size_t count = 0;
    if (overlap(subject.y, subject.height, anchorFrom.y, anchorFrom.height) > 0) {
      if (subject.x >= anchorFrom.x + anchorFrom.width) {
        candidates[count++] = {.edge = Edge::Left, .gap = subject.x - anchorFrom.x - anchorFrom.width};
      }
      if (subject.x + subject.width <= anchorFrom.x) {
        candidates[count++] = {.edge = Edge::Right, .gap = anchorFrom.x - subject.x - subject.width};
      }
    }
    if (overlap(subject.x, subject.width, anchorFrom.x, anchorFrom.width) > 0) {
      if (subject.y >= anchorFrom.y + anchorFrom.height) {
        candidates[count++] = {.edge = Edge::Top, .gap = subject.y - anchorFrom.y - anchorFrom.height};
      }
      if (subject.y + subject.height <= anchorFrom.y) {
        candidates[count++] = {.edge = Edge::Bottom, .gap = anchorFrom.y - subject.y - subject.height};
      }
    }
    if (count == 0) {
      return std::nullopt;
    }

    const Candidate candidate = *std::min_element(
        candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count),
        [](const Candidate& first, const Candidate& second) { return first.gap < second.gap; }
    );
    wlr_box target = subject;
    target.width = targetWidth;
    target.height = targetHeight;
    switch (candidate.edge) {
    case Edge::Left:
      target.x = anchorTo.x + anchorTo.width + candidate.gap;
      target.y += anchorTo.y - anchorFrom.y;
      break;
    case Edge::Right:
      target.x = anchorTo.x - candidate.gap - target.width;
      target.y += anchorTo.y - anchorFrom.y;
      break;
    case Edge::Top:
      target.x += anchorTo.x - anchorFrom.x;
      target.y = anchorTo.y + anchorTo.height + candidate.gap;
      break;
    case Edge::Bottom:
      target.x += anchorTo.x - anchorFrom.x;
      target.y = anchorTo.y - candidate.gap - target.height;
      break;
    }
    return target;
  }

  std::optional<wlr_box> edgeLockedTarget(const wlr_box& subject, const wlr_box& anchorFrom, const wlr_box& anchorTo) {
    return edgeLockedTargetSized(subject, anchorFrom, anchorTo, subject.width, subject.height);
  }

  std::int64_t boxDistanceSquared(const wlr_box& first, const wlr_box& second) {
    const std::int64_t dx = std::max({first.x - second.x - second.width, second.x - first.x - first.width, 0});
    const std::int64_t dy = std::max({first.y - second.y - second.height, second.y - first.y - first.height, 0});
    return dx * dx + dy * dy;
  }

  std::int64_t boxIntersectionArea(const wlr_box& first, const wlr_box& second) {
    const std::int64_t width = std::max(
        std::int64_t{0},
        std::min<std::int64_t>(std::int64_t{first.x} + first.width, std::int64_t{second.x} + second.width)
            - std::max<std::int64_t>(first.x, second.x)
    );
    const std::int64_t height = std::max(
        std::int64_t{0},
        std::min<std::int64_t>(std::int64_t{first.y} + first.height, std::int64_t{second.y} + second.height)
            - std::max<std::int64_t>(first.y, second.y)
    );
    return width * height;
  }

} // namespace umbriel
