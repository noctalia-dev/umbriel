#include "layout/stacking.h"

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  namespace {
    class StackingSnapshot final : public LayoutSnapshot {
    public:
      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Stacking; }
      [[nodiscard]] size_t memberCount() const override { return 0; }
    };
  } // namespace

  LayoutCapture StackingLayout::captureState() const { return {std::make_shared<StackingSnapshot>(), {}}; }
  bool StackingLayout::restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) {
    return snapshot.mode() == LayoutMode::Stacking && members.empty();
  }
  wlr_box StackingLayout::targetBox(const View*) const { return {}; }
  const std::vector<Column>& StackingLayout::columns() const {
    static const std::vector<Column> empty;
    return empty;
  }
} // namespace umbriel
