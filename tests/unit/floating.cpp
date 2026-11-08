#include "view/floating.h"

#include "check.h"

using umbriel::anchoredContentOrigin;
using umbriel::attachmentOffset;
using umbriel::centeredOrigin;
using umbriel::centeredOverShown;
using umbriel::clampFloatingOrigin;
using umbriel::clampFloatingOriginForResize;
using umbriel::floatingFractionSize;
using umbriel::FloatingGeometry;
using umbriel::floatingKeepVisible;
using umbriel::FloatingPoint;
using umbriel::floatingSizeFraction;
using umbriel::serialSettled;

namespace {
  constexpr wlr_box kUsable{0, 0, 1920, 1080};
}

// Serial handoff
UMBRIEL_TEST(serialSettledOnAnExactMatch) { CHECK(serialSettled(100, 100)); }

UMBRIEL_TEST(serialNotSettledWhileTheClientLags) {
  CHECK(!serialSettled(99, 100));
  CHECK(!serialSettled(0, 1));
}

UMBRIEL_TEST(serialSettledOnceTheClientPassesIt) {
  // The client may commit several configures past ours before we look.
  CHECK(serialSettled(140, 100));
}

UMBRIEL_TEST(serialSurvivesCounterWraparound) {
  // The whole reason this is a signed difference. Requested just below the wrap, committed just above it: the client is
  // ahead, and a plain `committed >= requested` would say the opposite for 2 billion serials.
  constexpr uint32_t kNearMax = 0xFFFFFFF0;
  CHECK(serialSettled(4, kNearMax));
  CHECK(!serialSettled(kNearMax, 4));
}

UMBRIEL_TEST(noOutstandingRequestIsAlwaysSettled) {
  FloatingGeometry floating;
  CHECK(floating.retireSizeRequestIfSettled(0));
  CHECK(floating.retireSizeRequestIfSettled(12345));
}

UMBRIEL_TEST(aRequestBlocksUntilTheClientCatchesUp) {
  FloatingGeometry floating;
  floating.recordSizeRequest(300, 200, 50);
  CHECK(!floating.retireSizeRequestIfSettled(49));
  // Still outstanding, so a second early commit is still refused.
  CHECK(!floating.retireSizeRequestIfSettled(49));
  CHECK(floating.retireSizeRequestIfSettled(50));
  // Retired: the next commit passes without a request in flight.
  CHECK(floating.retireSizeRequestIfSettled(0));
}

UMBRIEL_TEST(aFullySpecifiedRequestIsPendingUntilTheClientCommitsIt) {
  FloatingGeometry floating;
  floating.recordSizeRequest(300, 200, 50);
  CHECK(floating.pendingSize().has_value());
  CHECK_EQ((*floating.pendingSize())[0], 300);
  CHECK_EQ((*floating.pendingSize())[1], 200);
  // The client lags: the pending size is what resize actions accumulate against.
  CHECK(!floating.retireSizeRequestIfSettled(49));
  CHECK(floating.pendingSize().has_value());
  CHECK(floating.retireSizeRequestIfSettled(50));
  // Retired: the client owns its size again and the basis falls back to it.
  CHECK(!floating.pendingSize().has_value());
}

UMBRIEL_TEST(aClientPreferenceRequestLeavesNoPendingSize) {
  // A configure with a 0 axis lets the client pick that axis: nothing is
  // pending, though the serial still gates ownership handback.
  FloatingGeometry floating;
  floating.recordSizeRequest(0, 0, 5);
  CHECK(!floating.pendingSize().has_value());
  CHECK(!floating.retireSizeRequestIfSettled(4));
  CHECK(floating.retireSizeRequestIfSettled(5));
}

UMBRIEL_TEST(clearSizeRequestDropsThePendingSize) {
  FloatingGeometry floating;
  floating.recordSizeRequest(300, 200, 5);
  floating.clearSizeRequest();
  CHECK(!floating.pendingSize().has_value());
}

UMBRIEL_TEST(settlingARequestDropsTheAnchorWhenNotResizing) {
  FloatingGeometry floating;
  floating.beginResize({10, 20, 300, 200}, WLR_EDGE_LEFT);
  floating.recordSizeRequest(300, 200, 7);
  floating.endResize(); // pointer released, but a configure is still in flight
  CHECK(floating.anchor().has_value());

  CHECK(floating.retireSizeRequestIfSettled(7));
  // The last configure landed and nothing is dragging: the anchor is spent.
  CHECK(!floating.anchor().has_value());
  CHECK_EQ(static_cast<int>(floating.edges()), 0);
}

UMBRIEL_TEST(anchorSurvivesWhileTheResizeIsStillUnderThePointer) {
  FloatingGeometry floating;
  floating.beginResize({10, 20, 300, 200}, WLR_EDGE_TOP);
  floating.recordSizeRequest(300, 200, 7);

  CHECK(floating.retireSizeRequestIfSettled(7));
  // More configures are coming; dropping the anchor now would let the far edge
  // walk for the rest of the drag.
  CHECK(floating.anchor().has_value());
  CHECK_EQ(static_cast<int>(floating.edges()), static_cast<int>(WLR_EDGE_TOP));
}

UMBRIEL_TEST(endResizeKeepsTheAnchorWhileAConfigureIsInFlight) {
  FloatingGeometry floating;
  floating.beginResize({10, 20, 300, 200}, WLR_EDGE_LEFT);
  floating.recordSizeRequest(300, 200, 9);
  floating.endResize();
  CHECK(floating.anchor().has_value());
}

UMBRIEL_TEST(endResizeDropsTheAnchorWhenNothingIsInFlight) {
  FloatingGeometry floating;
  floating.beginResize({10, 20, 300, 200}, WLR_EDGE_LEFT);
  floating.endResize();
  CHECK(!floating.anchor().has_value());
}

UMBRIEL_TEST(aResizeAckNeedsUsableContentBeforeHandoff) {
  umbriel::FloatingSizeConfigure sent{.size = {600, 450}, .base = {400, 300}, .serial = 10};
  CHECK(!sent.settled(9, sent.size, {600, 450}, false));
  CHECK(!sent.settled(10, sent.size, sent.base, false));
  CHECK(sent.settled(10, sent.size, {600, 450}, false));
  CHECK(!sent.settled(10, sent.size, {590, 440}, true));
  CHECK(sent.settled(10, sent.size, {590, 440}, false));
  sent.supplied = true;
  CHECK(!sent.settled(10, sent.size, sent.base, true));
  CHECK(sent.settled(10, sent.size, sent.base, false));
  CHECK(!sent.settled(10, sent.size, {0, 450}, false));
}

UMBRIEL_TEST(queuedPointerTargetsRetainTheSentConfigureAndAnchor) {
  FloatingGeometry floating;
  floating.beginResize({200, 200, 400, 300}, WLR_EDGE_LEFT | WLR_EDGE_TOP);
  floating.recordSizeRequest(440, 340, 20);
  const umbriel::FloatingSizeConfigure sent{.size = {440, 340}, .base = {400, 300}, .serial = 20, .supplied = true};
  floating.setPendingSize(520, 420);
  floating.endResize();
  CHECK_EQ((*floating.pendingSize())[0], 520);
  CHECK_EQ((*floating.pendingSize())[1], 420);
  CHECK(!sent.settled(20, floating.pendingSize(), sent.size, false));
  CHECK(floating.anchor().has_value());
  const auto origin = anchoredContentOrigin(*floating.anchor(), floating.edges(), {12, 12, 520, 420});
  CHECK_EQ(origin.x + 520, 600);
  CHECK_EQ(origin.y + 420, 500);
}

UMBRIEL_TEST(sameSizedBuffersCanAdvanceResizeBackpressure) {
  CHECK(!umbriel::floatingSizeResponseUsable({400, 300}, {400, 300}, false));
  CHECK(umbriel::floatingSizeResponseUsable({400, 300}, {400, 300}, true));
  CHECK(umbriel::floatingSizeResponseUsable({500, 300}, {400, 300}, false));
  CHECK(!umbriel::floatingSizeResponseUsable({0, 300}, {400, 300}, true));
}

UMBRIEL_TEST(contentHandoffSupportsXwaylandAndSerialWraparound) {
  umbriel::FloatingSizeConfigure sent{.size = {600, 450}, .base = {400, 300}};
  CHECK(!sent.settled(0, sent.size, sent.base, false));
  CHECK(sent.settled(0, sent.size, {590, 440}, false));
  sent.serial = 0xFFFFFFF0;
  CHECK(!sent.settled(4, sent.size, sent.base, false));
  CHECK(sent.settled(4, sent.size, {600, 450}, false));
  CHECK(sent.settled(4, std::nullopt, {600, 450}, false));
}

// Clamp arithmetic
UMBRIEL_TEST(keepVisibleIsAQuarterWithinBounds) {
  CHECK_EQ(floatingKeepVisible(200), 50);
  // Floored at 10 for a tiny window, capped at 75 for a huge one.
  CHECK_EQ(floatingKeepVisible(20), 10);
  CHECK_EQ(floatingKeepVisible(0), 10);
  CHECK_EQ(floatingKeepVisible(4000), 75);
}

UMBRIEL_TEST(aWindowFullyOnScreenIsNotMoved) {
  const wlr_box geo{0, 0, 800, 600};
  const FloatingPoint origin{500, 300};
  const FloatingPoint clamped = clampFloatingOrigin(origin, geo, kUsable);
  CHECK_EQ(clamped.x, 500);
  CHECK_EQ(clamped.y, 300);
}

UMBRIEL_TEST(aWindowDraggedOffTheLeftKeepsASliceOnScreen) {
  const wlr_box geo{0, 0, 800, 600};
  // Far off to the left; only `floatingKeepVisible` may remain reachable.
  const FloatingPoint clamped = clampFloatingOrigin({-5000, 300}, geo, kUsable);
  CHECK_EQ(clamped.x, floatingKeepVisible(800) - 800);
  CHECK(clamped.x + geo.width >= kUsable.x + floatingKeepVisible(800));
}

UMBRIEL_TEST(aWindowDraggedOffTheRightKeepsASliceOnScreen) {
  const wlr_box geo{0, 0, 800, 600};
  const FloatingPoint clamped = clampFloatingOrigin({5000, 300}, geo, kUsable);
  CHECK_EQ(clamped.x, kUsable.width - floatingKeepVisible(800));
  CHECK(clamped.x <= kUsable.x + kUsable.width - floatingKeepVisible(800));
}

UMBRIEL_TEST(clampRespectsAnOffsetUsableArea) {
  // A second monitor to the right, with a bar taking the top. The bounds are
  // relative to that monitor's usable box, not to the layout origin.
  const wlr_box usable{1920, 40, 1920, 1040};
  const wlr_box geo{0, 0, 800, 600};
  const FloatingPoint clamped = clampFloatingOrigin({0, -5000}, geo, usable);
  CHECK_EQ(clamped.x, usable.x + floatingKeepVisible(800) - 800);
  CHECK_EQ(clamped.y, usable.y + floatingKeepVisible(600) - 600);
}

UMBRIEL_TEST(aWindowWiderThanItsOutputStillClampsToTheFarEdge) {
  // A window larger than the output it sits on: the range stays valid, so it is
  // pulled back to the right-hand bound like any other.
  const wlr_box usable{0, 0, 100, 100};
  const wlr_box geo{0, 0, 800, 600};
  const FloatingPoint clamped = clampFloatingOrigin({500, 500}, geo, usable);
  CHECK_EQ(clamped.x, usable.x + usable.width - floatingKeepVisible(800));
  CHECK_EQ(clamped.y, usable.y + usable.height - floatingKeepVisible(600));
}

UMBRIEL_TEST(clampDoesNotInvertWhenTheBoundsCross) {
  // The guard's actual trigger: with no geometry to speak of and a tiny usable area, the low bound overshoots the high
  // one. Without the guard std::clamp is undefined behaviour here.
  const wlr_box usable{0, 0, 10, 10};
  const wlr_box geo{0, 0, 0, 0};
  const FloatingPoint clamped = clampFloatingOrigin({500, 500}, geo, usable);
  // Low bound wins rather than the range being read backwards.
  CHECK_EQ(clamped.x, usable.x + floatingKeepVisible(0));
  CHECK_EQ(clamped.y, usable.y + floatingKeepVisible(0));
}

UMBRIEL_TEST(resizeClampSnapsOnlyTheAxisTheWindowFills) {
  // Width fills the usable axis and snaps to its edge; height has slack and
  // keeps the ordinary on-screen clamp, off-screen hang included.
  const wlr_box usable{0, 26, 1280, 694};
  const wlr_box geo{0, 0, 1280, 300};
  const FloatingPoint clamped = clampFloatingOriginForResize({500, -5000}, geo, usable);
  CHECK_EQ(clamped.x, 0);
  CHECK_EQ(clamped.y, clampFloatingOrigin({500, -5000}, geo, usable).y);
}

// Dialog placement
UMBRIEL_TEST(aDialogCentersOnAParentThatIsFullyInView) {
  const wlr_box parent{100, 110, 900, 600};
  const FloatingPoint origin = centeredOverShown(parent, kUsable, 400, 300);
  CHECK_EQ(origin.x, 350);
  CHECK_EQ(origin.y, 260);
}

UMBRIEL_TEST(aDialogCentersOnTheVisiblePartOfAScrolledParent) {
  // The parent hangs 500 wide off the left edge, so only its right 400 show.
  const wlr_box parent{-500, 0, 900, 600};
  const FloatingPoint origin = centeredOverShown(parent, kUsable, 200, 100);
  CHECK_EQ(origin.x, 100);
  CHECK_EQ(origin.y, 250);
}

UMBRIEL_TEST(aDialogOverAParentOutOfViewCentersOnTheArea) {
  const wlr_box parent{-2000, 0, 900, 600};
  const FloatingPoint origin = centeredOverShown(parent, kUsable, 400, 300);
  CHECK_EQ(origin.x, 760);
  CHECK_EQ(origin.y, 390);
}

UMBRIEL_TEST(aDialogStaysInsideTheAreaWhenItFits) {
  // Centered on the sliver of parent in the corner, the dialog would overhang; it is pulled back to the edge.
  const wlr_box parent{1800, 1000, 900, 600};
  const FloatingPoint origin = centeredOverShown(parent, kUsable, 400, 300);
  CHECK_EQ(origin.x, kUsable.width - 400);
  CHECK_EQ(origin.y, kUsable.height - 300);
}

UMBRIEL_TEST(aDialogLargerThanTheAreaKeepsItsTopLeftOnIt) {
  const wlr_box usable{0, 40, 1280, 680};
  const FloatingPoint origin = centeredOverShown({0, 40, 1280, 680}, usable, 1500, 900);
  CHECK_EQ(origin.x, 0);
  CHECK_EQ(origin.y, 40);
}

// Attached dialog placement, one axis at a time
UMBRIEL_TEST(anAttachedDialogCentersOnItsParent) {
  // A 900 long parent at 100 centers a 400 long dialog 250 into it.
  CHECK_EQ(attachmentOffset(100, 900, 400, 0, 1920, 2), 250);
}

UMBRIEL_TEST(anAttachedDialogWiderThanItsParentStaysInsideTheArea) {
  // Centered on a 200 long parent at the area's start, the 600 long dialog would start at -200; it stops at the
  // area's start plus its border.
  CHECK_EQ(attachmentOffset(0, 200, 600, 0, 1920, 2), 2);
  // At the far end it stops one border short of the area's end, at 1920 - 2 - 600 = 1318: -402 from the parent.
  CHECK_EQ(attachmentOffset(1720, 200, 600, 0, 1920, 2), -402);
}

UMBRIEL_TEST(anAttachedDialogLongerThanTheAreaKeepsItsStartInside) {
  CHECK_EQ(attachmentOffset(40, 680, 900, 40, 680, 0), 0);
  CHECK_EQ(attachmentOffset(100, 500, 900, 40, 680, 0), -60);
}

UMBRIEL_TEST(anAttachedDialogGoesAlongWithAParentOutsideTheArea) {
  // A column scrolled wholly out of view keeps its dialog centered on it rather than left behind on screen.
  CHECK_EQ(attachmentOffset(2000, 200, 600, 0, 1920, 2), -200);
  CHECK_EQ(attachmentOffset(-400, 400, 600, 0, 1920, 2), -100);
}

UMBRIEL_TEST(anAttachedDialogWithNoAreaIsCentered) { CHECK_EQ(attachmentOffset(0, 200, 600, 0, 0, 2), -200); }

// Resize anchoring
UMBRIEL_TEST(draggingTheRightEdgeLeavesTheOriginAlone) {
  const wlr_box anchor{100, 100, 400, 300};
  const wlr_box shrunk{0, 0, 250, 300};
  const FloatingPoint origin = anchoredContentOrigin(anchor, WLR_EDGE_RIGHT, shrunk);
  CHECK_EQ(origin.x, 100);
  CHECK_EQ(origin.y, 100);
}

UMBRIEL_TEST(draggingTheLeftEdgePinsTheRightOne) {
  const wlr_box anchor{100, 100, 400, 300};
  const wlr_box shrunk{0, 0, 250, 300};
  const FloatingPoint origin = anchoredContentOrigin(anchor, WLR_EDGE_LEFT, shrunk);
  // Right edge stays at 500 as the window narrows.
  CHECK_EQ(origin.x, 250);
  CHECK_EQ(origin.x + shrunk.width, anchor.x + anchor.width);
}

UMBRIEL_TEST(draggingTheTopEdgePinsTheBottomOne) {
  const wlr_box anchor{100, 100, 400, 300};
  const wlr_box shrunk{0, 0, 400, 180};
  const FloatingPoint origin = anchoredContentOrigin(anchor, WLR_EDGE_TOP, shrunk);
  CHECK_EQ(origin.y, 220);
  CHECK_EQ(origin.y + shrunk.height, anchor.y + anchor.height);
}

UMBRIEL_TEST(draggingACornerPinsBothOppositeEdges) {
  const wlr_box anchor{100, 100, 400, 300};
  const wlr_box shrunk{0, 0, 250, 180};
  const FloatingPoint origin = anchoredContentOrigin(anchor, WLR_EDGE_TOP | WLR_EDGE_LEFT, shrunk);
  CHECK_EQ(origin.x + shrunk.width, anchor.x + anchor.width);
  CHECK_EQ(origin.y + shrunk.height, anchor.y + anchor.height);
}

// Remembered position
UMBRIEL_TEST(positionIsRestoredOnTheSameOutput) {
  FloatingGeometry floating;
  floating.rememberPositionFraction({480, 270}, kUsable);
  const auto restored = floating.restoredOrigin(kUsable);
  CHECK(restored.has_value());
  CHECK_EQ(restored->x, 480);
  CHECK_EQ(restored->y, 270);
}

UMBRIEL_TEST(positionScalesOntoADifferentOutput) {
  FloatingGeometry floating;
  // A quarter across a 1920-wide output...
  floating.rememberPositionFraction({480, 270}, kUsable);
  // ...is a quarter across a 3840-wide one.
  const wlr_box wide{0, 0, 3840, 2160};
  const auto restored = floating.restoredOrigin(wide);
  CHECK(restored.has_value());
  CHECK_EQ(restored->x, 960);
  CHECK_EQ(restored->y, 540);
}

UMBRIEL_TEST(nothingIsRestoredWithoutAMemory) {
  FloatingGeometry floating;
  CHECK(!floating.restoredOrigin(kUsable).has_value());
}

UMBRIEL_TEST(aDegenerateUsableAreaIsNeitherRememberedNorRestored) {
  FloatingGeometry floating;
  // An output being torn down reports a zero box; dividing by it would poison
  // the remembered fraction with inf/nan.
  floating.rememberPositionFraction({480, 270}, {0, 0, 0, 0});
  CHECK(!floating.restoredOrigin(kUsable).has_value());

  floating.rememberPositionFraction({480, 270}, kUsable);
  CHECK(!floating.restoredOrigin({0, 0, 0, 0}).has_value());
}

UMBRIEL_TEST(sizeIsRememberedAcrossRoundTrips) {
  FloatingGeometry floating;
  CHECK(!floating.size().has_value());
  floating.rememberSize(640, 480);
  CHECK(floating.size().has_value());
  CHECK_EQ((*floating.size())[0], 640);
  CHECK_EQ((*floating.size())[1], 480);
}

// Centering
UMBRIEL_TEST(centeringPlacesEqualMarginsOnEachSide) {
  const FloatingPoint origin = centeredOrigin(kUsable, 800, 600);
  CHECK_EQ(origin.x, 560);
  CHECK_EQ(origin.y, 240);
  CHECK_EQ(origin.x - kUsable.x, kUsable.width - 800 - (origin.x - kUsable.x));
}

UMBRIEL_TEST(centeringHonoursAnOffsetUsableArea) {
  const wlr_box usable{1920, 40, 1920, 1040};
  const FloatingPoint origin = centeredOrigin(usable, 800, 600);
  CHECK_EQ(origin.x, 1920 + 560);
  CHECK_EQ(origin.y, 40 + 220);
}

// Fraction sizing (window_rule.default_width / default_height on floats)
UMBRIEL_TEST(aFractionOfTheUsableAxisBecomesPixels) {
  CHECK_EQ(floatingFractionSize(0.5, 1920), 960);
  CHECK_EQ(floatingFractionSize(0.5, 1080), 540);
  CHECK_EQ(floatingFractionSize(0.1, 1920), 192);
  CHECK_EQ(floatingFractionSize(1.0, 1920), 1920);
}

UMBRIEL_TEST(aDegenerateUsableAxisLeavesTheChoiceToTheClient) {
  CHECK_EQ(floatingFractionSize(0.5, 0), 0);
  CHECK_EQ(floatingFractionSize(0.5, -1), 0);
}

UMBRIEL_TEST(aFractionRoundsRatherThanTruncates) {
  // Layout::fractionalWidth rounds, so a float and a tiled lane at the same
  // fraction must not disagree by a pixel when the usable axis is odd, which a
  // layer-shell exclusive zone readily produces.
  CHECK_EQ(floatingFractionSize(0.5, 1281), 641);
  CHECK_EQ(floatingFractionSize(0.5, 719), 360);
  CHECK_EQ(floatingFractionSize(0.75, 1366), 1025);
}
UMBRIEL_TEST(fractionPixelsStayWithinTheAxis) {
  // Defensive: parse clamps fractions to [0.1, 1.0], but the arithmetic result
  // never leaves the usable axis and never collapses to 0.
  CHECK_EQ(floatingFractionSize(0.0001, 100), 1);
  CHECK_EQ(floatingFractionSize(1.5, 800), 800);
}

UMBRIEL_TEST(aSizeReadsBackAsTheFractionItCameFrom) {
  CHECK(floatingSizeFraction(960, 1920) == 0.5);
  CHECK(floatingSizeFraction(192, 1920) == 0.1);
}

UMBRIEL_TEST(titleBarRemainsReachableWithoutMovingOtherFloats) {
  const wlr_box usable{-300, 40, 1280, 720};
  CHECK_EQ(umbriel::keepTitleBarReachable({100, 42}, 28, usable).y, 68);
  CHECK_EQ(umbriel::keepTitleBarReachable({100, 150}, 28, usable).y, 150);
  CHECK_EQ(umbriel::keepTitleBarReachable({100, -100}, 0, usable).y, -100);
  CHECK_EQ(umbriel::keepTitleBarReachable({-500, 42}, 28, usable).x, -500);
}

UMBRIEL_TEST(cascadeKeepsAnUnoccupiedOrigin) {
  const auto point = umbriel::cascadeOrigin({400, 210}, {}, 32, {0, 0, 1280, 720}, 480, 300);
  CHECK_EQ(point.x, 400);
  CHECK_EQ(point.y, 210);
}

UMBRIEL_TEST(cascadeAdvancesPastNearbyOrigins) {
  const wlr_box taken[]{{403, 208, 480, 300}, {435, 240, 480, 300}};
  const auto point = umbriel::cascadeOrigin({400, 210}, taken, 32, {0, 0, 1280, 720}, 480, 300);
  CHECK_EQ(point.x, 467);
  CHECK_EQ(point.y, 272);
}

UMBRIEL_TEST(cascadeWrapsWithinLogicalUsableArea) {
  const wlr_box taken[]{{-500, 200, 480, 300}};
  const auto point = umbriel::cascadeOrigin({-500, 200}, taken, 32, {-800, 100, 800, 400}, 480, 300);
  CHECK_EQ(point.x, -768);
  CHECK_EQ(point.y, 132);
}

UMBRIEL_TEST(cascadeChecksWrappedOriginsAndFillsFurtherDiagonals) {
  std::vector<wlr_box> taken;
  const wlr_box area{0, 28, 1280, 692};
  for (int i = 0; i < 40; ++i) {
    const auto point = umbriel::cascadeOrigin({400, 224}, taken, 32, area, 480, 300);
    CHECK(std::ranges::none_of(taken, [&](const auto& other) { return point.x == other.x && point.y == other.y; }));
    CHECK(point.x >= area.x && point.x + 480 <= area.x + area.width);
    CHECK(point.y >= area.y && point.y + 300 <= area.y + area.height);
    taken.push_back({point.x, point.y, 480, 300});
  }
}

UMBRIEL_TEST(cascadePreservesAnExposedFrameOfADifferentSize) {
  const wlr_box taken[]{{558, 294, 164, 132}};
  const auto point = umbriel::cascadeOrigin({478, 254}, taken, 35, {0, 0, 1280, 720}, 324, 212);
  CHECK_EQ(point.x, 593);
  CHECK_EQ(point.y, 329);
}

UMBRIEL_TEST(cascadeIgnoresCoveredCornersAndPartialOverlap) {
  const wlr_box taken[]{{550, 280, 300, 220}, {558, 294, 164, 132}};
  const auto point = umbriel::cascadeOrigin({478, 254}, taken, 35, {0, 0, 1280, 720}, 324, 212);
  CHECK_EQ(point.x, 478);
  CHECK_EQ(point.y, 254);
}

UMBRIEL_TEST(cascadeStepClearsTheDecoratedTitleBar) {
  CHECK_EQ(umbriel::floatingCascadeStep(28, 2), 35);
  CHECK_EQ(umbriel::floatingCascadeStep(60, 2), 67);
  CHECK_EQ(umbriel::floatingCascadeStep(0, 2), 32);
}

int main() { return RUN_TESTS(); }
