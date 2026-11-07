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
  // The configuration defaults: size and padding derived from the bar height.
  constexpr TitleBarButtonGeometry kDerived{.size = -1, .gap = 8, .padding = -1};
  constexpr TitleBarButtonGeometry kExplicit{.size = 12, .gap = 8, .padding = 13};
} // namespace

UMBRIEL_TEST(rightButtonsPutLastListedOutermost) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, kDerived);
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
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll, kDerived);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 7);
  CHECK_EQ(parts.button(TitleBarButton::Maximize)->x, 29);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 51);
  CHECK_EQ(parts.title.x, 73);
  CHECK_EQ(parts.title.width, 399);
}

UMBRIEL_TEST(largeFrameCornersInsetButtonsAndTitle) {
  const auto right = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, kDerived, 50, 14);
  const auto left = titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll, kDerived, 50, 14);
  CHECK_EQ(left.button(TitleBarButton::Close)->x, 26);
  CHECK_EQ(right.button(TitleBarButton::Close)->x, 440);
  CHECK_EQ(left.button(TitleBarButton::Close)->width, 14);
  CHECK_EQ(right.title.x, 26);
  CHECK_EQ(left.title.width, right.title.width);
  CHECK_EQ(left.title.x + left.title.width, 454);
  const auto padded =
      titleBarLayout(480, 28, TitleBarButtonSide::Left, kAll, {.size = -1, .gap = 8, .padding = 60}, 50);
  CHECK_EQ(padded.button(TitleBarButton::Close)->x, 60);
  for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
    const auto bare = titleBarLayout(480, 28, side, kNone, kDerived, 50, 14);
    CHECK_EQ(bare.title.x, 26);
    CHECK_EQ(bare.title.width, 428);
  }
}

UMBRIEL_TEST(titleInsetsFollowTheTextHeightAndKeepSmallRadiiAtNormalPadding) {
  const auto low = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, kDerived, 12, 17);
  CHECK_EQ(low.title.x, kTitleBarTitlePadding);
  CHECK_EQ(low.button(TitleBarButton::Close)->x, 459);
  const auto normal = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, kDerived, 50, 17);
  const auto tall = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, kDerived, 50, 24);
  CHECK_EQ(normal.title.x, 30);
  CHECK(normal.title.x < 50);
  CHECK(tall.title.x > normal.title.x);
}

UMBRIEL_TEST(tallTitlesRespectBothCornersWithSmallButtons) {
  constexpr std::array one{TitleBarButton::Close};
  for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
    const auto parts = titleBarLayout(480, 28, side, one, {.size = 1, .gap = 0, .padding = 0}, 50, 28);
    CHECK_EQ(parts.title.x, 51);
    CHECK_EQ(parts.title.width, 378);
  }
}

UMBRIEL_TEST(buttonBoxesStayInsideTheCornerArc) {
  for (const int radius : {20, 50, 100}) {
    for (const int height : {16, 28, 40}) {
      for (const int size : {-1, height}) {
        for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
          const auto parts = titleBarLayout(480, height, side, kAll, {.size = size, .gap = 8, .padding = 0}, radius);
          const auto box = *parts.button(TitleBarButton::Close);
          const int x = side == TitleBarButtonSide::Left ? box.x : 480 - box.x - box.width;
          if (box.y < radius && x < radius) {
            const int dx = radius - x;
            const int dy = radius - box.y;
            CHECK(dx * dx + dy * dy < radius * radius);
          }
          CHECK_EQ(box.width, titleBarButtonDiameter(height, size));
        }
      }
    }
  }
}

UMBRIEL_TEST(narrowRoundedBarsDropControlsBeforeCrossingTheArc) {
  for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
    const auto parts = titleBarLayout(70, 28, side, kAll, kDerived, 50, 17);
    CHECK(parts.button(TitleBarButton::Close).has_value());
    CHECK(!parts.button(TitleBarButton::Maximize).has_value());
    CHECK(!parts.button(TitleBarButton::Minimize).has_value());
    CHECK_EQ(parts.title.width, 0);
    const auto tiny = titleBarLayout(50, 28, side, kAll, kDerived, 50, 17);
    CHECK(!tiny.button(TitleBarButton::Close).has_value());
    CHECK_EQ(tiny.title.width, 0);
  }
}

UMBRIEL_TEST(roundedButtonHitTargetsFollowTheirInset) {
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 0, true, TitleBarButtonSide::Right, kAll, kDerived, 447, -14, 50).part,
      SsdFramePart::Close
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 0, true, TitleBarButtonSide::Right, kAll, kDerived, 466, -14, 50).part,
      SsdFramePart::TitleBar
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 0, true, TitleBarButtonSide::Left, kAll, kDerived, 33, -14, 50).part,
      SsdFramePart::Close
  );
}

UMBRIEL_TEST(omittedButtonsLeaveNoGapAndWidenTheTitle) {
  constexpr std::array order{TitleBarButton::Close, TitleBarButton::Minimize};
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, order, kDerived);
  CHECK_EQ(parts.button(TitleBarButton::Minimize)->x, 459);
  CHECK_EQ(parts.button(TitleBarButton::Close)->x, 437);
  CHECK(!parts.button(TitleBarButton::Maximize).has_value());
  CHECK_EQ(parts.title.width, 421);
  for (const auto side : {TitleBarButtonSide::Right, TitleBarButtonSide::Left}) {
    const auto bare = titleBarLayout(480, 28, side, kNone, kDerived);
    CHECK(!bare.button(TitleBarButton::Close).has_value());
    CHECK_EQ(bare.title.x, 8);
    CHECK_EQ(bare.title.width, 464);
  }
}

UMBRIEL_TEST(derivedSizeAndPaddingFollowTheBarHeight) {
  constexpr std::array heights{24, 27, 28, 30, 60, 120};
  constexpr std::array sizes{12, 13, 14, 14, 30, 60};
  constexpr std::array paddings{6, 7, 7, 8, 15, 30};
  for (size_t i = 0; i < heights.size(); ++i) {
    CHECK_EQ(titleBarButtonDiameter(heights[i], -1), sizes[i]);
    const auto parts = titleBarLayout(480, heights[i], TitleBarButtonSide::Right, kAll, kDerived);
    CHECK_EQ(parts.gap, 8);
    CHECK_EQ(480 - parts.button(TitleBarButton::Close)->x - sizes[i], paddings[i]);
  }
}

UMBRIEL_TEST(explicitSizeIsExactUpToTheBarHeight) {
  for (const int size : {1, 11, 12, 13}) {
    CHECK_EQ(titleBarButtonDiameter(28, size), size);
  }
  CHECK_EQ(titleBarButtonDiameter(28, 100), 28);
  // An odd remainder leaves the extra pixel below the button.
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right, kAll, {.size = 11, .gap = 8, .padding = 4});
  CHECK_EQ(parts.button(TitleBarButton::Close)->y, 8);
  CHECK_EQ(parts.button(TitleBarButton::Close)->height, 11);
}

UMBRIEL_TEST(narrowBarsShrinkPaddingThenGapThenDropInnerButtons) {
  constexpr std::array order{TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
  const auto paddingOnly = titleBarLayout(60, 30, TitleBarButtonSide::Left, order, kExplicit);
  CHECK_EQ(paddingOnly.button(TitleBarButton::Close)->x, 8);
  CHECK_EQ(paddingOnly.gap, 8);
  const auto right = titleBarLayout(50, 30, TitleBarButtonSide::Right, order, kExplicit);
  const auto left = titleBarLayout(50, 30, TitleBarButtonSide::Left, order, kExplicit);
  CHECK_EQ(right.button(TitleBarButton::Close)->x, 38);
  CHECK_EQ(left.button(TitleBarButton::Close)->x, 0);
  CHECK_EQ(right.gap, 7);
  // Two buttons fit; the innermost one gets no slot and its place belongs to the bar.
  const auto partial = titleBarLayout(30, 30, TitleBarButtonSide::Right, order, kExplicit);
  CHECK_EQ(partial.button(TitleBarButton::Close)->x, 18);
  CHECK_EQ(partial.button(TitleBarButton::Maximize)->x, 6);
  CHECK(!partial.button(TitleBarButton::Minimize).has_value());
  for (const auto side : {TitleBarButtonSide::Left, TitleBarButtonSide::Right}) {
    const auto parts = titleBarLayout(10, 30, side, order, kExplicit);
    CHECK(!parts.button(TitleBarButton::Close).has_value());
    CHECK_EQ(ssdFramePointAt(10, 300, 30, 2, true, side, order, kExplicit, 5, -15).part, SsdFramePart::TitleBar);
  }
  CHECK_EQ(
      ssdFramePointAt(30, 300, 30, 2, true, TitleBarButtonSide::Right, order, kExplicit, 2, -15).part,
      SsdFramePart::TitleBar
  );
  const auto empty = titleBarLayout(20, 30, TitleBarButtonSide::Left, kNone, kExplicit);
  CHECK_EQ(empty.gap, 8);
  CHECK(!empty.button(TitleBarButton::Close).has_value());
}

UMBRIEL_TEST(buttonHitSpansHalfConfiguredGap) {
  static constexpr std::array one{TitleBarButton::Close};
  const auto hit = [](double x) {
    return ssdFramePointAt(100, 300, 30, 2, true, TitleBarButtonSide::Right, one, kExplicit, x, -15).part;
  };
  CHECK_EQ(hit(71), SsdFramePart::Close);
  CHECK_EQ(hit(70), SsdFramePart::TitleBar);
  CHECK_EQ(hit(90), SsdFramePart::Close);
  CHECK_EQ(hit(91), SsdFramePart::TitleBar);
}

UMBRIEL_TEST(oddGapHitSpansMeetWithoutOverlapping) {
  const auto hit = [](TitleBarButtonSide side, double x) {
    return ssdFramePointAt(50, 300, 30, 2, true, side, kAll, kExplicit, x, -15).part;
  };
  // The 50px bar shrinks the configured 8px gap to 7px.
  CHECK_EQ(hit(TitleBarButtonSide::Left, 15), SsdFramePart::Close);
  CHECK_EQ(hit(TitleBarButtonSide::Left, 16), SsdFramePart::Maximize);
  CHECK_EQ(hit(TitleBarButtonSide::Right, 34), SsdFramePart::Maximize);
  CHECK_EQ(hit(TitleBarButtonSide::Right, 35), SsdFramePart::Close);
}

UMBRIEL_TEST(buttonsSitOnTheBarCenterAtEveryHeight) {
  for (int height = 16; height <= 128; ++height) {
    const auto parts = titleBarLayout(480, height, TitleBarButtonSide::Right, kAll, kDerived);
    const wlr_box close = *parts.button(TitleBarButton::Close);
    CHECK_EQ(2 * close.y + close.height, height);
    CHECK_EQ(480 - close.x - close.width, close.y);
    CHECK(close.height >= 8 && close.height <= (height + 1) / 2);
  }
}

UMBRIEL_TEST(buttonHitSpansTitleBarHeight) {
  const auto at = [](TitleBarButtonSide side, std::span<const TitleBarButton> shown, double x, double y) {
    return ssdFramePointAt(480, 300, 28, 2, true, side, shown, kDerived, x, y).part;
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
  const auto at = [](bool resizable, double x) {
    return ssdFramePointAt(480, 300, 28, 2, resizable, TitleBarButtonSide::Right, kAll, kDerived, x, 100);
  };
  CHECK_EQ(at(false, -3).part, SsdFramePart::None);
  CHECK_EQ(at(true, -3).edges, uint32_t{WLR_EDGE_LEFT});
  CHECK_EQ(at(true, -8.1).part, SsdFramePart::None);
}

UMBRIEL_TEST(resizeBandWithoutTitleBar) {
  const auto at = [](bool resizable, double x, double y) {
    return ssdFramePointAt(480, 300, 0, 2, resizable, TitleBarButtonSide::Right, kAll, kDerived, x, y);
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
  const auto at = [](double x, double y) {
    return ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, kAll, kDerived, x, y).edges;
  };
  CHECK_EQ(at(-3, -15), uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP});
  CHECK_EQ(at(470, 302), uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM});
  CHECK_EQ(at(200, -31), uint32_t{WLR_EDGE_TOP});
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
