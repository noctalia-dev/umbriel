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
