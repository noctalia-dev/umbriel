#pragma once

#include "layout/layout.h"

namespace umbriel {
  class StackingLayout final : public Layout {
  public:
    LayoutMode mode() const override { return LayoutMode::Stacking; }
    int columnOf(const View*) const override { return -1; }
    int rowOf(const View*) const override { return -1; }
    LayoutCapture captureState() const override;
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
    wlr_box targetBox(const View*) const override;
    InitialSize
    initialSize(const wlr_box&, bool, std::optional<double>, std::optional<int>, const View*) const override {
      return {};
    }
    bool cycleWidth(int, int) override { return false; }
    bool toggleFullWidth(int) override { return false; }
    bool setWidthFraction(int, double) override { return false; }
    void clearFullWidthState(int) override {}
    double widthFraction(int) const override { return 1.0; }
    double heightFraction(const View*) const override { return 1.0; }
    bool setHeightFraction(View*, double) override { return false; }
    const std::vector<Column>& columns() const override;
    bool isFullWidth(int) const override { return false; }
  };
} // namespace umbriel
