// Regression coverage for the SceneFX subtree clip (wlr_scene_tree_set_clip). A tree clip has to contain everything
// that keys off node->visible, which the compositor observes as output membership and the wl_surface enter/leave that
// follows it, plus hit testing, which walks the scene independently. Umbriel relies on exactly that to keep one
// output's windows off its neighbour.
#include "check.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
}

namespace {

  // Nothing renders here, so the smallest conforming wlr_buffer is enough. It lives in the fixture, so destroy only
  // releases the common buffer state.
  void testBufferDestroy(wlr_buffer* buffer) { wlr_buffer_finish(buffer); }

  constexpr wlr_buffer_impl kTestBufferImpl = {
      .destroy = testBufferDestroy,
      .get_dmabuf = nullptr,
      .get_shm = nullptr,
      .begin_data_ptr_access = nullptr,
      .end_data_ptr_access = nullptr,
  };

  // Records the enter/leave stream for one scene output. Membership itself is private to SceneFX; these signals are
  // what a client observes, and what scale-aware clients map their input from.
  struct OutputTracker {
    wl_listener enter{};
    wl_listener leave{};
    const wlr_scene_output* watched = nullptr;
    bool inside = false;
    int enters = 0;
    int leaves = 0;

    void watch(wlr_scene_buffer* buffer, const wlr_scene_output* output) {
      watched = output;
      enter.notify = onEnter;
      leave.notify = onLeave;
      wl_signal_add(&buffer->events.output_enter, &enter);
      wl_signal_add(&buffer->events.output_leave, &leave);
    }

    void stop() {
      wl_list_remove(&enter.link);
      wl_list_remove(&leave.link);
    }

    static void onEnter(wl_listener* listener, void* data) {
      OutputTracker* self = nullptr;
      self = wl_container_of(listener, self, enter);
      if (data == self->watched) {
        self->inside = true;
        ++self->enters;
      }
    }

    static void onLeave(wl_listener* listener, void* data) {
      OutputTracker* self = nullptr;
      self = wl_container_of(listener, self, leave);
      if (data == self->watched) {
        self->inside = false;
        ++self->leaves;
      }
    }
  };

  struct Fixture {
    // Both outputs are 1280x720, side by side. The buffer straddles x=1280 with 180px on the left output and 220px on
    // the right one, so both clear SceneFX's 10% membership threshold and the right one wins the primary output.
    static constexpr int kOutputWidth = 1280;
    static constexpr int kOutputHeight = 720;
    static constexpr int kBufferWidth = 400;
    static constexpr int kBufferHeight = 300;
    static constexpr int kBufferX = 1100;
    static constexpr int kBufferY = 100;

    wl_display* display = nullptr;
    wlr_backend* backend = nullptr;
    wlr_scene* scene = nullptr;
    wlr_scene_output* left = nullptr;
    wlr_scene_output* right = nullptr;
    wlr_scene_tree* tree = nullptr;
    wlr_scene_buffer* buffer = nullptr;
    wlr_buffer surface{};
    OutputTracker onLeft;
    OutputTracker onRight;

    [[nodiscard]] bool setUp() {
      display = wl_display_create();
      if (display == nullptr) {
        return false;
      }
      backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
      scene = wlr_scene_create();
      if (backend == nullptr || scene == nullptr) {
        return false;
      }

      left = addOutput(0);
      right = addOutput(kOutputWidth);
      if (left == nullptr || right == nullptr) {
        return false;
      }

      wlr_buffer_init(&surface, &kTestBufferImpl, kBufferWidth, kBufferHeight);
      tree = wlr_scene_tree_create(&scene->tree);
      if (tree == nullptr) {
        return false;
      }
      // A scene buffer holding no buffer is never visible, so the node can be placed before anyone is watching and
      // the first enter observed by the trackers is the one the content itself causes.
      buffer = wlr_scene_buffer_create(tree, nullptr);
      if (buffer == nullptr) {
        return false;
      }
      wlr_scene_node_set_position(&buffer->node, kBufferX, kBufferY);
      onLeft.watch(buffer, left);
      onRight.watch(buffer, right);
      wlr_scene_buffer_set_buffer(buffer, &surface);
      return true;
    }

    void tearDown() {
      if (buffer != nullptr) {
        onLeft.stop();
        onRight.stop();
      }
      if (scene != nullptr) {
        wlr_scene_node_destroy(&scene->tree.node);
      }
      if (surface.impl != nullptr) {
        wlr_buffer_drop(&surface);
      }
      if (backend != nullptr) {
        wlr_backend_destroy(backend);
      }
      if (display != nullptr) {
        wl_display_destroy(display);
      }
    }

    [[nodiscard]] wlr_scene_node* nodeAt(double lx, double ly) const {
      double nx = 0;
      double ny = 0;
      return wlr_scene_node_at(&scene->tree.node, lx, ly, &nx, &ny);
    }

  private:
    wlr_scene_output* addOutput(int x) {
      wlr_output* output = wlr_headless_add_output(backend, kOutputWidth, kOutputHeight);
      if (output == nullptr) {
        return nullptr;
      }

      // Membership skips disabled outputs, so the mode has to be committed before anything is asserted.
      wlr_output_state state{};
      wlr_output_state_init(&state);
      wlr_output_state_set_enabled(&state, true);
      wlr_output_state_set_custom_mode(&state, kOutputWidth, kOutputHeight, 0);
      const bool committed = wlr_output_commit_state(output, &state);
      wlr_output_state_finish(&state);
      if (!committed) {
        return nullptr;
      }

      wlr_scene_output* sceneOutput = wlr_scene_output_create(scene, output);
      if (sceneOutput != nullptr) {
        wlr_scene_output_set_position(sceneOutput, x, 0);
      }
      return sceneOutput;
    }
  };

  constexpr wlr_box kLeftOutputBox = {0, 0, Fixture::kOutputWidth, Fixture::kOutputHeight};

} // namespace

UMBRIEL_TEST(unclippedTreeStraddlesBothOutputs) {
  Fixture fixture;
  CHECK(fixture.setUp());

  CHECK(fixture.onLeft.inside);
  CHECK(fixture.onRight.inside);
  CHECK(fixture.buffer->primary_output == fixture.right);
  CHECK(fixture.nodeAt(1300, 150) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(clipDropsNeighbourMembershipAndFiresLeave) {
  Fixture fixture;
  CHECK(fixture.setUp());

  wlr_scene_tree_set_clip(fixture.tree, &kLeftOutputBox);

  CHECK(fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  // Exactly one transition, and none on the output that keeps the content.
  CHECK_EQ(fixture.onRight.leaves, 1);
  CHECK_EQ(fixture.onLeft.leaves, 0);
  CHECK_EQ(fixture.onLeft.enters, 1);
  CHECK(fixture.buffer->primary_output == fixture.left);

  // Past the clipped edge a click falls through; just inside it still lands on the buffer.
  CHECK(fixture.nodeAt(1300, 150) == nullptr);
  CHECK(fixture.nodeAt(1200, 150) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(clearingTheClipRestoresMembership) {
  Fixture fixture;
  CHECK(fixture.setUp());

  wlr_scene_tree_set_clip(fixture.tree, &kLeftOutputBox);
  CHECK(!fixture.onRight.inside);

  wlr_scene_tree_set_clip(fixture.tree, nullptr);

  CHECK(fixture.onLeft.inside);
  CHECK(fixture.onRight.inside);
  CHECK_EQ(fixture.onRight.enters, 2);
  CHECK(fixture.buffer->primary_output == fixture.right);
  CHECK(fixture.nodeAt(1300, 150) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(clipOutsideTheNodeHidesItEntirely) {
  Fixture fixture;
  CHECK(fixture.setUp());

  constexpr wlr_box corner = {0, 0, 50, 50};
  wlr_scene_tree_set_clip(fixture.tree, &corner);

  CHECK(!fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  CHECK(fixture.buffer->primary_output == nullptr);
  CHECK(fixture.nodeAt(1200, 150) == nullptr);

  fixture.tearDown();
}

UMBRIEL_TEST(nestedClipsIntersect) {
  Fixture fixture;
  CHECK(fixture.setUp());

  // Both clips have to accumulate. The inner tree sits at the origin of the outer one, so its clip lands in the same
  // layout coordinates.
  wlr_scene_tree* inner = wlr_scene_tree_create(fixture.tree);
  CHECK(inner != nullptr);
  wlr_scene_node_reparent(&fixture.buffer->node, inner);

  wlr_scene_tree_set_clip(fixture.tree, &kLeftOutputBox);
  constexpr wlr_box narrower = {0, 0, 1150, Fixture::kOutputHeight};
  wlr_scene_tree_set_clip(inner, &narrower);

  CHECK(fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);

  // 1200 is inside the outer clip but outside the inner one.
  CHECK(fixture.nodeAt(1200, 150) == nullptr);
  CHECK(fixture.nodeAt(1140, 150) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(hitTestSeededInsideTheClippedSubtreeStillClips) {
  Fixture fixture;
  CHECK(fixture.setUp());

  wlr_scene_tree_set_clip(fixture.tree, &kLeftOutputBox);

  double nx = 0;
  double ny = 0;
  // A walk that starts below the clipped tree still has to honour the clips above it.
  CHECK(wlr_scene_node_at(&fixture.buffer->node, 1300, 150, &nx, &ny) == nullptr);
  CHECK(wlr_scene_node_at(&fixture.buffer->node, 1200, 150, &nx, &ny) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(regionClipPreservesDisconnectedPiecesAndCutsTheHole) {
  Fixture fixture;
  CHECK(fixture.setUp());

  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, 100, Fixture::kBufferHeight);
  pixman_region32_union_rect(&region, &region, 300, 0, 100, Fixture::kBufferHeight);
  CHECK(wlr_scene_node_set_clip_region(&fixture.buffer->node, &region));
  pixman_region32_fini(&region);

  // The source region was copied; its destruction leaves both islands visible.
  CHECK(fixture.nodeAt(1150, 150) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1450, 150) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1300, 150) == nullptr);
  CHECK(fixture.onLeft.inside);
  CHECK(fixture.onRight.inside);

  fixture.tearDown();
}

UMBRIEL_TEST(emptyRegionLeavesOutputsAndClearingItReenters) {
  Fixture fixture;
  CHECK(fixture.setUp());

  pixman_region32_t empty;
  pixman_region32_init(&empty);
  CHECK(wlr_scene_node_set_clip_region(&fixture.tree->node, &empty));
  pixman_region32_fini(&empty);

  CHECK(!fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  CHECK_EQ(fixture.onLeft.leaves, 1);
  CHECK_EQ(fixture.onRight.leaves, 1);
  CHECK(fixture.buffer->primary_output == nullptr);
  CHECK(fixture.nodeAt(1200, 150) == nullptr);
  CHECK(wlr_scene_node_get_clip_region(&fixture.tree->node) != nullptr);

  CHECK(wlr_scene_node_set_clip_region(&fixture.tree->node, nullptr));
  CHECK(fixture.onLeft.inside);
  CHECK(fixture.onRight.inside);
  CHECK_EQ(fixture.onLeft.enters, 2);
  CHECK_EQ(fixture.onRight.enters, 2);
  CHECK(fixture.nodeAt(1200, 150) == &fixture.buffer->node);
  CHECK(wlr_scene_node_get_clip_region(&fixture.tree->node) == nullptr);

  fixture.tearDown();
}

UMBRIEL_TEST(regionAndRectangleClipsIntersectOnTheSameTree) {
  Fixture fixture;
  CHECK(fixture.setUp());

  pixman_region32_t region;
  pixman_region32_init_rect(&region, 1100, 100, 100, Fixture::kBufferHeight);
  pixman_region32_union_rect(&region, &region, 1400, 100, 100, Fixture::kBufferHeight);
  CHECK(wlr_scene_node_set_clip_region(&fixture.tree->node, &region));
  pixman_region32_fini(&region);
  wlr_scene_tree_set_clip(fixture.tree, &kLeftOutputBox);

  CHECK(fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  CHECK(fixture.nodeAt(1150, 150) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1450, 150) == nullptr);

  // Removing only the rectangle restores the second island, keeping the hole.
  wlr_scene_tree_set_clip(fixture.tree, nullptr);
  CHECK(fixture.onRight.inside);
  CHECK_EQ(fixture.onRight.enters, 2);
  CHECK(fixture.nodeAt(1450, 150) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1300, 150) == nullptr);

  CHECK(wlr_scene_node_set_clip_region(&fixture.tree->node, nullptr));
  CHECK(fixture.nodeAt(1300, 150) == &fixture.buffer->node);

  fixture.tearDown();
}

UMBRIEL_TEST(regionClipsFollowTreeMovementAndSeededHitTests) {
  Fixture fixture;
  CHECK(fixture.setUp());

  pixman_region32_t region;
  pixman_region32_init_rect(&region, 1100, 100, 100, Fixture::kBufferHeight);
  CHECK(wlr_scene_node_set_clip_region(&fixture.tree->node, &region));
  pixman_region32_fini(&region);
  wlr_scene_node_set_position(&fixture.tree->node, 30, 40);

  CHECK(fixture.nodeAt(1150, 190) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1110, 190) == nullptr);
  double nx = 0;
  double ny = 0;
  CHECK(wlr_scene_node_at(&fixture.buffer->node, 1150, 190, &nx, &ny) == &fixture.buffer->node);
  CHECK(wlr_scene_node_at(&fixture.buffer->node, 1250, 190, &nx, &ny) == nullptr);

  fixture.tearDown();
}

UMBRIEL_TEST(regionHoleUncoversPreviouslyOccludedContent) {
  Fixture fixture;
  CHECK(fixture.setUp());

  constexpr float opaque[] = {1.F, 0.F, 0.F, 1.F};
  wlr_scene_rect* occluder = wlr_scene_rect_create(fixture.tree, Fixture::kBufferWidth, Fixture::kBufferHeight, opaque);
  CHECK(occluder != nullptr);
  wlr_scene_node_set_position(&occluder->node, Fixture::kBufferX, Fixture::kBufferY);
  CHECK(!fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  CHECK_EQ(fixture.onLeft.leaves, 1);
  CHECK_EQ(fixture.onRight.leaves, 1);

  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, 100, Fixture::kBufferHeight);
  pixman_region32_union_rect(&region, &region, 300, 0, 100, Fixture::kBufferHeight);
  CHECK(wlr_scene_node_set_clip_region(&occluder->node, &region));
  pixman_region32_fini(&region);

  CHECK(fixture.onLeft.inside);
  CHECK(fixture.onRight.inside);
  CHECK_EQ(fixture.onLeft.enters, 2);
  CHECK_EQ(fixture.onRight.enters, 2);
  CHECK(fixture.nodeAt(1300, 150) == &fixture.buffer->node);
  CHECK(fixture.nodeAt(1150, 150) == &occluder->node);

  CHECK(wlr_scene_node_set_clip_region(&occluder->node, nullptr));
  CHECK(!fixture.onLeft.inside);
  CHECK(!fixture.onRight.inside);
  CHECK_EQ(fixture.onLeft.leaves, 2);
  CHECK_EQ(fixture.onRight.leaves, 2);
  CHECK(fixture.nodeAt(1300, 150) == &occluder->node);

  fixture.tearDown();
}

int main() { return RUN_TESTS(); }
