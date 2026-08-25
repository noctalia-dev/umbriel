#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;
  class Layout;
  struct ResolvedLayoutConfig;

  enum class LayoutMode {
    Scrolling,
    Dwindle,
  };

  enum class ScrollingDirection {
    Horizontal,
    Vertical,
  };

  // What a layout needs to know about a view in order to size it: the client's
  // size hints plus whether it is going fullscreen.
  struct LayoutConstraints {
    int minWidth = 1;
    int minHeight = 1;
    int maxWidth = 0;  // 0 = unlimited
    int maxHeight = 0; // 0 = unlimited
    bool fullscreen = false;
    bool maximizedToEdges = false;

    [[nodiscard]] int clampWidth(int width) const {
      width = width < minWidth ? minWidth : width;
      if (maxWidth > 0 && width > maxWidth) {
        width = maxWidth;
      }
      return width < 1 ? 1 : width;
    }

    [[nodiscard]] int clampHeight(int height) const {
      height = height < minHeight ? minHeight : height;
      if (maxHeight > 0 && height > maxHeight) {
        height = maxHeight;
      }
      return height < 1 ? 1 : height;
    }
  };

  // Layouts treat View as an opaque identity. They still need its constraints, so the owning Workspace supplies the
  // lookup rather than layout/ depending on View's definition. Unset means unconstrained, which is what the geometry
  // tests want.
  using LayoutConstraintsFn = LayoutConstraints (*)(const View*);

  // Scrolling interprets widthFrac as the primary-axis extent fraction.
  // heightWeights and edge gap weights divide the cross-axis extent.
  // Dwindle continues to use the fields in its native horizontal/vertical sense.
  struct Column {
    std::vector<View*> views;
    std::vector<double> heightWeights;
    double topGapWeight = 0.0;
    double bottomGapWeight = 0.0;
    double widthFrac = 0.5;
    double savedWidthFrac = 0.0;
  };

  // Layout-owned interactive resize session. Cursor feeds a pointer delta; the session mutates only its layout's
  // geometry state (split ratios, width fractions, row weights). Protocol calls and arrange() stay in Cursor.
  struct ResizeGrab {
    virtual ~ResizeGrab() = default;
    virtual void applyDelta(double dx, double dy, const wlr_box& usable) = 0;
    // Identity of the layout that created this session, for stale-pointer
    // detection when a config reload swaps the workspace's layout mid-grab.
    [[nodiscard]] virtual const Layout* ownerLayout() const = 0;
    // True when the layout cleared a maximized/full-width state at grab start,
    // so Cursor should un-maximize the toplevel immediately (matches legacy).
    [[nodiscard]] virtual bool unmaximizeOnBegin() const { return false; }
  };

  class Layout {
  public:
    virtual ~Layout() = default;
    void setConfig(const ResolvedLayoutConfig* config) { m_config = config; }
    [[nodiscard]] const ResolvedLayoutConfig* layoutConfig() const { return m_config; }

    void setConstraints(LayoutConstraintsFn constraints) { m_constraints = constraints; }
    [[nodiscard]] LayoutConstraints constraintsFor(const View* view) const {
      return m_constraints != nullptr && view != nullptr ? m_constraints(view) : LayoutConstraints{};
    }

    [[nodiscard]] virtual LayoutMode mode() const = 0;

    [[nodiscard]] virtual int columnOf(const View* view) const = 0;
    [[nodiscard]] virtual int rowOf(const View* view) const = 0;

    virtual void insertView(View* view, int columnIndex) = 0;
    virtual void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) = 0;
    virtual bool consumeLeft(View* view) = 0;
    virtual bool expelRight(View* view) = 0;
    virtual bool moveViewVertical(View* view, int direction) = 0;
    virtual void removeView(View* view) = 0;
    virtual void moveColumn(int from, int to) = 0;
    virtual void arrange(const wlr_box& usable) = 0;

    [[nodiscard]] virtual wlr_box targetBox(const View* view) const = 0;

    struct InitialSize {
      int width = 0;
      int height = 0;
    };

    // Size for the very first configure, before the view has joined the layout. It must agree with what arrange() will
    // later assign, or the client's first buffer is the wrong size and the window visibly resizes on its first paint
    // (Electron and friends keep that buffer until they redraw). `ruleWidthFraction` is a window rule's default_width,
    // which is a viewport fraction and so means nothing to a splitting layout.
    [[nodiscard]] virtual InitialSize
    initialSize(const wlr_box& usable, std::optional<double> ruleWidthFraction) const = 0;

    [[nodiscard]] virtual View* focusVerticalLeaf(const View* /*view*/, int /*direction*/) const { return nullptr; }

    virtual bool cycleWidth(int columnIndex, int direction) = 0;
    virtual bool toggleFullWidth(int columnIndex) = 0;
    virtual bool setWidthFraction(int columnIndex, double fraction) = 0;
    virtual void clearFullWidthState(int columnIndex) = 0;
    [[nodiscard]] virtual double widthFraction(int columnIndex) const = 0;

    // Interactive resize    // Edges grabbable at a pointer position (0 = none). Base = not resizable.
    [[nodiscard]] virtual uint32_t resizeEdgesAt(const View* /*view*/, double /*cx*/, double /*cy*/) const { return 0; }
    // Drop edges this layout cannot resize. Base = unchanged.
    [[nodiscard]] virtual uint32_t sanitizeResizeEdges(const View* /*view*/, uint32_t edges) const { return edges; }
    // Resolve the final resize edges: an explicit request is sanitized; an empty
    // request (or one sanitized to nothing) falls back to the pointer proposal.
    [[nodiscard]] uint32_t resolveResizeEdges(const View* view, uint32_t requested, double cx, double cy) const {
      uint32_t edges = requested != 0 ? sanitizeResizeEdges(view, requested) : resizeEdgesAt(view, cx, cy);
      if (edges == 0) {
        edges = resizeEdgesAt(view, cx, cy);
      }
      return edges;
    }
    // Begin an interactive resize with already-resolved edges; null = not resizable.
    virtual std::unique_ptr<ResizeGrab> beginResize(View* /*view*/, uint32_t /*edges*/, const wlr_box& /*usable*/) {
      return nullptr;
    }

    // Both layouts present their contents as columns: scrolling owns them
    // directly, dwindle flattens its tree into them.
    [[nodiscard]] virtual const std::vector<Column>& columns() const = 0;
    // Whether a column occupies the full viewport, however each layout gets there.
    [[nodiscard]] virtual bool isFullWidth(int columnIndex) const = 0;

    // Anything only one layout can answer lives on that layout. Reach it through the single downcast seam,
    // Workspace::scrollingLayout(), rather than by asking every layout a question most of them have no answer to.

  protected:
    // The usable area minus edge padding on both axes: the box the layout has
    // to fill.
    [[nodiscard]] wlr_box contentArea(const wlr_box& usable) const;
    // Gap-aware width of a column occupying `fraction` of the viewport. Solving sum(w) + (N-1)g = V with w = p*(V+g) -
    // g makes N columns whose fractions sum to 1 tile the viewport exactly.
    [[nodiscard]] int fractionalWidth(int viewportPrimary, double fraction) const;

    const ResolvedLayoutConfig* m_config = nullptr;
    LayoutConstraintsFn m_constraints = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Layout> createLayout(LayoutMode mode);

} // namespace umbriel
