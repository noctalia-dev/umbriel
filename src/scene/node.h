#pragma once

#include <cstdint>

namespace umbriel {

  enum class SceneNodeKind : uint8_t {
    View,
    LayerSurface,
    LockSurface,
  };

  // Stored in wlr_scene_node::data so hit-testing can tell views from layers
  // (including xdg popups parented to a layer surface).
  struct SceneNode {
    // Distinguishes our tagged nodes from any other user data that ends up in
    // wlr_scene_node::data. "UMBS".
    static constexpr uint32_t kMagic = 0x554D4253;

    explicit SceneNode(SceneNodeKind kind) : kind(kind) {}

    uint32_t magic = kMagic;
    SceneNodeKind kind;
  };

  // Produces the pointer to store in wlr_scene_node::data; always store through this, never a raw `this`. A derived
  // class with a polymorphic base does not start with its SceneNode subobject, so the implicit conversion here applies
  // the offset that sceneNodeFrom relies on.
  [[nodiscard]] inline void* sceneNodeData(SceneNode* node) { return node; }

  // Recovers a SceneNode from a wlr_scene_node::data pointer, or null when the pointer is something else, such as user
  // data a wlroots or SceneFX helper stored on a node we walk over. It is a guard, not a proof: reading `magic` through
  // a foreign pointer is only safe because anything in that field points at a live object.
  [[nodiscard]] inline SceneNode* sceneNodeFrom(void* data) {
    auto* node = static_cast<SceneNode*>(data);
    if (node == nullptr || node->magic != SceneNode::kMagic) {
      return nullptr;
    }
    return node;
  }

} // namespace umbriel
