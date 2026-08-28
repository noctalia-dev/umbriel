#pragma once

#include "layout/layout.h"

#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  // Public width and X vocabulary names primary-axis quantities for compatibility:
  // X is the primary scroll axis, width is its extent, and height is the cross
  // axis extent. Horizontal layouts map primary/cross to X/Y. Vertical layouts
  // map them to Y/X.

  class ScrollingLayout : public Layout {
  public:
    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Scrolling; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;
    [[nodiscard]] LayoutCapture captureState() const override;
    [[nodiscard]] LayoutCapture captureStateForViewport(int viewportPrimary) const;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;
    // Structural replay records a surviving lane anchor. Once focus has
    // settled, restore its viewport position using the returned output's
    // current logical extent. Exact raw offsets are used only when geometry is
    // known to be unchanged.
    void restoreSnapshotViewport(const LayoutSnapshot& snapshot, int viewportPrimary, bool geometryUnchanged);
    [[nodiscard]] double scroll() const { return m_scroll; }
    [[nodiscard]] int columnX(int columnIndex, int viewportPrimary) const;
    [[nodiscard]] int columnWidth(int columnIndex, int viewportPrimary) const;
    bool setWidthFromPixels(int columnIndex, int viewportPrimary, int width);
    [[nodiscard]] bool isFullWidth(int columnIndex) const override;
    [[nodiscard]] int maxScroll(int viewportPrimary) const {
      return std::max(0, totalWidth(viewportPrimary) - viewportPrimary);
    }

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consumeLeft(View* view) override;
    bool expelRight(View* view) override;
    bool moveViewVertical(View* view, int direction) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    // Raw scroll mutation. `centeredRest` is true only when restoring a saved column-center resting position.
    void setScroll(double scroll, bool centeredRest = false);
    bool centerColumn(int columnIndex, int viewportPrimary);
    [[nodiscard]] bool centeredRest() const { return m_centeredRest; }
    // How much to subtract from the scroll offset when `columnIndex` is about
    // to lose its last view. Removing a lane closes the primary-axis space it
    // held. Compensation re-anchors content when that space was hidden toward
    // strip start, while a visible lane closes in place. Call before removeView,
    // while the lane still exists.
    [[nodiscard]] double scrollShiftForColumnRemoval(int columnIndex, int viewportPrimary) const;
    void ensureVisible(int columnIndex, int viewportPrimary);
    void snapVisible(int columnIndex, int viewportPrimary);
    [[nodiscard]] double scrollAmountToEnsureVisible(int columnIndex, int viewportPrimary) const;
    void arrange(const wlr_box& usable) override;
    [[nodiscard]] wlr_box targetBox(const View* view) const override;
    [[nodiscard]] InitialSize initialSize(
        const wlr_box& usable, std::optional<double> ruleWidthFraction, const View* /*splitAnchor*/
    ) const override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;

    [[nodiscard]] uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    [[nodiscard]] uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

    bool setRowBoundary(int columnIndex, int upperRow, double upperWeight, double lowerWeight);
    bool setHeightWeight(int columnIndex, int row, double weight);
    bool setTopGapWeight(int columnIndex, double weight);
    bool setBottomGapWeight(int columnIndex, double weight);
    [[nodiscard]] double heightWeight(int columnIndex, int row) const;
    [[nodiscard]] double topGapWeight(int columnIndex) const;
    [[nodiscard]] double bottomGapWeight(int columnIndex) const;

  private:
    struct Target {
      View* view = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    [[nodiscard]] int totalWidth(int viewportPrimary) const;
    [[nodiscard]] int rawTotalWidth(int viewportPrimary) const;
    [[nodiscard]] int centeringOffset(int viewportPrimary) const;
    [[nodiscard]] double targetScrollForEnsureVisible(int columnIndex, int viewportPrimary, bool force = false) const;
    [[nodiscard]] bool vertical() const;
    [[nodiscard]] bool expandSingleColumn() const;
    void syncHeightWeights(Column& column);

    std::vector<Column> m_columns;
    std::vector<Target> m_targets;
    double m_scroll = 0;
    bool m_centeredRest = false;
    int m_lastViewportPrimary = 0;
    const LayoutSnapshot* m_pendingViewportSnapshot = nullptr;
    View* m_pendingViewportAnchor = nullptr;
    double m_pendingViewportCenterFraction = 0.5;
    bool m_pendingViewportComplete = false;
    // Cross extent available during the last arrange, used to preserve existing
    // pixel sizes when a drop converts an outer gap into another stacked view.
    int m_lastAvailableCross = 0;
  };

} // namespace umbriel
