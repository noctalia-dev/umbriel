// Blur regions requested by clients through ext_background_effect_v1. The region is surface-local, while the blur node
// keeps the whole surface box because its transparency mask stretches the surface buffer over that box; the request is
// therefore applied as a node clip, mapped by the same ratio the mask uses and rounded outward so a fractional
// animation scale never eats a blurred edge.
#include "check.h"
#include "scene/surface_blur.h"

// clang-format off
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace {

  pixman_region32_t rect(int x1, int y1, int x2, int y2) {
    pixman_region32_t region;
    pixman_region32_init_rect(&region, x1, y1, static_cast<unsigned>(x2 - x1), static_cast<unsigned>(y2 - y1));
    return region;
  }

  bool equals(const pixman_region32_t& actual, int x1, int y1, int x2, int y2) {
    pixman_region32_t expected = rect(x1, y1, x2, y2);
    const bool same = pixman_region32_equal(&actual, &expected) != 0;
    pixman_region32_fini(&expected);
    return same;
  }

  wlr_scene_blur* onlyBlurChild(wlr_scene_tree& tree) {
    CHECK(!wl_list_empty(&tree.children));
    if (wl_list_empty(&tree.children)) {
      return nullptr;
    }
    wlr_scene_node* child;
    child = wl_container_of(tree.children.next, child, link);
    CHECK_EQ(child->type, WLR_SCENE_NODE_BLUR);
    if (child->type != WLR_SCENE_NODE_BLUR) {
      return nullptr;
    }
    return wlr_scene_blur_from_node(child);
  }

  const pixman_region32_t* blurClip(wlr_scene_blur* blur) { return wlr_scene_node_get_clip_region(&blur->node); }

} // namespace

UMBRIEL_TEST(blurRegionMapsIdentityWhenNodeMatchesSurface) {
  pixman_region32_t source = rect(10, 20, 30, 40);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 100, 100}, 100, 100, &mapped);
  CHECK(equals(mapped, 10, 20, 30, 40));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

UMBRIEL_TEST(blurRegionFollowsAnimationScale) {
  pixman_region32_t source = rect(10, 20, 30, 40);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 100, 100}, 150, 150, &mapped);
  CHECK(equals(mapped, 15, 30, 45, 60));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

UMBRIEL_TEST(blurRegionRoundsOutwardForFractionalScales) {
  pixman_region32_t source = rect(1, 1, 2, 2);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 100, 100}, 133, 133, &mapped);
  // 1.33..2.66 covers pixels 1 and 2: floor on the near edge, ceil on the far edge.
  CHECK(equals(mapped, 1, 1, 3, 3));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

UMBRIEL_TEST(blurRegionClampsToNodeBox) {
  pixman_region32_t source = rect(-20, -20, 200, 200);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 100, 100}, 100, 100, &mapped);
  CHECK(equals(mapped, 0, 0, 100, 100));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

UMBRIEL_TEST(blurRegionIgnoresDegenerateSizes) {
  pixman_region32_t source = rect(0, 0, 10, 10);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 0, 100}, 100, 100, &mapped);
  CHECK(pixman_region32_empty(&mapped) != 0);
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

// The node stays the whole surface so the mask keeps its alignment; only the clip narrows.
// Client-side decorations put the surface origin at the geometry origin: the scene helper places the surface tree at
// the negative of it, so a surface-local request has to subtract it before the ratio applies.
UMBRIEL_TEST(blurRegionSubtractsSurfaceBoxOrigin) {
  pixman_region32_t source = rect(10, 10, 60, 110);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{10, 10, 100, 100}, 100, 100, &mapped);
  CHECK(equals(mapped, 0, 0, 50, 100));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

// wl_region coordinates are arbitrary int32; clamping before the conversion keeps a wild request inside the node.
UMBRIEL_TEST(blurRegionClampsOversizedRequests) {
  pixman_region32_t source;
  pixman_region32_init_rect(&source, -1000000, -1000000, 4000000, 4000000);
  pixman_region32_t mapped;
  pixman_region32_init(&mapped);
  umbriel::mapBlurRegionToNode(source, wlr_box{0, 0, 100, 100}, 150, 150, &mapped);
  CHECK(equals(mapped, 0, 0, 150, 150));
  pixman_region32_fini(&mapped);
  pixman_region32_fini(&source);
}

UMBRIEL_TEST(clientBlurRegionClipsTheWholeSurfaceNode) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::SurfaceBlur blur;
  const wlr_box box{0, 0, 200, 100};
  pixman_region32_t region = rect(0, 0, 100, 100);
  blur.update(
      &scene->tree, nullptr, box, box, corner_radii_none(), nullptr, {}, 1.0F, nullptr,
      umbriel::ClientBlurRegion{.region = &region, .source = box}
  );

  wlr_scene_blur* node = onlyBlurChild(scene->tree);
  CHECK(node != nullptr);
  if (node != nullptr) {
    CHECK(node->node.enabled);
    CHECK_EQ(node->width, 200);
    CHECK_EQ(node->height, 100);
    const pixman_region32_t* clip = blurClip(node);
    CHECK(clip != nullptr);
    if (clip != nullptr) {
      CHECK(equals(*clip, 0, 0, 100, 100));
    }
  }

  pixman_region32_fini(&region);
  wlr_scene_node_destroy(&scene->tree.node);
}

// A request replaces the rule's whole-surface blur; the rule can stay enabled.
UMBRIEL_TEST(clientBlurRegionRestrictsRuleBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::SurfaceBlur blur;
  const wlr_box box{0, 0, 200, 100};
  umbriel::SurfaceBlurOptions options;
  options.enabled = true;
  pixman_region32_t region = rect(50, 0, 100, 100);
  blur.update(
      &scene->tree, nullptr, box, box, corner_radii_none(), nullptr, options, 1.0F, nullptr,
      umbriel::ClientBlurRegion{.region = &region, .source = box}
  );

  wlr_scene_blur* node = onlyBlurChild(scene->tree);
  CHECK(node != nullptr);
  if (node != nullptr) {
    const pixman_region32_t* clip = blurClip(node);
    CHECK(clip != nullptr);
    if (clip != nullptr) {
      CHECK(equals(*clip, 50, 0, 100, 100));
    }
  }

  pixman_region32_fini(&region);
  wlr_scene_node_destroy(&scene->tree.node);
}

// An empty region is a client saying "no blur here", not a missing request.
UMBRIEL_TEST(emptyClientRegionSuppressesBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::SurfaceBlur blur;
  const wlr_box box{0, 0, 200, 100};
  umbriel::SurfaceBlurOptions options;
  options.enabled = true;
  pixman_region32_t region;
  pixman_region32_init(&region);
  blur.update(
      &scene->tree, nullptr, box, box, corner_radii_none(), nullptr, options, 1.0F, nullptr,
      umbriel::ClientBlurRegion{.region = &region, .source = box}
  );

  CHECK(wl_list_empty(&scene->tree.children));

  pixman_region32_fini(&region);
  wlr_scene_node_destroy(&scene->tree.node);
}

// No request and no rule leaves the surface alone, even though it looks transparent.
UMBRIEL_TEST(noRequestAndNoRuleSkipsBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::SurfaceBlur blur;
  const wlr_box box{0, 0, 200, 100};
  blur.update(&scene->tree, nullptr, box, box, corner_radii_none(), nullptr, {}, 0.5F, nullptr, {});

  CHECK(wl_list_empty(&scene->tree.children));

  wlr_scene_node_destroy(&scene->tree.node);
}

// Dropping the request turns the blur off again on the next update.
UMBRIEL_TEST(clearedClientRegionRemovesBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::SurfaceBlur blur;
  const wlr_box box{0, 0, 200, 100};
  pixman_region32_t region = rect(0, 0, 100, 100);
  blur.update(
      &scene->tree, nullptr, box, box, corner_radii_none(), nullptr, {}, 1.0F, nullptr,
      umbriel::ClientBlurRegion{.region = &region, .source = box}
  );
  wlr_scene_blur* node = onlyBlurChild(scene->tree);
  CHECK(node != nullptr);

  blur.update(&scene->tree, nullptr, box, box, corner_radii_none(), nullptr, {}, 0.5F, nullptr, {});
  CHECK(node != nullptr);
  if (node != nullptr) {
    CHECK(!node->node.enabled);
  }

  pixman_region32_fini(&region);
  wlr_scene_node_destroy(&scene->tree.node);
}

int main() { return RUN_TESTS(); }
