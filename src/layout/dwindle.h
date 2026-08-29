#pragma once

#include "layout/layout.h"

#include <cstdint>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  class DwindleLayout : public Layout {
  public:
    struct Node {
      // AutoSplit is a split whose axis follows its area on arrange unless preserve_split or an explicit directional
      // drop locks it. The tree is built before any arrange (Workspace::applyConfig batch-inserts on a fresh layout),
      // so the axis cannot be decided at insertion time without guessing the output's orientation.
      enum Type : uint8_t { Leaf, AutoSplit, HSplit, VSplit };
      Type type = Leaf;
      std::unique_ptr<Node> left;
      std::unique_ptr<Node> right;
      Node* parent = nullptr;
      double ratio = 0.5;
      View* view = nullptr;
      bool locked = false;
      int areaX = 0;
      int areaY = 0;
      int areaW = 0;
      int areaH = 0;
    };

    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Dwindle; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_flatColumns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;
    [[nodiscard]] LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;

    // Inserts at a gap index: the preceding leaf is split, except gap 0 which splits the first leaf with the new view
    // first.
    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consume(View* view, int direction) override;
    bool expel(View* view, int direction) override;
    bool moveViewVertical(View* view, int direction) override;
    bool swapViews(View* a, View* b) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void arrange(const wlr_box& usable) override;
    [[nodiscard]] wlr_box targetBox(const View* view) const override;

    [[nodiscard]] InitialSize
    initialSize(const wlr_box& usable, std::optional<double> ruleWidthFraction, const View* splitAnchor) const override;

    [[nodiscard]] int leafIndexAt(double cx, double cy) const;
    [[nodiscard]] wlr_box targetBoxByIndex(int index) const;
    [[nodiscard]] std::optional<View*> focusHorizontalLeaf(const View* view, int direction) const override;
    [[nodiscard]] std::optional<View*> focusVerticalLeaf(const View* view, int direction) const override;

    // Drag-and-drop: split the target leaf and place the new view on the given
    // WLR edge (0 = default/automatic orientation, new view last).
    void insertViewSplitOnView(View* newView, View* targetView, uint32_t edge);

    // Interactive resize: which edges of the view border an internal split (screen-facing edges are excluded), the
    // current ratio/pixel span of the boundary behind an edge, and a setter for that boundary ratio.
    [[nodiscard]] uint32_t resizableEdges(const View* view) const;
    [[nodiscard]] bool resizeBoundary(const View* view, uint32_t edge, double* outRatio, double* outSpan) const;
    bool setResizeBoundary(View* view, uint32_t edge, double ratio);

    [[nodiscard]] uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    [[nodiscard]] uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

    bool cycleWidth(int columnIndex, int direction) override;
    bool toggleFullWidth(int columnIndex) override;
    [[nodiscard]] bool isFullWidth(int columnIndex) const override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;
    [[nodiscard]] double heightFraction(const View* view) const override;
    bool setHeightFraction(View* view, double fraction) override;

  private:
    struct Split {
      Node* node = nullptr;
      bool first = false;
      double outerProduct = 1.0;
    };

    [[nodiscard]] Node* findNode(const View* view) const;
    [[nodiscard]] Node* nodeAtFlatIndex(int index) const;
    [[nodiscard]] std::vector<Split> splits(Node* node, Node::Type type) const;
    [[nodiscard]] static double splitShare(const Split& split);
    static void setSplitShare(const Split& split, double share);
    bool applyFraction(const std::vector<Split>& splits, double fraction);
    void splitLeaf(Node* node, View* newView, Node::Type split, bool newFirst);
    [[nodiscard]] Node* boundaryNode(const View* view, uint32_t edge) const;
    void arrangeNode(Node* node, const wlr_box& area);
    void collectColumns(const Node* node);
    bool swapLeafViews(Node* first, Node* second);
    // Refreshes the flat-column cache. Every operation that changes the tree or reassigns a leaf's view must call this
    // before returning: insertView reads the cache to locate its target leaf, so a caller that inserts twice with no
    // arrange() in between would otherwise silently drop the second view.
    void rebuildFlatColumns();
    void detachNode(Node* node);

    std::unique_ptr<Node> m_root;
    mutable std::vector<Column> m_flatColumns;
    mutable std::vector<LayoutTarget> m_targets;
  };

} // namespace umbriel
