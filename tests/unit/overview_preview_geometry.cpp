#include "check.h"
#include "core/animation.h"
#include "overview/preview_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
  // A 2560x1600 output at scale 1.5 is 1707x1067 in layout coordinates; the overview previews it at half size.
  constexpr int kOutputWidth = 1707;
  constexpr int kOutputHeight = 1067;
  constexpr double kZoom = 0.5;
  constexpr double kScale = 1.5;
  constexpr int kRefreshHz = 165;

  umbriel::PreviewProjection projectionForAxis(bool horizontal) {
    return {
        .outputBox = {1707, -1067, kOutputWidth, kOutputHeight},
        .zoom = kZoom,
        .overviewZoom = kZoom,
        .outputScale = kScale,
        .outputPixelWidth = 2560,
        .outputPixelHeight = 1600,
        .horizontal = horizontal,
    };
  }

  wlr_fbox backgroundBox(const umbriel::PreviewProjection& projection) {
    const wlr_box& output = projection.outputBox;
    return {
        static_cast<double>(output.x),
        static_cast<double>(output.y),
        static_cast<double>(output.width),
        static_cast<double>(output.height),
    };
  }

  std::array<long, 4> physicalEdges(const umbriel::PreviewProjection& projection, const wlr_fbox& box) {
    const wlr_box& output = projection.outputBox;
    return {
        std::lround((box.x - output.x) * projection.outputScale),
        std::lround((box.y - output.y) * projection.outputScale),
        std::lround((box.x + box.width - output.x) * projection.outputScale),
        std::lround((box.y + box.height - output.y) * projection.outputScale),
    };
  }

  struct PreviewRow {
    wlr_fbox world;
    size_t index;
  };

  bool allRowEdgesSettled(
      const umbriel::PreviewProjection& projection, const std::vector<PreviewRow>& rows, double target, double remaining
  ) {
    return std::ranges::all_of(rows, [&](const PreviewRow& row) {
      return umbriel::previewRowEdgesSettled(projection, row.world, row.index, target, remaining);
    });
  }
} // namespace

UMBRIEL_TEST(previewGridPreservesFractionalGeometry) {
  const umbriel::PreviewGrid grid = umbriel::previewGrid(0, 0, kOutputWidth, kOutputHeight, kZoom, false);
  CHECK_NEAR(grid.step(false), 586.85, 0.000001);
  CHECK_EQ(grid.width, 853.5);
  CHECK_EQ(grid.height, 533.5);
  CHECK_EQ(grid.baseX, 426.75);
  CHECK_EQ(grid.baseY, 266.75);

  const umbriel::PreviewGrid offset = umbriel::previewGrid(1707, 0, kOutputWidth, kOutputHeight, kZoom, true);
  CHECK_EQ(offset.baseX - 1707, grid.baseX);
  CHECK_EQ(offset.step(true), offset.width + offset.gap);
}

UMBRIEL_TEST(overviewZoomReachesEveryPhysicalPixel) {
  // 4K at 150%: changing zoom by 1/1920 moves each horizontal preview edge
  // by exactly one physical pixel, including at a nonzero output origin.
  constexpr int outputX = 2560;
  for (int step = 0; step <= 6; ++step) {
    const double zoom = 0.5 + step / 1920.0;
    const umbriel::PreviewGrid grid = umbriel::previewGrid(outputX, -1440, 2560, 1440, zoom, false);
    CHECK_EQ(std::lround((grid.baseX - outputX) * kScale), 960 - step);
    CHECK_EQ(std::lround((grid.baseX + grid.width - outputX) * kScale), 2880 + step);
    CHECK_EQ(std::lround(umbriel::previewZoomEdge(0.0, 2560.0, zoom, kZoom, kScale) * kScale), 960 - step);
    CHECK_EQ(std::lround(umbriel::previewZoomEdge(2560.0, 2560.0, zoom, kZoom, kScale) * kScale), 2880 + step);
    CHECK_NEAR(grid.baseX + grid.width / 2.0, outputX + 1280.0, 0.000001);
  }
  const umbriel::PreviewGrid desktop = umbriel::previewGrid(outputX, -1440, 2560, 1440, 1.0, false);
  CHECK_EQ(desktop.baseX, outputX);
  CHECK_EQ(desktop.baseY, -1440);
  CHECK_EQ(desktop.width, 2560);
  CHECK_EQ(desktop.height, 1440);
}

UMBRIEL_TEST(overviewCloseReachesTheOutputsExactLastPixel) {
  for (double extent : {1706.0, 1707.0}) {
    for (double zoom : {0.99999, 1.0}) {
      const double right = umbriel::previewZoomEdge(extent, extent, zoom, kZoom, kScale, 0, 2560);
      CHECK_EQ(std::lround(right * kScale), 2560);
    }
  }
}

UMBRIEL_TEST(overviewHalfPixelEdgesSettleTogether) {
  constexpr double extent = 2560.0;
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 800.0, .mass = 1.0};
  // At 150% these edges end on half pixels when open or closed. Opposite sides
  // of the output approach them from opposite directions.
  const double edges[] = {10.0, 11.0, 1262.0, 1298.0, 2549.0, 2550.0};
  for (bool closing : {false, true}) {
    const double startZoom = closing ? kZoom : 1.0;
    const double targetZoom = closing ? 1.0 : kZoom;
    const double travel = std::ceil((1.0 - kZoom) * extent * kScale * (closing ? 0.5 : 1.6)) + 1.0;
    const auto pixel = [&](double edge, double value) {
      const double zoom = std::lerp(startZoom, targetZoom, value);
      return std::lround(umbriel::previewZoomEdge(edge, extent, zoom, kZoom, kScale) * kScale);
    };
    umbriel::AnimatedValue animation{0.0};
    animation.settleSpring(1.0, spring);
    CHECK(animation.tick(1000.0));
    bool finishedTail = false;
    for (int frame = 1; frame < 400 && animation.animating(); ++frame) {
      animation.tick(1000.0 + frame * 1000.0 / kRefreshHz);
      const double before = animation.current();
      if (animation.finishSpringTail(travel)) {
        finishedTail = true;
        for (double edge : edges) {
          CHECK_EQ(pixel(edge, before), pixel(edge, animation.current()));
        }
      }
    }
    CHECK(finishedTail);
    CHECK(!animation.animating());
    for (double edge : edges) {
      const double target = extent / 2.0 + (edge - extent / 2.0) * targetZoom;
      CHECK_EQ(pixel(edge, 1.0), std::lround(target * kScale));
    }
  }
}

UMBRIEL_TEST(previewProjectionCentersRowsOnTranslatedOutputs) {
  for (bool horizontal : {false, true}) {
    const umbriel::PreviewProjection projection = projectionForAxis(horizontal);
    const wlr_fbox world = backgroundBox(projection);
    const wlr_fbox selected = umbriel::projectPreviewPresentationBox(projection, world, 3.0, 3);
    const auto centered = physicalEdges(projection, selected);
    CHECK_EQ(centered[0], 640);
    CHECK_EQ(centered[1], 400);
    CHECK_EQ(centered[2], 1920);
    CHECK_EQ(centered[3], 1200);

    const wlr_fbox neighbor = umbriel::projectPreviewPresentationBox(projection, world, 3.0, 4);
    const auto adjacent = physicalEdges(projection, neighbor);
    const size_t axis = horizontal ? 0 : 1;
    const size_t other = horizontal ? 1 : 0;
    const umbriel::PreviewGrid grid = umbriel::previewGrid(0, 0, kOutputWidth, kOutputHeight, kZoom, horizontal);
    CHECK(std::abs((adjacent[axis] - centered[axis]) - grid.step(horizontal) * kScale) <= 1.0);
    CHECK_EQ(adjacent[other], centered[other]);
    CHECK_EQ(adjacent[other + 2], centered[other + 2]);

    // Renumbering the strip leaves a fractional row presentation unchanged.
    const auto fractional =
        physicalEdges(projection, umbriel::projectPreviewPresentationBox(projection, world, 2.25, 3));
    CHECK(fractional == physicalEdges(projection, umbriel::projectPreviewPresentationBox(projection, world, 4.25, 5)));

    umbriel::PreviewProjection translated = projection;
    translated.outputBox.x -= 3000;
    translated.outputBox.y += 2000;
    CHECK(
        fractional
        == physicalEdges(
            translated, umbriel::projectPreviewPresentationBox(translated, backgroundBox(translated), 2.25, 3)
        )
    );
  }
}

UMBRIEL_TEST(previewProjectionKeepsCardSharedEdgesTogether) {
  for (bool horizontal : {false, true}) {
    umbriel::PreviewProjection projection = projectionForAxis(horizontal);
    const wlr_fbox first{projection.outputBox.x + 100.25, projection.outputBox.y + 200.5, 300.5, 150.25};
    const wlr_fbox second{first.x + first.width, first.y, 200.75, first.height};
    for (double zoom : {kZoom, 0.7, 1.0}) {
      projection.zoom = zoom;
      for (double scroll : {2.0, 2.25, 2.75}) {
        const auto a = physicalEdges(projection, umbriel::projectPreviewPresentationBox(projection, first, scroll, 2));
        const auto b = physicalEdges(projection, umbriel::projectPreviewPresentationBox(projection, second, scroll, 2));
        CHECK_EQ(a[2], b[0]);
        CHECK_EQ(a[1], b[1]);
        CHECK_EQ(a[3], b[3]);
      }
    }
  }
}

UMBRIEL_TEST(filmstripSpringSettlesAllProjectedBackgroundAndCardEdges) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  for (bool horizontal : {false, true}) {
    const umbriel::PreviewProjection projection = projectionForAxis(horizontal);
    const wlr_fbox background = backgroundBox(projection);
    constexpr size_t backgroundCount = 6;
    std::vector<PreviewRow> rows;
    rows.reserve(backgroundCount + 3);
    for (size_t index = 0; index < backgroundCount; ++index) {
      rows.push_back({background, index});
    }
    rows.push_back({{background.x + 10.2, background.y + 11.2, 120.25, 90.5}, 0});
    rows.push_back({{background.x + 1262.4, background.y + 400.2, 200.6, 300.4}, 2});
    rows.push_back({{background.x + 100.3, background.y + 900.4, 500.2, 100.3}, 5});
    const auto edgesSettled = [&](double target, double remaining) {
      return allRowEdgesSettled(projection, rows, target, remaining);
    };
    for (double target : {4.0, 2.2}) {
      for (int direction : {-1, 1}) {
        umbriel::AnimatedValue scroll{target - direction * 4.0};
        scroll.settleSpring(target, spring);
        CHECK(scroll.tick(1000.0));
        bool finishedTail = false;
        for (int frame = 1; frame < 400 && scroll.animating(); ++frame) {
          const double previous = scroll.current();
          CHECK(scroll.tick(1000.0 + frame * 1000.0 / kRefreshHz));
          const double before = scroll.current();
          for (const PreviewRow& row : rows) {
            const auto oldEdges = physicalEdges(
                projection, umbriel::projectPreviewPresentationBox(projection, row.world, previous, row.index)
            );
            const auto newEdges = physicalEdges(
                projection, umbriel::projectPreviewPresentationBox(projection, row.world, before, row.index)
            );
            const size_t axis = horizontal ? 0 : 1;
            CHECK(direction > 0 ? newEdges[axis] <= oldEdges[axis] : newEdges[axis] >= oldEdges[axis]);
            CHECK(direction > 0 ? newEdges[axis + 2] <= oldEdges[axis + 2] : newEdges[axis + 2] >= oldEdges[axis + 2]);
          }
          if (umbriel::finishPreviewRowScrollSpringTail(scroll, projection, false, edgesSettled)) {
            finishedTail = true;
            for (const PreviewRow& row : rows) {
              CHECK(
                  physicalEdges(
                      projection, umbriel::projectPreviewPresentationBox(projection, row.world, before, row.index)
                  )
                  == physicalEdges(
                      projection, umbriel::projectPreviewPresentationBox(projection, row.world, target, row.index)
                  )
              );
            }
          }
        }
        CHECK(finishedTail);
        CHECK(!scroll.animating());
        CHECK_EQ(scroll.current(), target);
        for (int frame = 0; frame < 5; ++frame) {
          CHECK(!scroll.tick(4000.0 + frame * 1000.0 / kRefreshHz));
          for (const PreviewRow& row : rows) {
            CHECK(
                physicalEdges(
                    projection,
                    umbriel::projectPreviewPresentationBox(projection, row.world, scroll.current(), row.index)
                )
                == physicalEdges(
                    projection, umbriel::projectPreviewPresentationBox(projection, row.world, target, row.index)
                )
            );
          }
        }
      }
    }
  }
}

UMBRIEL_TEST(filmstripSpringWaitsForCardEdgesAndActiveZoom) {
  const umbriel::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  constexpr double kDisplacement = 0.00005;
  for (bool horizontal : {false, true}) {
    umbriel::PreviewProjection projection = projectionForAxis(horizontal);
    const wlr_fbox background = backgroundBox(projection);
    const wlr_fbox card{background.x + 0.49, background.y + 0.49, 100.0, 100.0};
    const std::vector<PreviewRow> rows{{background, 0}, {card, 0}};
    // This excursion moves either axis by less than 0.071 physical pixels:
    // the background has 0.125 pixels of margin, but the card has only 0.0075.
    CHECK(umbriel::previewRowEdgesSettled(projection, background, 0, 0.0, kDisplacement));
    CHECK(!umbriel::previewRowEdgesSettled(projection, card, 0, 0.0, kDisplacement));
    umbriel::AnimatedValue scroll{kDisplacement};
    scroll.settleSpring(0.0, spring);
    const auto edgesSettled = [&](double target, double remaining) {
      return allRowEdgesSettled(projection, rows, target, remaining);
    };
    CHECK(!umbriel::finishPreviewRowScrollSpringTail(scroll, projection, false, edgesSettled));
    CHECK(scroll.animating());
    CHECK_EQ(scroll.current(), kDisplacement);

    scroll.snap(0.000001);
    scroll.settleSpring(0.0, spring);
    CHECK(edgesSettled(scroll.target(), 0.000001));
    CHECK(!umbriel::finishPreviewRowScrollSpringTail(scroll, projection, true, edgesSettled));
    CHECK(scroll.animating());
    for (double zoom : {kZoom - 0.01, 1.01}) {
      projection.zoom = zoom;
      CHECK(!umbriel::finishPreviewRowScrollSpringTail(scroll, projection, false, edgesSettled));
      CHECK(scroll.animating());
    }
    projection.zoom = kZoom;
    CHECK(umbriel::finishPreviewRowScrollSpringTail(scroll, projection, false, edgesSettled));
    CHECK(!scroll.animating());
    CHECK_EQ(scroll.current(), 0.0);
  }
}

int main() { return RUN_TESTS(); }
