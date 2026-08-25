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
  // offset by half the difference it no longer has.
  presentation.updateFullscreen(false, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(presentation.offsetX(), 0);
  CHECK_EQ(presentation.offsetY(), 0);
  CHECK_EQ(surfaceTree->node.x, -40);
  CHECK_EQ(surfaceTree->node.y, -30);

  wlr_scene_node_destroy(&scene->tree.node);
}

int main() { return RUN_TESTS(); }
