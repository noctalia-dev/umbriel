#include "view/presentation.h"

#include "check.h"

// clang-format off
#include "wlr.h"
// clang-format on

// A fullscreen client whose buffer does not match the output is centered rather than scaled, and that centering lives
// in the scene node's position. The wlroots xdg scene helper rewrites that position to (-geometry.x, -geometry.y) on
// every commit, so the offset has to be re-applied afterwards or an oversized fullscreen buffer drifts to the top left
// on the next frame the client draws.
UMBRIEL_TEST(fullscreenCenteringSurvivesSceneReconfiguration) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }
  wlr_scene_tree* surfaceTree = wlr_scene_tree_create(&scene->tree);
  CHECK(surfaceTree != nullptr);
  if (surfaceTree == nullptr) {
    wlr_scene_node_destroy(&scene->tree.node);
    return;
  }

  umbriel::ViewPresentation presentation;
  // Non-zero geometry origin, as a client with CSD shadows commits: the node
  // position is the centering offset minus that origin, not the offset alone.
  const wlr_box geometry{40, 30, 1920, 1080};
  presentation.updateFullscreen(true, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(presentation.offsetX(), 320);
  CHECK_EQ(presentation.offsetY(), 180);
  CHECK_EQ(surfaceTree->node.x, 280);
  CHECK_EQ(surfaceTree->node.y, 150);

  // What the scene helper does on a commit. Re-running the fullscreen update is
  // what puts the centering back.
  wlr_scene_node_set_position(&surfaceTree->node, -geometry.x, -geometry.y);
  presentation.updateFullscreen(true, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(surfaceTree->node.x, 280);
  CHECK_EQ(surfaceTree->node.y, 150);

  // Leaving fullscreen drops the centering, otherwise the tiled view renders
  // offset by half a size difference that no longer applies.
  presentation.updateFullscreen(false, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(presentation.offsetX(), 0);
  CHECK_EQ(presentation.offsetY(), 0);
  CHECK_EQ(surfaceTree->node.x, -40);
  CHECK_EQ(surfaceTree->node.y, -30);

  wlr_scene_node_destroy(&scene->tree.node);
}

UMBRIEL_TEST(retargetingToTheDisplayedSizeStopsTheOldTrajectory) {
  umbriel::ViewPresentation presentation;
  presentation.setSize(400, 300);
  presentation.animateTo(800, 600, 200);
  CHECK(presentation.animating());
  presentation.animateTo(400, 300, 200);
  CHECK(!presentation.animating());
  CHECK(!presentation.tick(1000));
  CHECK_EQ(presentation.width(), 400);
  CHECK_EQ(presentation.height(), 300);
}

UMBRIEL_TEST(resizeGapFillExcludesClientContentAndInput) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }
  umbriel::ViewPresentation presentation;
  presentation.createBackdrop(&scene->tree);
  presentation.updateResizeBackdrop(true, 600, 450, 400, 300, corner_radii_new(0, 0, 12, 12));
  wlr_scene_rect* fill = nullptr;
  wlr_scene_node* node;
  wl_list_for_each(node, &scene->tree.children, link) {
    if (node->enabled && node->type == WLR_SCENE_NODE_RECT) {
      fill = wlr_scene_rect_from_node(node);
    }
  }
  CHECK(fill != nullptr);
  if (fill != nullptr) {
    CHECK(!fill->accepts_input);
    CHECK_EQ(fill->width, 600);
    CHECK_EQ(fill->height, 450);
    const pixman_region32_t* clip = wlr_scene_node_get_clip_region(&fill->node);
    CHECK(clip != nullptr);
    if (clip != nullptr) {
      CHECK(!pixman_region32_contains_point(clip, 200, 150, nullptr));
      CHECK(pixman_region32_contains_point(clip, 500, 150, nullptr));
      CHECK(pixman_region32_contains_point(clip, 200, 400, nullptr));
    }
    wlr_scene_tree* snapshot = wlr_scene_tree_create(&scene->tree);
    CHECK(snapshot != nullptr);
    if (snapshot != nullptr) {
      presentation.snapshotBackdrops(snapshot);
      CHECK(!wl_list_empty(&snapshot->children));
      wlr_scene_node* copyNode;
      wl_list_for_each(copyNode, &snapshot->children, link) {
        const wlr_scene_rect* copy = wlr_scene_rect_from_node(copyNode);
        CHECK_EQ(copy->corners.top_left, 0);
        CHECK_EQ(copy->corners.bottom_left, 12);
        const pixman_region32_t* copyClip = wlr_scene_node_get_clip_region(copyNode);
        CHECK(copyClip != nullptr);
        if (copyClip != nullptr && clip != nullptr) {
          CHECK(pixman_region32_equal(copyClip, clip));
        }
      }
    }
    presentation.updateResizeBackdrop(false, 600, 450, 400, 300, corner_radii_all(12));
    CHECK(!fill->node.enabled);
    presentation.updateResizeBackdrop(true, 600, 450, 600, 450, corner_radii_all(12));
    CHECK(!fill->node.enabled);
  }
  wlr_scene_node_destroy(&scene->tree.node);
}

int main() { return RUN_TESTS(); }
