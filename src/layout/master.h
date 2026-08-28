#pragma once

#include "layout/layout.h"

#include <cstdint>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  class MasterStackLayout final : public Layout {
  public:
    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Master; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;
    [[nodiscard]] LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consumeLeft(View* view) override;
    bool expelRight(View* view) override;
    bool moveViewVertical(View* view, int direction) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void arrange(const wlr_box& usable) override;

    [[nodiscard]] wlr_box targetBox(const View* view) const override;
    [[nodiscard]] InitialSize initialSize(
        const wlr_box& usable, std::optional<double> ruleWidthFraction, const View* /*splitAnchor*/
    ) const override;
    [[nodiscard]] std::optional<View*> focusHorizontalLeaf(const View* view, int direction) const override;
    [[nodiscard]] std::optional<View*> focusVerticalLeaf(const View* view, int direction) const override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    [[nodiscard]] bool isFullWidth(int columnIndex) const override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;

    [[nodiscard]] uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    [[nodiscard]] uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

  private:
    struct Area {
      std::vector<View*> views;
      std::vector<double> weights;
    };

    [[nodiscard]] double masterFrac() const;
    [[nodiscard]] bool masterIsLeft() const;
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
    double m_masterFrac = -1.0;
    double m_savedFrac = 0.0;
    mutable std::vector<Column> m_columns;
    std::vector<LayoutTarget> m_targets;
  };

} // namespace umbriel
