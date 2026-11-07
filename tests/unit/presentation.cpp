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

// A minimizing window collapses toward its anchor with its own box scaled, and never to an empty box.
UMBRIEL_TEST(minimizeCollapseScalesAboutTheConfiguredAnchor) {
  const wlr_box resting{100, 200, 400, 300};
  const wlr_box usable{0, 0, 1280, 720};

  // Center: the scaled box keeps the resting centre.
  const wlr_box centered = umbriel::minimizeCollapseBox(resting, 0.5, usable, umbriel::MinimizeAnchor::Center);
  CHECK_EQ(centered.width, 200);
  CHECK_EQ(centered.height, 150);
  CHECK_EQ(centered.x + centered.width / 2, resting.x + resting.width / 2);
  CHECK_EQ(centered.y + centered.height / 2, resting.y + resting.height / 2);

  const wlr_box left = umbriel::minimizeCollapseBox(resting, 0.5, usable, umbriel::MinimizeAnchor::Left);
  CHECK_EQ(left.x, usable.x);
  CHECK_EQ(left.y + left.height / 2, usable.y + usable.height / 2);

  const wlr_box right = umbriel::minimizeCollapseBox(resting, 0.5, usable, umbriel::MinimizeAnchor::Right);
  CHECK_EQ(right.x + right.width, usable.x + usable.width);
  CHECK_EQ(right.y + right.height / 2, usable.y + usable.height / 2);

  const wlr_box top = umbriel::minimizeCollapseBox(resting, 0.5, usable, umbriel::MinimizeAnchor::TopCenter);
  CHECK_EQ(top.y, usable.y);
  CHECK_EQ(top.x + top.width / 2, usable.x + usable.width / 2);

  const wlr_box bottom = umbriel::minimizeCollapseBox(resting, 0.5, usable, umbriel::MinimizeAnchor::BottomCenter);
  CHECK_EQ(bottom.y + bottom.height, usable.y + usable.height);
  CHECK_EQ(bottom.x + bottom.width / 2, usable.x + usable.width / 2);
}

UMBRIEL_TEST(minimizeCollapseIsIdentityAtFullScaleAndNeverEmpty) {
  const wlr_box resting{100, 200, 400, 300};
  const wlr_box usable{0, 0, 1280, 720};
  const wlr_box full = umbriel::minimizeCollapseBox(resting, 1.0, usable, umbriel::MinimizeAnchor::Center);
  CHECK_EQ(full.x, resting.x);
  CHECK_EQ(full.y, resting.y);
  CHECK_EQ(full.width, resting.width);
  CHECK_EQ(full.height, resting.height);

  // Out-of-range scales clamp, and the box keeps at least one pixel on each axis.
  const wlr_box tiny = umbriel::minimizeCollapseBox(resting, 0.0, usable, umbriel::MinimizeAnchor::Center);
  CHECK_EQ(tiny.width, 1);
  CHECK_EQ(tiny.height, 1);
  const wlr_box over = umbriel::minimizeCollapseBox(resting, 4.0, usable, umbriel::MinimizeAnchor::Left);
  CHECK_EQ(over.width, resting.width);
  CHECK_EQ(over.height, resting.height);
}

int main() { return RUN_TESTS(); }
