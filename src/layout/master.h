#pragma once

#include "layout/layout.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class View;

  class MasterStackLayout final : public Layout, public TabbedContainers {
  public:
    struct Area {
      std::vector<View*> views;
      std::vector<double> weights;
      TabState tabs;
    };

    // An empty stack a drop can start beside the master area: `gap` is the gap index openStack takes (0 before every
    // area, columns().size() after them), `box` where arrange puts the stack once a window joins it.
    struct EmptyStack {
      int gap = 0;
      wlr_box box{};
    };

    LayoutMode mode() const override { return LayoutMode::Master; }

    const std::vector<Column>& columns() const override { return m_columns; }
    int columnOf(const View* view) const override;
    int rowOf(const View* view) const override;
    LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) override;

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    std::vector<EmptyStack> emptyStacks(const wlr_box& usable) const;
    // Where arrange puts the master area once a window starts it in an empty layout.
    wlr_box emptyMasterBox(const wlr_box& usable) const;
    // Starts the empty stack at gap index `gap` with `view`, which must not be in the layout. False when no empty stack
    // lies there.
    bool openStack(View* view, int gap);
    bool consume(View* view, int direction) override;
    bool expel(View* view, int direction) override;
    bool consumeFrom(View* view, int direction) override;
    bool moveViewVertical(View* view, int direction) override;
    bool swapViews(View* a, View* b) override;
    bool promoteFromStack();
    bool demoteToStack();
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void arrange(const wlr_box& usable) override;

    wlr_box targetBox(const View* view) const override;
    InitialSize initialSize(
        const wlr_box& usable, bool wantMaximized, std::optional<double> ruleExtent, std::optional<int> ruleExtentPx,
        const View* /*splitAnchor*/
    ) const override;
    std::optional<View*> focusHorizontalLeaf(const View* view, int direction) const override;
    std::optional<View*> focusVerticalLeaf(const View* view, int direction) const override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    bool isFullWidth(int columnIndex) const override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    double widthFraction(int columnIndex) const override;
    double heightFraction(const View* view) const override;
    bool setHeightFraction(View* view, double fraction) override;

    uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

    TabbedContainers* tabbedContainers() override { return this; }
    bool setTabbed(const View* view, bool tabbed) override;
    bool selectTab(const View* view) override;
    bool insertTab(View* view, int column, int row, const View* member) override;
    bool moveTab(const View* view, int direction) override;
    bool setTabBar(const View* view, std::optional<bool> shown) override;

  private:
    // Row changes for every area, keeping the tab selection on its view. An area that fills from empty takes the
    // display new columns start with.
    void insertRow(Area& area, size_t row, View* view, double weight) const;
    double eraseRow(Area& area, size_t row);
    // The row a window joining `area` by consume lands on.
    size_t joinRow(const Area& area) const;
    // The window focus entering `area` from outside lands on: its shown tab when it is tabbed, else its first row.
    View* areaEntry(const Area& area) const;
    // Targets other than hidden tabs, which share the box of the tab on show and would otherwise tie with it.
    std::vector<LayoutTarget> visibleTargets() const;
    // The size a window joining `area`, `width` wide, gets, matching what arrange will assign.
    InitialSize joiningSize(const Area& area, int width, int contentHeight, int gap) const;
    double masterFrac() const;
    bool masterIsLeft() const;
    bool masterIsCenter() const;
    // The areas in visual order, master centered between both stacks in center mode. Unused slots are null.
    std::array<Area*, 3> orderedAreas();
    std::array<const Area*, 3> orderedAreas() const;
    // Where a new stack row goes: the emptier stack in center mode, ties to m_stack.
    Area& insertionStack();
    const Area& insertionStack() const;
    // Where a promotion takes its row from: the fuller stack in center mode, ties to m_stack. Null when every stack
    // is empty.
    Area* promotionStack();
    // Outside center mode the second stack cannot exist, so a reload that leaves center appends it to m_stack.
    void foldSecondStack();
    // Whether primary extent actions have a boundary to move: the master margins in center mode, both areas otherwise.
    bool widthAdjustable() const;
    Area* areaOf(const View* view);
    const Area* areaOf(const View* view) const;
    Area* visualArea(int columnIndex);
    const Area* visualArea(int columnIndex) const;
    // The empty stack at the outer gap index `gap` beside a non-empty master area, else null.
    const Area* emptyStackAt(int gap) const;
    int rowInArea(const Area& area, const View* view) const;
    uint32_t resizableEdges(const View* view) const;
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
