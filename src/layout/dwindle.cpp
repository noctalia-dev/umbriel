#include "layout/dwindle.h"

#include "config/config.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

// wlr_box and WLR_EDGE_* only; see the note in scrolling.cpp.
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace umbriel {

  namespace {

    struct DwindleSnapshot final : LayoutSnapshot {
      struct SavedNode {
        DwindleLayout::Node::Type type = DwindleLayout::Node::Leaf;
        std::unique_ptr<SavedNode> left;
        std::unique_ptr<SavedNode> right;
        double ratio = 0.5;
        bool locked = false;
        LayoutMemberId member = 0;
      };

      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Dwindle; }
      [[nodiscard]] size_t memberCount() const override { return members; }

      std::unique_ptr<SavedNode> root;
      size_t members = 0;
    };

  } // namespace

  DwindleLayout::Node* DwindleLayout::findNode(const View* view) const {
    if (m_root == nullptr) {
      return nullptr;
    }
    std::vector<Node*> stack;
    stack.push_back(m_root.get());
    while (!stack.empty()) {
      Node* node = stack.back();
      stack.pop_back();
      if (node->type == Node::Leaf) {
        if (node->view == view) {
          return node;
        }
      } else {
        if (node->right != nullptr) {
          stack.push_back(node->right.get());
        }
        if (node->left != nullptr) {
          stack.push_back(node->left.get());
        }
      }
    }
    return nullptr;
  }

  DwindleLayout::Node* DwindleLayout::nodeAtFlatIndex(int index) const {
    if (m_root == nullptr || index < 0) {
      return nullptr;
    }
    int count = 0;
    std::vector<Node*> stack;
    stack.push_back(m_root.get());
    Node* node = nullptr;
    while (!stack.empty()) {
      node = stack.back();
      stack.pop_back();
      if (node->type == Node::Leaf) {
        if (count == index) {
          return node;
        }
        ++count;
      } else {
        if (node->right != nullptr) {
          stack.push_back(node->right.get());
        }
        if (node->left != nullptr) {
          stack.push_back(node->left.get());
        }
      }
    }
    return nullptr;
  }

  std::vector<DwindleLayout::WidthSplit> DwindleLayout::widthSplits(Node* node) const {
    std::vector<WidthSplit> splits;
    Node* child = node;
    while (child != nullptr && child->parent != nullptr) {
      Node* parent = child->parent;
      if (parent->type == Node::HSplit) {
        splits.push_back({.node = parent, .first = parent->left.get() == child});
      }
      child = parent;
    }

    double outerProduct = 1.0;
    for (auto& split : std::views::reverse(splits)) {
      split.outerProduct = outerProduct;
      outerProduct *= widthShare(split);
    }
    return splits;
  }

  double DwindleLayout::widthShare(const WidthSplit& split) {
    return split.first ? split.node->ratio : 1.0 - split.node->ratio;
  }

  void DwindleLayout::setWidthShare(const WidthSplit& split, double share) {
    split.node->ratio = split.first ? share : 1.0 - share;
  }

  bool DwindleLayout::applyWidthFraction(const std::vector<WidthSplit>& splits, double fraction) {
    if (splits.empty()) {
      return false;
    }

    const double used = std::clamp(fraction, 0.1, 1.0);
    double innerProduct = 1.0;
    for (const WidthSplit& split : splits) {
      const double denominator = innerProduct * split.outerProduct;
      const double desired = denominator > 0.0 ? used / denominator : std::numeric_limits<double>::infinity();
      const double applied = std::clamp(desired, 0.1, 0.9);
      setWidthShare(split, applied);
      innerProduct *= applied;
      if (applied == desired) {
        break;
      }
    }
    return true;
  }

  void DwindleLayout::splitLeaf(Node* node, View* newView, Node::Type split, bool newFirst) {
    if (node == nullptr) {
      return;
    }
    View* oldView = node->view;
    node->view = nullptr;
    node->type = split;
    node->ratio = 0.5;

    auto first = std::make_unique<Node>();
    first->type = Node::Leaf;
    first->parent = node;

    auto second = std::make_unique<Node>();
    second->type = Node::Leaf;
    second->parent = node;

    first->view = newFirst ? newView : oldView;
    second->view = newFirst ? oldView : newView;

    node->left = std::move(first);
    node->right = std::move(second);
  }

  void DwindleLayout::detachNode(Node* node) {
    if (node == nullptr) {
      return;
    }
    Node* parent = node->parent;
    if (parent == nullptr) {
      m_root.reset();
      return;
    }
    Node* grandparent = parent->parent;
    Node* sibling = (parent->left.get() == node) ? parent->right.get() : parent->left.get();
    if (sibling == nullptr) {
      if (grandparent != nullptr) {
        if (grandparent->left.get() == parent) {
          grandparent->left.reset();
        } else {
          grandparent->right.reset();
        }
      } else {
        m_root.reset();
      }
      return;
    }
    sibling->parent = nullptr;
    std::unique_ptr<Node> ownedSibling;
    if (parent->left.get() == node) {
      ownedSibling = std::move(parent->right);
    } else {
      ownedSibling = std::move(parent->left);
    }
    parent->left.reset();
    parent->right.reset();
    ownedSibling->parent = grandparent;
    if (grandparent != nullptr) {
      if (grandparent->left.get() == parent) {
        grandparent->left = std::move(ownedSibling);
      } else {
        grandparent->right = std::move(ownedSibling);
      }
    } else {
      m_root = std::move(ownedSibling);
    }
  }

  void DwindleLayout::arrangeNode(Node* node, const wlr_box& area) {
    if (node == nullptr || area.width <= 0 || area.height <= 0) {
      return;
    }
    node->areaX = area.x;
    node->areaY = area.y;
    node->areaW = area.width;
    node->areaH = area.height;
    if (node->type == Node::Leaf) {
      if (node->view != nullptr) {
        m_targets.push_back({
            .view = node->view,
            .x = area.x,
            .y = area.y,
            .width = area.width,
            .height = area.height,
        });
      }
      return;
    }

    if (node->type == Node::AutoSplit || (!node->locked && !m_config->dwindle.preserveSplit)) {
      // AutoSplit and unlocked splits follow the longer edge. With preserve_split enabled, the first arrange resolves
      // AutoSplit and then freezes it, while explicit directional drops remain fixed.
      node->type = area.width >= area.height ? Node::HSplit : Node::VSplit;
    }

    const int gap = m_config->totalGap;
    const double ratio = std::clamp(node->ratio, 0.1, 0.9);

    if (node->type == Node::HSplit) {
      const int totalWidth = area.width;
      const int leftWidth = std::max(1, static_cast<int>(std::lround(ratio * totalWidth)) - gap / 2);
      const int rightWidth = std::max(1, totalWidth - leftWidth - gap);

      wlr_box leftArea = area;
      leftArea.width = leftWidth;
      wlr_box rightArea = area;
      rightArea.x = area.x + leftWidth + gap;
      rightArea.width = rightWidth;

      if (node->left != nullptr) {
        arrangeNode(node->left.get(), leftArea);
      }
      if (node->right != nullptr) {
        arrangeNode(node->right.get(), rightArea);
      }
    } else {
      const int totalHeight = area.height;
      const int topHeight = std::max(1, static_cast<int>(std::lround(ratio * totalHeight)) - gap / 2);
      const int bottomHeight = std::max(1, totalHeight - topHeight - gap);

      wlr_box topArea = area;
      topArea.height = topHeight;
      wlr_box bottomArea = area;
      bottomArea.y = area.y + topHeight + gap;
      bottomArea.height = bottomHeight;

      if (node->left != nullptr) {
        arrangeNode(node->left.get(), topArea);
      }
      if (node->right != nullptr) {
        arrangeNode(node->right.get(), bottomArea);
      }
    }
  }

  void DwindleLayout::collectColumns(const Node* node) {
    if (node == nullptr) {
      return;
    }
    if (node->type == Node::Leaf) {
      if (node->view != nullptr) {
        Column col;
        col.views.push_back(node->view);
        col.heightWeights.push_back(1.0);
        col.widthFrac = 0.5;
        m_flatColumns.push_back(std::move(col));
      }
      return;
    }
    if (node->left != nullptr) {
      collectColumns(node->left.get());
    }
    if (node->right != nullptr) {
      collectColumns(node->right.get());
    }
  }

  void DwindleLayout::rebuildFlatColumns() {
    m_flatColumns.clear();
    collectColumns(m_root.get());
  }

  bool DwindleLayout::swapLeafViews(Node* first, Node* second) {
    if (first == nullptr
        || second == nullptr
        || first == second
        || first->type != Node::Leaf
        || second->type != Node::Leaf) {
      return false;
    }
    View* firstView = first->view;
    View* secondView = second->view;
    std::swap(first->view, second->view);
    for (LayoutTarget& target : m_targets) {
      if (target.view == firstView) {
        target.view = secondView;
      } else if (target.view == secondView) {
        target.view = firstView;
      }
    }
    rebuildFlatColumns();
    return true;
  }

  // Public Layout interface
  int DwindleLayout::columnOf(const View* view) const {
    for (size_t i = 0; i < m_flatColumns.size(); ++i) {
      if (!m_flatColumns[i].views.empty() && m_flatColumns[i].views[0] == view) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int DwindleLayout::rowOf(const View* /*view*/) const { return 0; }

  LayoutCapture DwindleLayout::captureState() const {
    auto snapshot = std::make_shared<DwindleSnapshot>();
    LayoutCapture capture{.snapshot = snapshot, .members = {}};
    const auto saveNode = [&capture](auto&& self, const Node* node) -> std::unique_ptr<DwindleSnapshot::SavedNode> {
      if (node == nullptr) {
        return nullptr;
      }
      auto saved = std::make_unique<DwindleSnapshot::SavedNode>();
      saved->type = node->type;
      saved->ratio = node->ratio;
      saved->locked = node->locked;
      if (node->type == Node::Leaf) {
        const auto id = static_cast<LayoutMemberId>(capture.members.size());
        capture.members.push_back({.id = id, .view = node->view});
        saved->member = id;
      } else {
        saved->left = self(self, node->left.get());
        saved->right = self(self, node->right.get());
      }
      return saved;
    };
    snapshot->root = saveNode(saveNode, m_root.get());
    snapshot->members = capture.members.size();
    return capture;
  }

  bool DwindleLayout::restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) {
    const auto* snapshot = dynamic_cast<const DwindleSnapshot*>(&base);
    if (snapshot == nullptr || m_root != nullptr) {
      return false;
    }
    const std::optional<std::vector<View*>> resolved = resolveLayoutMembers(snapshot->memberCount(), members);
    if (!resolved) {
      return false;
    }

    const auto restoreNode =
        [&resolved](auto&& self, const DwindleSnapshot::SavedNode* saved, Node* parent) -> std::unique_ptr<Node> {
      if (saved == nullptr) {
        return nullptr;
      }
      if (saved->type == Node::Leaf) {
        View* view = (*resolved)[static_cast<size_t>(saved->member)];
        if (view == nullptr) {
          return nullptr;
        }
        auto node = std::make_unique<Node>();
        node->type = Node::Leaf;
        node->parent = parent;
        node->view = view;
        return node;
      }

      auto node = std::make_unique<Node>();
      node->type = saved->type;
      node->parent = parent;
      node->ratio = saved->ratio;
      node->locked = saved->locked;
      node->left = self(self, saved->left.get(), node.get());
      node->right = self(self, saved->right.get(), node.get());
      if (node->left == nullptr) {
        if (node->right != nullptr) {
          node->right->parent = parent;
        }
        return std::move(node->right);
      }
      if (node->right == nullptr) {
        node->left->parent = parent;
        return std::move(node->left);
      }
      return node;
    };

    m_root = restoreNode(restoreNode, snapshot->root.get(), nullptr);
    m_targets.clear();
    rebuildFlatColumns();
    return true;
  }

  void DwindleLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || findNode(view) != nullptr) {
      return;
    }
    if (m_root == nullptr) {
      auto node = std::make_unique<Node>();
      node->type = Node::Leaf;
      node->view = view;
      m_root = std::move(node);
      rebuildFlatColumns();
      return;
    }
    const int count = static_cast<int>(m_flatColumns.size());
    const int gap = std::clamp(columnIndex, 0, count);
    if (gap == 0) {
      splitLeaf(nodeAtFlatIndex(0), view, Node::AutoSplit, /*newFirst=*/true);
    } else {
      splitLeaf(nodeAtFlatIndex(gap - 1), view, Node::AutoSplit, /*newFirst=*/false);
    }
    rebuildFlatColumns();
  }

  void DwindleLayout::insertViewIntoColumn(View* view, int columnIndex, int /*rowIndex*/) {
    if (view == nullptr || findNode(view) != nullptr) {
      return;
    }
    if (m_root == nullptr) {
      auto node = std::make_unique<Node>();
      node->type = Node::Leaf;
      node->view = view;
      m_root = std::move(node);
      rebuildFlatColumns();
      return;
    }
    const int count = static_cast<int>(m_flatColumns.size());
    Node* target = nodeAtFlatIndex(std::clamp(columnIndex, 0, count - 1));
    if (target != nullptr && target->type == Node::Leaf) {
      splitLeaf(target, view, Node::AutoSplit, /*newFirst=*/false);
    }
    rebuildFlatColumns();
  }

  bool DwindleLayout::consumeLeft(View* view) {
    Node* a = findNode(view);
    const int col = columnOf(view);
    if (a == nullptr || col <= 0) {
      return false;
    }
    Node* b = nodeAtFlatIndex(col - 1);
    if (b == nullptr || b->type != Node::Leaf) {
      return false;
    }
    return swapLeafViews(a, b);
  }

  bool DwindleLayout::expelRight(View* view) {
    Node* a = findNode(view);
    const int col = columnOf(view);
    if (a == nullptr || col < 0 || col + 1 >= static_cast<int>(m_flatColumns.size())) {
      return false;
    }
    Node* b = nodeAtFlatIndex(col + 1);
    if (b == nullptr || b->type != Node::Leaf) {
      return false;
    }
    return swapLeafViews(a, b);
  }

  bool DwindleLayout::moveViewVertical(View* view, int direction) {
    return swapLeafViews(findNode(view), findNode(umbriel::directionalNeighbor(m_targets, view, false, direction)));
  }

  void DwindleLayout::removeView(View* view) {
    Node* node = findNode(view);
    if (node == nullptr) {
      return;
    }
    detachNode(node);
    std::erase_if(m_targets, [view](const LayoutTarget& t) { return t.view == view; });
    rebuildFlatColumns();
  }

  void DwindleLayout::moveColumn(int from, int to) {
    const int count = static_cast<int>(m_flatColumns.size());
    if (from < 0 || from >= count) {
      return;
    }
    const int destination = std::clamp(to, 0, count - 1);
    if (from == destination) {
      return;
    }
    Node* a = nodeAtFlatIndex(from);
    Node* b = nodeAtFlatIndex(destination);
    swapLeafViews(a, b);
  }

  void DwindleLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    const int edgePad = m_config->edgePad;
    wlr_box area{
        .x = usable.x + edgePad,
        .y = usable.y + edgePad,
        .width = std::max(1, usable.width - 2 * edgePad),
        .height = std::max(1, usable.height - 2 * edgePad),
    };
    if (m_root != nullptr) {
      arrangeNode(m_root.get(), area);
    }
    rebuildFlatColumns();
  }

  Layout::InitialSize DwindleLayout::initialSize(
      const wlr_box& usable, std::optional<double> /*ruleWidthFraction*/, const View* splitAnchor
  ) const {
    const wlr_box content = contentArea(usable);
    // A window rule's default_width is a viewport fraction, which a splitting layout has no use for. The first leaf
    // owns the whole area.
    if (m_flatColumns.empty()) {
      return {.width = content.width, .height = content.height};
    }
    // A focused insert splits the anchor leaf. Without a tiled anchor, the caller appends and splits the last leaf.
    const Node* target = findNode(splitAnchor);
    if (target == nullptr || target->type != Node::Leaf) {
      target = nodeAtFlatIndex(static_cast<int>(m_flatColumns.size()) - 1);
    }
    const int width = target != nullptr && target->areaW > 0 ? target->areaW : content.width;
    const int height = target != nullptr && target->areaH > 0 ? target->areaH : content.height;
    const auto half = [gap = m_config->totalGap](int span) {
      return std::max(1, static_cast<int>(std::lround(0.5 * span)) - gap / 2);
    };
    if (width >= height) {
      return {.width = half(width), .height = height};
    }
    return {.width = width, .height = half(height)};
  }

  wlr_box DwindleLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const LayoutTarget& t) { return t.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  int DwindleLayout::leafIndexAt(double cx, double cy) const {
    for (int i = 0; i < static_cast<int>(m_targets.size()); ++i) {
      const LayoutTarget& t = m_targets[i];
      if (cx >= t.x && cx < t.x + t.width && cy >= t.y && cy < t.y + t.height) {
        return i;
      }
    }
    return -1;
  }

  wlr_box DwindleLayout::targetBoxByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_targets.size())) {
      return {};
    }
    const LayoutTarget& t = m_targets[static_cast<size_t>(index)];
    return {.x = t.x, .y = t.y, .width = t.width, .height = t.height};
  }

  std::optional<View*> DwindleLayout::focusHorizontalLeaf(const View* view, int direction) const {
    return umbriel::directionalNeighbor(m_targets, view, true, direction);
  }

  std::optional<View*> DwindleLayout::focusVerticalLeaf(const View* view, int direction) const {
    return umbriel::directionalNeighbor(m_targets, view, false, direction);
  }

  bool DwindleLayout::cycleWidth(int columnIndex, int direction) {
    Node* node = nodeAtFlatIndex(columnIndex);
    const std::vector<WidthSplit> splits = widthSplits(node);
    if (splits.empty()) {
      return false;
    }
    double current = 1.0;
    for (const WidthSplit& split : splits) {
      current *= widthShare(split);
    }
    const auto& presets = m_config->widthPresets;
    double next = 0.0;
    if (direction < 0) {
      const auto it = std::ranges::find_if(presets | std::views::reverse, [current](double preset) {
        return preset < current - 0.0001;
      });
      next = it == presets.rend() ? presets.back() : *it;
    } else {
      const auto it = std::ranges::find_if(presets, [current](double preset) { return preset > current + 0.0001; });
      next = it == presets.end() ? presets[0] : *it;
    }
    return applyWidthFraction(splits, next);
  }

  bool DwindleLayout::toggleFullWidth(int columnIndex) {
    Node* node = nodeAtFlatIndex(columnIndex);
    const std::vector<WidthSplit> splits = widthSplits(node);
    if (splits.empty()) {
      return false;
    }
    double current = 1.0;
    double maximum = 1.0;
    for (const WidthSplit& split : splits) {
      current *= widthShare(split);
      maximum *= 0.9;
    }
    if (current >= maximum - 0.0001) {
      applyWidthFraction(splits, 0.5);
      return false;
    }
    applyWidthFraction(splits, 1.0);
    return true;
  }

  bool DwindleLayout::isFullWidth(int columnIndex) const {
    const std::vector<WidthSplit> splits = widthSplits(nodeAtFlatIndex(columnIndex));
    if (splits.empty()) {
      return false;
    }
    double current = 1.0;
    double maximum = 1.0;
    for (const WidthSplit& split : splits) {
      current *= widthShare(split);
      maximum *= 0.9;
    }
    return current >= maximum - 0.0001;
  }

  bool DwindleLayout::setWidthFraction(int columnIndex, double fraction) {
    return applyWidthFraction(widthSplits(nodeAtFlatIndex(columnIndex)), fraction);
  }

  void DwindleLayout::clearFullWidthState(int /*columnIndex*/) {}

  double DwindleLayout::widthFraction(int columnIndex) const {
    const std::vector<WidthSplit> splits = widthSplits(nodeAtFlatIndex(columnIndex));
    if (splits.empty()) {
      return 1.0;
    }
    double fraction = 1.0;
    for (const WidthSplit& split : splits) {
      fraction *= widthShare(split);
    }
    return fraction;
  }

  // Drag-and-drop directional insertion
  void DwindleLayout::insertViewSplitOnView(View* newView, View* targetView, uint32_t edge) {
    if (newView == nullptr || findNode(newView) != nullptr) {
      return;
    }
    Node* target = findNode(targetView);
    if (target == nullptr || target->type != Node::Leaf) {
      return;
    }
    if (edge == 0) {
      splitLeaf(target, newView, Node::AutoSplit, /*newFirst=*/false);
      rebuildFlatColumns();
      return;
    }
    const bool horizontal = (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0;
    const bool newFirst = (edge & (WLR_EDGE_LEFT | WLR_EDGE_TOP)) != 0;
    splitLeaf(target, newView, horizontal ? Node::HSplit : Node::VSplit, newFirst);
    target->locked = true;
    rebuildFlatColumns();
  }

  // Interactive resize
  DwindleLayout::Node* DwindleLayout::boundaryNode(const View* view, uint32_t edge) const {
    Node* node = findNode(view);
    if (node == nullptr) {
      return nullptr;
    }
    // LEFT/RIGHT edges ride a horizontal split; TOP/BOTTOM ride a vertical one.
    // LEFT/TOP mean the view sits in the split's second (right/bottom) child.
    const bool wantHorizontal = (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0;
    const bool wantSecond = (edge & (WLR_EDGE_LEFT | WLR_EDGE_TOP)) != 0;
    Node* current = node;
    while (current->parent != nullptr) {
      Node* parent = current->parent;
      const bool horizontal = parent->type == Node::HSplit;
      if (horizontal == wantHorizontal) {
        const bool second = parent->right.get() == current;
        if (second == wantSecond) {
          return parent;
        }
      }
      current = parent;
    }
    return nullptr;
  }

  uint32_t DwindleLayout::resizableEdges(const View* view) const {
    uint32_t edges = 0;
    for (const uint32_t edge : {WLR_EDGE_LEFT, WLR_EDGE_RIGHT, WLR_EDGE_TOP, WLR_EDGE_BOTTOM}) {
      if (boundaryNode(view, edge) != nullptr) {
        edges |= edge;
      }
    }
    return edges;
  }

  bool DwindleLayout::resizeBoundary(const View* view, uint32_t edge, double* outRatio, double* outSpan) const {
    Node* parent = boundaryNode(view, edge);
    if (parent == nullptr) {
      return false;
    }
    const bool horizontal = parent->type == Node::HSplit;
    const double span = horizontal ? parent->areaW : parent->areaH;
    if (span <= 0) {
      return false;
    }
    if (outRatio != nullptr) {
      *outRatio = parent->ratio;
    }
    if (outSpan != nullptr) {
      *outSpan = span;
    }
    return true;
  }

  bool DwindleLayout::setResizeBoundary(View* view, uint32_t edge, double ratio) {
    Node* parent = boundaryNode(view, edge);
    if (parent == nullptr) {
      return false;
    }
    parent->ratio = std::clamp(ratio, 0.05, 0.95);
    return true;
  }

  namespace {

    // Layout-owned resize session: nudges the split ratios behind the grabbed
    // horizontal/vertical edges by (delta / boundary span).
    class DwindleResizeGrab : public ResizeGrab {
    public:
      DwindleResizeGrab(
          DwindleLayout* layout, View* view, bool hActive, uint32_t hEdge, double hRatio, double hSpan, bool vActive,
          uint32_t vEdge, double vRatio, double vSpan
      )
          : m_layout(layout), m_view(view), m_hActive(hActive), m_hEdge(hEdge), m_hRatio(hRatio), m_hSpan(hSpan),
            m_vActive(vActive), m_vEdge(vEdge), m_vRatio(vRatio), m_vSpan(vSpan) {}

      void applyDelta(double dx, double dy, const wlr_box& /*usable*/) override {
        if (m_hActive && m_hSpan > 0) {
          m_layout->setResizeBoundary(m_view, m_hEdge, m_hRatio + dx / m_hSpan);
        }
        if (m_vActive && m_vSpan > 0) {
          m_layout->setResizeBoundary(m_view, m_vEdge, m_vRatio + dy / m_vSpan);
        }
      }

      [[nodiscard]] const Layout* ownerLayout() const override { return m_layout; }

    private:
      DwindleLayout* m_layout;
      View* m_view;
      bool m_hActive;
      uint32_t m_hEdge;
      double m_hRatio;
      double m_hSpan;
      bool m_vActive;
      uint32_t m_vEdge;
      double m_vRatio;
      double m_vSpan;
    };

  } // namespace

  uint32_t DwindleLayout::resizeEdgesAt(const View* view, double cx, double cy) const {
    const wlr_box box = targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    return sanitizeResizeEdges(view, resizeEdgesForPoint(box, cx, cy));
  }

  uint32_t DwindleLayout::sanitizeResizeEdges(const View* view, uint32_t edges) const {
    return edges & resizableEdges(view);
  }

  std::unique_ptr<ResizeGrab> DwindleLayout::beginResize(View* view, uint32_t edges, const wlr_box& /*usable*/) {
    bool hActive = false;
    bool vActive = false;
    uint32_t hEdge = 0;
    uint32_t vEdge = 0;
    double hRatio = 0;
    double hSpan = 0;
    double vRatio = 0;
    double vSpan = 0;
    double ratio = 0;
    double span = 0;
    if ((edges & WLR_EDGE_LEFT) != 0 && resizeBoundary(view, WLR_EDGE_LEFT, &ratio, &span)) {
      hActive = true;
      hEdge = WLR_EDGE_LEFT;
      hRatio = ratio;
      hSpan = span;
    } else if ((edges & WLR_EDGE_RIGHT) != 0 && resizeBoundary(view, WLR_EDGE_RIGHT, &ratio, &span)) {
      hActive = true;
      hEdge = WLR_EDGE_RIGHT;
      hRatio = ratio;
      hSpan = span;
    }
    if ((edges & WLR_EDGE_TOP) != 0 && resizeBoundary(view, WLR_EDGE_TOP, &ratio, &span)) {
      vActive = true;
      vEdge = WLR_EDGE_TOP;
      vRatio = ratio;
      vSpan = span;
    } else if ((edges & WLR_EDGE_BOTTOM) != 0 && resizeBoundary(view, WLR_EDGE_BOTTOM, &ratio, &span)) {
      vActive = true;
      vEdge = WLR_EDGE_BOTTOM;
      vRatio = ratio;
      vSpan = span;
    }
    if (!hActive && !vActive) {
      return nullptr;
    }
    return std::make_unique<DwindleResizeGrab>(
        this, view, hActive, hEdge, hRatio, hSpan, vActive, vEdge, vRatio, vSpan
    );
  }

} // namespace umbriel
