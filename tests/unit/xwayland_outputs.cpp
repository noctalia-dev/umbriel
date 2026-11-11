// The X screen Xwayland is told about. X11 clients place windows and read monitor geometry in it, so outputs must not
// overlap there, and a window covering a whole output must be exactly as large as the output in X pixels.
#include "check.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on
#include "xwayland/outputs.h"

extern "C" {
#include <wlr/backend/headless.h>
}

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

  struct OutputSpec {
    int width;
    int height;
    float scale;
    int x;
    int y;
  };

  struct Fixture {
    wl_display* display = nullptr;
    wlr_backend* backend = nullptr;
    wlr_output_layout* layout = nullptr;
    std::vector<wlr_output*> outputs;

    [[nodiscard]] bool setUp(const std::vector<OutputSpec>& specs) {
      display = wl_display_create();
      if (display == nullptr) {
        return false;
      }
      backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
      layout = wlr_output_layout_create(display);
      if (backend == nullptr || layout == nullptr) {
        return false;
      }
      for (const OutputSpec& spec : specs) {
        wlr_output* output = wlr_headless_add_output(backend, spec.width, spec.height);
        if (output == nullptr) {
          return false;
        }
        wlr_output_state state{};
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, spec.width, spec.height, 0);
        wlr_output_state_set_scale(&state, spec.scale);
        const bool committed = wlr_output_commit_state(output, &state);
        wlr_output_state_finish(&state);
        if (!committed || wlr_output_layout_add(layout, output, spec.x, spec.y) == nullptr) {
          return false;
        }
        outputs.push_back(output);
      }
      return true;
    }

    // Runs after the XwaylandOutputs under test, which is declared later and destroys its global first.
    ~Fixture() {
      if (layout != nullptr) {
        wlr_output_layout_destroy(layout);
      }
      if (backend != nullptr) {
        wlr_backend_destroy(backend);
      }
      if (display != nullptr) {
        wl_display_destroy(display);
      }
    }
  };

  // A 4K output at 1.25, a 1440p output above it that puts the layout origin above the 4K one, and a 1080p output to
  // its right.
  const std::vector<OutputSpec> kMixedScales = {
      {.width = 3840, .height = 2160, .scale = 1.25F, .x = 0, .y = 0},
      {.width = 2560, .height = 1440, .scale = 1.0F, .x = 1300, .y = -1440},
      {.width = 1920, .height = 1080, .scale = 1.0F, .x = 3072, .y = 0},
  };

} // namespace

UMBRIEL_TEST(logicalScreenIsTheLayoutMovedToTheOrigin) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  int renderScaleCalls = 0;
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, false, [&](const wlr_output*) {
    ++renderScaleCalls;
    return 2.5;
  });

  const umbriel::XwaylandRegion main = outputs.region(fixture.outputs[0], 0, 0);
  CHECK_EQ(renderScaleCalls, 0);
  CHECK_EQ(outputs.scale(), 1.0);
  CHECK_EQ(main.x.x, 0);
  CHECK_EQ(main.x.y, 1440);
  CHECK_EQ(main.x.width, 3072);
  CHECK_EQ(main.x.height, 1728);
  CHECK_EQ(main.toX(100), 100);
  CHECK_EQ(main.toY(100), 1540);
  CHECK_EQ(main.toXWidth(640), 640);
}

UMBRIEL_TEST(nativeScreenKeepsTheArrangementWithoutOverlap) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true);

  std::vector<wlr_box> boxes;
  boxes.reserve(fixture.outputs.size());
  for (wlr_output* output : fixture.outputs) {
    boxes.push_back(outputs.region(output, 0, 0).x);
  }
  // Each output at its physical size.
  CHECK_EQ(boxes[0].width, 3840);
  CHECK_EQ(boxes[0].height, 2160);
  CHECK_EQ(boxes[2].width, 1920);
  // The right neighbour still starts where the scaled output ends, and the one above still ends above it.
  CHECK_EQ(boxes[2].x, boxes[0].x + boxes[0].width);
  CHECK_EQ(boxes[2].y, boxes[0].y);
  CHECK(boxes[1].y + boxes[1].height <= boxes[0].y);
  for (size_t a = 0; a < boxes.size(); ++a) {
    for (size_t b = a + 1; b < boxes.size(); ++b) {
      wlr_box overlap{};
      CHECK(!wlr_box_intersection(&overlap, &boxes[a], &boxes[b]));
    }
  }
}

UMBRIEL_TEST(nativeScreenMapsWindowsThroughTheirOutputScale) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true);

  const umbriel::XwaylandRegion main = outputs.region(fixture.outputs[0], 0, 0);
  CHECK_EQ(main.scale, 1.25);
  CHECK_EQ(main.toX(800), main.x.x + 1000);
  CHECK_EQ(main.toLayoutX(main.x.x + 1000), 800);
  CHECK_EQ(main.toXWidth(1000), 1250);
  CHECK_EQ(main.toLayoutHeight(1250), 1000);

  // An X point in the gap between outputs belongs to the nearest one; one inside an output to that output.
  CHECK_EQ(outputs.regionAtX(main.x.x + 10, main.x.y + 10).scale, 1.25);
  const umbriel::XwaylandRegion right = outputs.region(fixture.outputs[2], 0, 0);
  CHECK_EQ(outputs.regionAtX(right.x.x + 10, right.x.y + 10).scale, 1.0);
}

UMBRIEL_TEST(wholeOutputMapsExactlyAtFractionalScale) {
  // 2560 / 1.5 rounds, so scaling the logical width back by 1.5 lands one pixel off the physical width.
  Fixture fixture;
  CHECK(fixture.setUp({{.width = 2560, .height = 1440, .scale = 1.5F, .x = 0, .y = 0}}));
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true);

  const umbriel::XwaylandRegion region = outputs.region(fixture.outputs[0], 0, 0);
  CHECK_EQ(region.toXWidth(region.layout.width), 2560);
  CHECK_EQ(region.toXHeight(region.layout.height), 1440);
  CHECK_EQ(region.toLayoutWidth(2560), region.layout.width);
  CHECK_EQ(region.toLayoutHeight(1440), region.layout.height);
}

UMBRIEL_TEST(primarySelectorFollowsIdentityAndLayoutMembership) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, false);
  wlr_output* main = fixture.outputs[0];
  std::free(main->make);
  std::free(main->model);
  std::free(main->serial);
  main->make = strdup("Microstep");
  main->model = strdup("MSI MAG 275QF");
  main->serial = strdup("CE2M165901246");

  CHECK(!outputs.primaryOutput(""));
  std::string connector = main->name;
  connector[0] = 'h';
  CHECK_EQ(outputs.primaryOutput(connector).value_or("unmanaged"), main->name);
  constexpr std::string_view kIdentity = "microstep MSI MAG 275QF ce2m165901246";
  CHECK_EQ(outputs.primaryOutput(kIdentity).value_or("unmanaged"), main->name);
  wlr_output_layout_remove(fixture.layout, main);
  CHECK_EQ(outputs.primaryOutput(kIdentity).value_or("unmanaged"), "");
  CHECK(wlr_output_layout_add(fixture.layout, main, 0, 0) != nullptr);
  CHECK_EQ(outputs.primaryOutput(kIdentity).value_or("unmanaged"), main->name);
  CHECK_EQ(outputs.primaryOutput("missing").value_or("unmanaged"), "");
}

UMBRIEL_TEST(nativeScreenPublishesTheRenderScale) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  // A forced render scale beyond the logical one lets X11 clients supersample.
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true, [](const wlr_output* output) {
    return static_cast<double>(output->scale) * 2.0;
  });

  const umbriel::XwaylandRegion main = outputs.region(fixture.outputs[0], 0, 0);
  CHECK_EQ(main.scale, 2.5);
  CHECK_EQ(main.toXWidth(1000), 2500);
  CHECK_EQ(main.toLayoutWidth(2500), 1000);

  // The whole-output mapping must agree with the coordinate mapping: a
  // fullscreen X window is configured to the X rectangle and drawn through the
  // same scale. The rectangle measures render-scale pixels (3072 logical x
  // 2.5), not the physical mode.
  CHECK_EQ(main.x.width, 7680);
  CHECK_EQ(main.x.height, 4320);
  CHECK_EQ(main.toXWidth(main.layout.width), main.x.width);
  CHECK_EQ(main.toX(main.layout.x + main.layout.width), main.x.x + main.x.width);
  CHECK_EQ(main.toLayoutWidth(main.x.width), main.layout.width);

  // Multi-output positions spread by the same factor as the rectangles, so
  // the right neighbour still starts where the scaled output ends.
  const umbriel::XwaylandRegion right = outputs.region(fixture.outputs[2], 0, 0);
  CHECK_EQ(right.x.x, main.x.x + main.x.width);
  CHECK_EQ(right.x.y, main.x.y);
}

UMBRIEL_TEST(nativeScreenKeepsNeighboursAdjacentWithAMixedRenderScale) {
  Fixture fixture;
  CHECK(fixture.setUp(kMixedScales));
  // The largest render scale sits on the RIGHT output, so scaling every origin
  // by it would open a strip between the other two. Each rectangle is sized by
  // its own factor: neighbours that touch in the layout must touch in X.
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true, [&fixture](const wlr_output* output) {
    return output == fixture.outputs[2] ? 5.0 : static_cast<double>(output->scale);
  });

  const wlr_box main = outputs.region(fixture.outputs[0], 0, 0).x;
  const wlr_box above = outputs.region(fixture.outputs[1], 0, 0).x;
  const wlr_box right = outputs.region(fixture.outputs[2], 0, 0).x;
  CHECK_EQ(main.width, 3840);
  CHECK_EQ(right.x, main.x + main.width);
  CHECK_EQ(right.y, main.y);
  CHECK_EQ(main.y, above.y + above.height);
  wlr_box overlap{};
  CHECK(!wlr_box_intersection(&overlap, &main, &right));
  CHECK(!wlr_box_intersection(&overlap, &main, &above));
}

UMBRIEL_TEST(nativeScreenKeepsARealLayoutGap) {
  Fixture fixture;
  // An 80 px hole between two 1080p outputs is deliberate; the X screen must
  // keep it, measured at the largest render scale.
  CHECK(fixture.setUp({
      {.width = 1920, .height = 1080, .scale = 1.0F, .x = 0, .y = 0},
      {.width = 1920, .height = 1080, .scale = 1.0F, .x = 2000, .y = 0},
  }));
  const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true, [&fixture](const wlr_output* output) {
    return output == fixture.outputs[1] ? 2.0 : 1.0;
  });

  const wlr_box left = outputs.region(fixture.outputs[0], 0, 0).x;
  const wlr_box right = outputs.region(fixture.outputs[1], 0, 0).x;
  CHECK_EQ(left.width, 1920);
  CHECK_EQ(right.width, 3840);
  CHECK_EQ(right.x - (left.x + left.width), 160);
}

UMBRIEL_TEST(nativeScreenPlacementIgnoresOutputRegistrationOrder) {
  // Two uneven monitors stacked to the left of a tall one: both end at layout
  // x = 1920, but the 2.0-scale one ends at X 3840 while the 1.0-scale one ends
  // at 1920. A tie in the layout edge must not depend on which was registered
  // first, and the tall output must start past the wider of them.
  const std::vector<OutputSpec> touching = {
      {.width = 1920, .height = 1080, .scale = 1.0F, .x = 0, .y = 0},
      {.width = 3840, .height = 2160, .scale = 2.0F, .x = 0, .y = 1080},
      {.width = 1920, .height = 2160, .scale = 1.0F, .x = 1920, .y = 0},
  };
  const std::vector<OutputSpec> reversed = {touching[1], touching[2], touching[0]};

  const auto place = [](const std::vector<OutputSpec>& specs) {
    Fixture fixture;
    CHECK(fixture.setUp(specs));
    const umbriel::XwaylandOutputs outputs(fixture.display, fixture.layout, true, [](const wlr_output* output) {
      return static_cast<double>(output->scale);
    });
    std::vector<wlr_box> boxes;
    boxes.reserve(fixture.outputs.size());
    for (wlr_output* output : fixture.outputs) {
      boxes.push_back(outputs.region(output, 0, 0).x);
    }
    for (size_t a = 0; a < boxes.size(); ++a) {
      for (size_t b = a + 1; b < boxes.size(); ++b) {
        wlr_box overlap{};
        CHECK(!wlr_box_intersection(&overlap, &boxes[a], &boxes[b]));
      }
    }
    const auto tall =
        std::ranges::find_if(specs, [](const OutputSpec& spec) { return spec.width == 1920 && spec.height == 2160; });
    return boxes[static_cast<size_t>(std::distance(specs.begin(), tall))].x;
  };

  const int first = place(touching);
  const int second = place(reversed);
  CHECK_EQ(first, 3840);
  CHECK_EQ(first, second);
}

int main() { return RUN_TESTS(); }
