#include "check.h"
#include "config/config.h"
#include "view/decoration.h"

// clang-format off
#include "wlr.h"
// clang-format on

namespace {

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

} // namespace

// Window-rule opacity belongs to the client surface composition. The blur
// behind it stays fully sampled, matching a client that submits the same alpha.
UMBRIEL_TEST(windowRuleOpacityDoesNotAttenuateBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::ResolvedWindowRule rule;
  rule.blur = true;
  umbriel::ViewDecoration decoration;
  decoration.applyRule(rule);

  const wlr_box box{0, 0, 100, 100};
  decoration.updateBlur(&scene->tree, nullptr, box, box, 0, nullptr, 0.8F, 1.0F);

  wlr_scene_blur* blur = onlyBlurChild(scene->tree);
  CHECK(blur != nullptr);
  if (blur != nullptr) {
    CHECK_EQ(blur->alpha, 1.0F);
  }

  wlr_scene_node_destroy(&scene->tree.node);
}

// Transition opacity still fades the effect itself, otherwise an invisible
// opening or closing window would leave a standalone blur rectangle.
UMBRIEL_TEST(transitionOpacityStillAttenuatesBlur) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }

  umbriel::ResolvedWindowRule rule;
  rule.blur = true;
  umbriel::ViewDecoration decoration;
  decoration.applyRule(rule);

  const wlr_box box{0, 0, 100, 100};
  decoration.updateBlur(&scene->tree, nullptr, box, box, 0, nullptr, 0.4F, 0.5F);

  wlr_scene_blur* blur = onlyBlurChild(scene->tree);
  CHECK(blur != nullptr);
  if (blur != nullptr) {
    CHECK_EQ(blur->alpha, 0.5F);
  }

  wlr_scene_node_destroy(&scene->tree.node);
}

int main() { return RUN_TESTS(); }
