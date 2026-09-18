#pragma once

#include "layout/layout.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  class MasterStackLayout final : public Layout {
  public:
    struct Area {
      std::vector<View*> views;
      std::vector<double> weights;
    };

    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Master; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;
    [[nodiscard]] LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consume(View* view, int direction) override;
    bool expel(View* view, int direction) override;
    bool moveViewVertical(View* view, int direction) override;
    bool swapViews(View* a, View* b) override;
    bool promoteFromStack();
    bool demoteToStack();
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void arrange(const wlr_box& usable) override;

    [[nodiscard]] wlr_box targetBox(const View* view) const override;
    [[nodiscard]] InitialSize initialSize(
        const wlr_box& usable, bool wantMaximized, std::optional<double> ruleExtent, std::optional<int> ruleExtentPx,
        const View* /*splitAnchor*/
    ) const override;
    [[nodiscard]] std::optional<View*> focusHorizontalLeaf(const View* view, int direction) const override;
    [[nodiscard]] std::optional<View*> focusVerticalLeaf(const View* view, int direction) const override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    [[nodiscard]] bool isFullWidth(int columnIndex) const override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;
    [[nodiscard]] double heightFraction(const View* view) const override;
    bool setHeightFraction(View* view, double fraction) override;

    [[nodiscard]] uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    [[nodiscard]] uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

  private:
    [[nodiscard]] double masterFrac() const;
    [[nodiscard]] bool masterIsLeft() const;
    [[nodiscard]] bool masterIsCenter() const;
    // The areas in visual order, master centered between both stacks in center mode. Unused slots are null.
    [[nodiscard]] std::array<Area*, 3> orderedAreas();
    [[nodiscard]] std::array<const Area*, 3> orderedAreas() const;
    // Where a new stack row goes: the emptier stack in center mode, ties to m_stack.
    [[nodiscard]] Area& insertionStack();
    [[nodiscard]] const Area& insertionStack() const;
    // Where a promotion takes its row from: the fuller stack in center mode, ties to m_stack. Null when every stack
    // is empty.
    [[nodiscard]] Area* promotionStack();
    // Outside center mode the second stack cannot exist, so a reload that leaves center appends it to m_stack.
    void foldSecondStack();
    // Whether primary extent actions have a boundary to move: the master margins in center mode, both areas otherwise.
    [[nodiscard]] bool widthAdjustable() const;
    [[nodiscard]] Area* areaOf(const View* view);
    [[nodiscard]] const Area* areaOf(const View* view) const;
    [[nodiscard]] Area* visualArea(int columnIndex);
    [[nodiscard]] const Area* visualArea(int columnIndex) const;
    [[nodiscard]] int rowInArea(const Area& area, const View* view) const;
    [[nodiscard]] uint32_t resizableEdges(const View* view) const;
    void eraseFromAreas(View* view);
    void rebuildColumns();

    Area m_master;
    Area m_stack;
    // Center mode splits the stack in two: m_stack is the left column, m_secondStack the right one.
    // Always empty with position "left" or "right".
    Area m_secondStack;
    double m_masterFrac = -1.0;
    double m_savedFrac = 0.0;
    mutable std::vector<Column> m_columns;
    std::vector<LayoutTarget> m_targets;
    // True between a structural change and the arrange that re-sizes the rows: m_targets still hold the old boxes.
    bool m_geometryStale = true;
  };

} // namespace umbriel
