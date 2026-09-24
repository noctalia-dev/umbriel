#include "check.h"
#include "scene/nine_rect.h"
#include "wlr.h"

#include <cmath>

using namespace umbriel;
namespace {
  std::shared_ptr<NineRectAsset> asset() {
    auto a = std::make_shared<NineRectAsset>();
    a->width = 8;
    a->height = 8;
    a->slices = {2, 2, 2, 2};
    a->contentInsets = FrameInsets{1, 1, 1, 1};
    a->tileX = true;
    a->pixels.assign(64, 0xFFFFFFFF);
    return a;
  }
} // namespace
UMBRIEL_TEST(input_uses_frame_geometry_and_passes_through_client_overlap) {
  auto* scene = wlr_scene_create();
  const float color[4]{1, 0, 0, 1};
  auto* below = wlr_scene_rect_create(&scene->tree, 100, 100, color);
  wlr_scene_node_set_position(&below->node, -10, -10);
  auto* view = wlr_scene_tree_create(&scene->tree);
  NineRectDecoration decoration;
  decoration.update(view, asset(), 70, 50);
  double x, y;
  CHECK(wlr_scene_node_at(&scene->tree.node, 0.5, -0.5, &x, &y) != &below->node);
  CHECK(wlr_scene_node_at(&scene->tree.node, 64.5, -0.5, &x, &y) != &below->node);
  CHECK(wlr_scene_node_at(&scene->tree.node, 69.5, -0.5, &x, &y) != &below->node);
  CHECK(wlr_scene_node_at(&scene->tree.node, 0.5, 0.5, &x, &y) == &below->node);
  CHECK(wlr_scene_node_at(&scene->tree.node, 2.5, 30.5, &x, &y) == &below->node);
  decoration.setEnabled(false);
  CHECK(wlr_scene_node_at(&scene->tree.node, 0.5, -0.5, &x, &y) == &below->node);
  wlr_scene_node_destroy(&scene->tree.node);
}
UMBRIEL_TEST(snapshot_retains_theme_generation_and_rejects_input) {
  auto* scene = wlr_scene_create();
  auto* view = wlr_scene_tree_create(&scene->tree);
  auto* snapshot = wlr_scene_tree_create(&scene->tree);
  auto old = asset();
  old->tintWithBorderColor = true;
  std::weak_ptr<const NineRectAsset> weak = old;
  NineRectDecoration decoration;
  decoration.update(view, old, 70, 50);
  decoration.setTint({0.2F, 0.4F, 0.6F, 0.8F});
  decoration.snapshot(snapshot, 0, 0);
  decoration.update(view, asset(), 75, 55);
  old.reset();
  CHECK(!weak.expired());
  int count = 0;
  wlr_scene_node_for_each_buffer(
      &snapshot->node,
      [](wlr_scene_buffer* buffer, int, int, void* data) {
        ++*static_cast<int*>(data);
        CHECK(buffer->buffer != nullptr);
        CHECK_EQ(buffer->slice_tint[0], 0.2F);
        CHECK_EQ(buffer->slice_tint[3], 0.8F);
        CHECK(buffer->slice_repeat[0] >= 1);
        CHECK_EQ(buffer->filter_mode, WLR_SCALE_FILTER_NEAREST);
        double x = 0, y = 0;
        CHECK(!buffer->point_accepts_input(buffer, &x, &y));
      },
      &count
  );
  CHECK_EQ(count, 8);
  wlr_scene_node_destroy(&snapshot->node);
  CHECK(weak.expired());
  wlr_scene_node_destroy(&scene->tree.node);
}
int main() { return RUN_TESTS(); }
