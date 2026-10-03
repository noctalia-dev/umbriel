#pragma once

#include "layout/layout.h"

#include <optional>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  // Public width/X names refer to the primary scroll axis and height to the cross axis. Horizontal layouts map
  // primary/cross to X/Y, vertical layouts to Y/X.

  class ScrollingLayout : public Layout, public TabbedContainers {
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
    bool consume(View* view, int direction) override;
    bool expel(View* view, int direction) override;
    bool consumeFrom(View* view, int direction) override;
    bool moveViewVertical(View* view, int direction) override;
    bool swapViews(View* a, View* b) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    // Raw scroll mutation. `centeredRest` is true only when restoring a saved column-center resting position.
    void setScroll(double scroll, bool centeredRest = false);
    bool centerColumn(int columnIndex, int viewportPrimary);
    // Clamps the offset into [0, maxScroll]. A centered rest may lie outside that range, but only as far as centering
    // the first or last column does.
    void clampScroll(int viewportPrimary);
    void reconcileFocusedColumn(int columnIndex, int viewportPrimary);
    [[nodiscard]] bool centeredRest() const { return m_centeredRest; }
    // How much to subtract from the scroll offset when `columnIndex` is about
    // to lose its last view. Removing a lane closes the primary-axis space it
    // held. Compensation re-anchors content when that space was hidden toward
    // strip start, while a visible lane closes in place. Call before removeView,
    // while the lane still exists.
    [[nodiscard]] double scrollShiftForColumnRemoval(int columnIndex, int viewportPrimary) const;
    void ensureVisible(int columnIndex, int viewportPrimary);
    void activateColumn(int columnIndex, int viewportPrimary, int previousIndex = -1);
    // Notes that the focused column is the one about to leave. Call before the detach, while the closing view still
    // names its column.
    void noteRemovalOfFocusedColumn(int columnIndex);
    // Re-applies the centering policy to a column whose extent just changed.
    void reevaluateColumn(int columnIndex, int viewportPrimary);
    // Re-applies it to the survivor of a removed focused column, against the column that took the removed one's place.
    void reevaluateAfterRemoval(int columnIndex, int viewportPrimary);
    void snapVisible(int columnIndex, int viewportPrimary);
    [[nodiscard]] double scrollAmountToEnsureVisible(int columnIndex, int viewportPrimary) const;
    void arrange(const wlr_box& usable) override;
    [[nodiscard]] wlr_box targetBox(const View* view) const override;
    [[nodiscard]] InitialSize initialSize(
        const wlr_box& usable, bool wantMaximized, std::optional<double> ruleExtent, std::optional<int> ruleExtentPx,
        const View* /*splitAnchor*/
    ) const override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;
    [[nodiscard]] double heightFraction(const View* view) const override;
    bool setHeightFraction(View* view, double fraction) override;

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

    [[nodiscard]] TabbedContainers* tabbedContainers() override { return this; }
    bool setTabbed(const View* view, bool tabbed) override;
    bool selectTab(const View* view) override;
    bool insertTab(View* view, int column, int row, const View* member) override;
    bool moveTab(const View* view, int direction) override;
    bool setTabBar(const View* view, std::optional<bool> shown) override;
    // Restore the tab groups of a column rebuilt row by row, replacing its initial display mode.
    // False when `tabs` reaches past the column's rows.
    bool adoptTabs(int columnIndex, const ColumnTabs& tabs);

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
    [[nodiscard]] double centeredScroll(int columnIndex, int viewportPrimary) const;
    [[nodiscard]] double
    targetScrollForEnsureVisible(int columnIndex, int viewportPrimary, bool center, bool force = false) const;
    // Which neighbor stands in for the side focus came from. A direction rather than an index: a width change is judged
    // long after the activation that set it, by which time the strip may have gained or lost columns.
    enum class FocusSide { None, FromLeft, FromRight };
    [[nodiscard]] bool alwaysCentersFocus() const;
    // Side a focus move from the focused column to `columnIndex` would come from.
    [[nodiscard]] FocusSide focusSideFrom(int columnIndex) const;
    // Side an extent change measures its pair from, with no focus move left to read it.
    [[nodiscard]] FocusSide reevaluationSide(int columnIndex) const;
    [[nodiscard]] bool shouldCenterFocusedColumn(int columnIndex, int viewportPrimary, FocusSide side) const;
    [[nodiscard]] bool shouldCenterOnOverflow(int columnIndex, int viewportPrimary, FocusSide side) const;
    [[nodiscard]] int focusNeighbor(int columnIndex, FocusSide side) const;
    // Scroll putting `columnIndex` and `neighbor` side by side at the edge `neighbor` sits on, for a pair known to fit.
    [[nodiscard]] double pairScroll(int columnIndex, int viewportPrimary, int neighbor) const;
    // Shared body of ensureVisible, activateColumn and applyCenteringPolicy: they differ only in which centering policy
    // applies, and `force` only in whether an already visible column may still be moved.
    void revealColumn(int columnIndex, int viewportPrimary, bool center, bool force = false);
    // Shared body of reevaluateColumn and snapVisible, which differ only in `force`.
    void applyCenteringPolicy(int columnIndex, int viewportPrimary, bool force);
    [[nodiscard]] bool vertical() const;
    void syncHeightWeights(Column& column);
    // Targets for the tabs of `group`: every one in `unit`, the group's share of the column, less its bar.
    void pushTabTargets(const Column& column, const TabGroup& group, const wlr_box& unit);
    // Where a window joining `column` by consume lands: among the tabs of the column's last unit when that is a tab
    // group, after its shown tab when new tabs go there, otherwise in a row of its own at the end.
    struct JoinPoint {
      size_t row = 0;
      std::optional<size_t> group;
    };
    [[nodiscard]] JoinPoint joinPoint(const Column& column) const;
    // Weight for a row being added to `column` at `row`, taking over the column's edge gap when the row lands against
    // one. Shared by fresh inserts and by consume, so free space always becomes the incoming row's extent.
    double claimInsertWeight(Column& column, int row, double fallbackWeight);

    std::vector<Column> m_columns;
    std::vector<Target> m_targets;
    double m_scroll = 0;
    bool m_centeredRest = false;
    // Whether that rest came from the centering policy rather than from a column-center the user asked for.
    bool m_policyCenteredRest = false;
    // Column the last activation focused, so CenterFocusedColumn::OnOverflow knows which side focus came from.
    int m_lastFocusedColumn = -1;
    // Side that activation came from, so a later width change judges the pair the focus move was judged by.
    FocusSide m_focusSide = FocusSide::None;
    // Set when the focused column itself left the strip, so the survivor is judged once the removal has settled.
    bool m_pendingRemovalReevaluate = false;
    // Column the pending removal took, kept to tell which side the survivor inherits.
    int m_removedFocusedColumn = -1;
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
