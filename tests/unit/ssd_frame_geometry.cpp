#include "scene/ssd_frame_geometry.h"

#include "check.h"

#include <array>
#include <span>

extern "C" {
#include <wlr/util/edges.h>
}

using namespace umbriel;

namespace {
  constexpr std::array kAll{TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
  constexpr std::span<const TitleBarButton> kNone{};
} // namespace

UMBRIEL_TEST(rightButtonsPutLastListedOutermost) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 459);
  CHECK_EQ(parts.button(TitleBarButton::Close)->y, 7);
  CHECK_EQ(parts.button(TitleBarButton::Close)->width, 14);
  CHECK_EQ(parts.button(TitleBarButton::Close)->height, 14);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 437);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 415);
  CHECK_EQ(parts.title.x, 8);
  CHECK_EQ(parts.title.width, 399);
}

UMBRIEL_TEST(leftButtonsMirror) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 7);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 29);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 51);
  CHECK_EQ(parts.title.x, 73);
  CHECK_EQ(parts.title.width, 399);
}

UMBRIEL_TEST(omittedButtonsLeaveNoGapAndWidenTheTitle) {
  constexpr std::array order{TitleBarButton::Close, TitleBarButton::Minimize};
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, order);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 459);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 437);
  CHECK(!parts.button(TitleBarButton::Maximize).has_value());
  CHECK_EQ(parts.title.width, 421);
  for (const auto side : {TitleBarButtonSide::Right, TitleBarButtonSide::Left}) {
    const auto bare = titleBarLayout(480, 28, side, kNone);
    CHECK(!bare.button(TitleBarButton::Close).has_value());
    CHECK_EQ(bare.title.x, 8);
    CHECK_EQ(bare.title.width, 464);
  }
}

UMBRIEL_TEST(buttonSizesAndDefaultMargins) {
  constexpr std::array heights{24, 27, 28, 30, 60, 120};
  constexpr std::array sizes{12, 13, 14, 14, 30, 60};
  constexpr std::array margins{6, 7, 7, 8, 15, 30};
  for (size_t i = 0; i < heights.size(); ++i) {
    CHECK_EQ(titleBarButtonDiameter(heights[i]), sizes[i]);
    const auto parts = titleBarLayout(480, heights[i], TitleBarButtonSide::Right, kAll);
    CHECK_EQ(parts.gap, 8);
    CHECK_EQ(480 - parts.button(TitleBarButton::Close)->x - sizes[i], margins[i]);
  }
  CHECK_EQ(titleBarButtonDiameter(28, 12), 12);
  CHECK_EQ(titleBarButtonDiameter(28, 13), 12);
  CHECK_EQ(titleBarButtonDiameter(28, 1), 2);
  CHECK_EQ(titleBarButtonDiameter(28, 0), 2);
  CHECK_EQ(titleBarButtonDiameter(28, -1), 14);
  CHECK_EQ(titleBarButtonDiameter(28, 100), 28);
}

UMBRIEL_TEST(explicitGeometryShrinksMarginThenGapAndMirrors) {
  constexpr std::array order{TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
  const auto marginOnly = titleBarLayout(60, 30, TitleBarButtonSide::Left, order, 12, 8, 13);
  CHECK_EQ(marginOnly.button(TitleBarButton::Close)->x, 8);
  CHECK_EQ(marginOnly.gap, 8);
  const auto right = titleBarLayout(50, 30, TitleBarButtonSide::Right, order, 12, 8, 13);
  const auto left = titleBarLayout(50, 30, TitleBarButtonSide::Left, order, 12, 8, 13);
  CHECK_EQ(right.button(TitleBarButton::Close)->x, 38);
  CHECK_EQ(left.button(TitleBarButton::Close)->x, 0);
  CHECK_EQ(right.gap, 7);
  for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
    const auto parts = titleBarLayout(10, 30, side, order, 12, 8, 13);
    CHECK_EQ(parts.gap, 0);
    CHECK_EQ(parts.button(TitleBarButton::Close)->width, 12);
    CHECK(parts.button(TitleBarButton::Minimize).has_value());
    CHECK_EQ(ssdFramePointAt(10, 300, 30, 2, true, side, order, 5, -15, 12, 8, 13).part, SsdFramePart::Close);
  }
  const auto empty = titleBarLayout(20, 30, TitleBarButtonSide::Left, kNone, 12, 8, 13);
  CHECK_EQ(empty.gap, 8);
  CHECK(!empty.button(TitleBarButton::Close).has_value());
}

UMBRIEL_TEST(buttonHitSpansHalfConfiguredGap) {
  static constexpr std::array one{TitleBarButton::Close};
  const auto hit = [](double x) {
    return ssdFramePointAt(100, 300, 30, 2, true, TitleBarButtonSide::Right, one, x, -15, 12, 8, 13).part;
  };
  CHECK_EQ(hit(71), SsdFramePart::Close);
  CHECK_EQ(hit(70), SsdFramePart::TitleBar);
  CHECK_EQ(hit(90), SsdFramePart::Close);
  CHECK_EQ(hit(91), SsdFramePart::TitleBar);
}

UMBRIEL_TEST(buttonsSitOnTheBarCenterAtEveryHeight) {
  for (int height = 16; height <= 128; ++height) {
    const auto parts = titleBarLayout(480, height, TitleBarButtonSide::Right, kAll);
    const wlr_box close = *parts.button(TitleBarButton::Close);
    CHECK_EQ(2 * close.y + close.height, height);
    CHECK_EQ(480 - close.x - close.width, close.y);
    CHECK(close.height >= 8 && close.height <= (height + 1) / 2);
  }
}

UMBRIEL_TEST(buttonHitSpansTitleBarHeight) {
  const auto at = [](TitleBarButtonSide side, std::span<const TitleBarButton> shown, double x, double y) {
    return ssdFramePointAt(480, 300, 28, 2, true, side, shown, x, y).part;
  };
  for (const double y : {-28.0, -14.0, -0.1}) {
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 466, y), SsdFramePart::Close);
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 444, y), SsdFramePart::Maximize);
    CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 422, y), SsdFramePart::Minimize);
    CHECK_EQ(at(TitleBarButtonSide::Left, kAll, 14, y), SsdFramePart::Close);
  }
  CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 466, 0), SsdFramePart::None);
  CHECK_EQ(at(TitleBarButtonSide::Right, kAll, 200, -15), SsdFramePart::TitleBar);
  // A hidden button's place belongs to the bar.
  CHECK_EQ(at(TitleBarButtonSide::Right, kNone, 466, -15), SsdFramePart::TitleBar);
}

UMBRIEL_TEST(resizeBandOnlyWhenResizable) {
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, false, TitleBarButtonSide::Right, kAll, -3, 100).part, SsdFramePart::None);
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -3, 100).edges, uint32_t{WLR_EDGE_LEFT}
  );
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -8.1, 100).part, SsdFramePart::None);
}

UMBRIEL_TEST(resizeBandWithoutTitleBar) {
  const auto at = [](bool resizable, double x, double y) {
    return ssdFramePointAt(480, 300, 0, 2, resizable, TitleBarButtonSide::Right, kAll, x, y);
  };
  CHECK_EQ(at(true, -3, 100).part, SsdFramePart::Resize);
  CHECK_EQ(at(true, -3, 100).edges, uint32_t{WLR_EDGE_LEFT});
  CHECK_EQ(at(true, 483, 100).edges, uint32_t{WLR_EDGE_RIGHT});
  CHECK_EQ(at(true, 200, -3).edges, uint32_t{WLR_EDGE_TOP});
  CHECK_EQ(at(true, 200, 303).edges, uint32_t{WLR_EDGE_BOTTOM});
  CHECK_EQ(at(true, -3, -3).edges, uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP});
  CHECK_EQ(at(true, 483, 303).edges, uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM});
  CHECK_EQ(at(false, -3, 100).part, SsdFramePart::None);
  CHECK_EQ(at(true, 200, 100).part, SsdFramePart::None);
  CHECK_EQ(at(true, 466, 0).part, SsdFramePart::None);
  CHECK_EQ(at(true, -8.1, 100).part, SsdFramePart::None);
}

UMBRIEL_TEST(cornerExtensionAddsBothEdges) {
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, -3, -15).edges,
      uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP}
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, 470, 302).edges,
      uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM}
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, 200, -31).edges, uint32_t{WLR_EDGE_TOP}
  );
}

UMBRIEL_TEST(contentBoxInTileRemovesTitleBar) {
  const auto content = contentBoxInTile({8, 8, 1264, 704}, 28);
  CHECK_EQ(content.x, 8);
  CHECK_EQ(content.y, 36);
  CHECK_EQ(content.width, 1264);
  CHECK_EQ(content.height, 676);
  CHECK_EQ(contentBoxInTile({-200, 40, 400, 12}, 28).height, 1);
  CHECK_EQ(contentBoxInTile({8, 8, 1264, 704}, 0).height, 704);
}

int main() { return RUN_TESTS(); }
