#pragma once

#include "layout/layout.h"

namespace umbriel {
  class StackingLayout final : public Layout {
  public:
    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Stacking; }
    [[nodiscard]] int columnOf(const View*) const override { return -1; }
    [[nodiscard]] int rowOf(const View*) const override { return -1; }
    [[nodiscard]] LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;
    void insertView(View*, int) override {}
    void insertViewIntoColumn(View*, int, int) override {}
    bool consume(View*, int) override { return false; }
    bool expel(View*, int) override { return false; }
    bool consumeFrom(View*, int) override { return false; }
    bool moveViewVertical(View*, int) override { return false; }
    bool swapViews(View*, View*) override { return false; }
    void removeView(View*) override {}
    void moveColumn(int, int) override {}
    void arrange(const wlr_box&) override {}
    [[nodiscard]] wlr_box targetBox(const View*) const override;
    [[nodiscard]] InitialSize
    initialSize(const wlr_box&, bool, std::optional<double>, std::optional<int>, const View*) const override {
      return {};
    }
    bool cycleWidth(int, int) override { return false; }
    bool toggleFullWidth(int) override { return false; }
    bool setWidthFraction(int, double) override { return false; }
    void clearFullWidthState(int) override {}
    [[nodiscard]] double widthFraction(int) const override { return 1.0; }
    [[nodiscard]] double heightFraction(const View*) const override { return 1.0; }
    bool setHeightFraction(View*, double) override { return false; }
    [[nodiscard]] const std::vector<Column>& columns() const override;
    [[nodiscard]] bool isFullWidth(int) const override { return false; }
  };
} // namespace umbriel
