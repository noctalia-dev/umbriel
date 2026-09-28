#include "check.h"
#include "scene/node.h"

#include <cstdint>

using umbriel::SceneNode;
using umbriel::sceneNodeFrom;
using umbriel::SceneNodeKind;

namespace {

  // Stands in for LayerSurface and LockSurface: SceneNode is the only base.
  struct TaggedNode : SceneNode {
    explicit TaggedNode(SceneNodeKind kind) : SceneNode(kind) {}
    int payload = 42;
  };

  // Stands in for View, which also inherits a polymorphic base. That base takes offset 0 for its vptr, so the SceneNode
  // subobject does NOT start at the address of the object.
  struct PolymorphicBase {
    virtual ~PolymorphicBase() = default;
    virtual void anything() = 0;
  };
  struct TaggedPolymorphicNode : SceneNode, PolymorphicBase {
    explicit TaggedPolymorphicNode(SceneNodeKind kind) : SceneNode(kind) {}
    void anything() override {}
    int payload = 7;
  };

  // Stands in for user data some other library stashed in the same field.
  struct ForeignData {
    uint32_t somethingElse = 0xDEADBEEF;
    void* pointer = nullptr;
  };

} // namespace

UMBRIEL_TEST(nullDataYieldsNull) { CHECK(sceneNodeFrom(nullptr) == nullptr); }

UMBRIEL_TEST(recoversATaggedNode) {
  SceneNode node(SceneNodeKind::View);
  SceneNode* recovered = sceneNodeFrom(&node);
  CHECK(recovered == &node);
  CHECK(recovered != nullptr && recovered->kind == SceneNodeKind::View);
}

UMBRIEL_TEST(recoversThroughADerivedPointer) {
  // The real call sites store a derived `this` and read it back as SceneNode*.
  TaggedNode layer(SceneNodeKind::LayerSurface);
  void* stored = &layer;

  SceneNode* recovered = sceneNodeFrom(stored);
  CHECK(recovered != nullptr);
  CHECK(recovered != nullptr && recovered->kind == SceneNodeKind::LayerSurface);
  CHECK(static_cast<TaggedNode*>(recovered) == &layer);
  CHECK(static_cast<TaggedNode*>(recovered)->payload == 42);
}

UMBRIEL_TEST(everyKindRoundTrips) {
  for (SceneNodeKind kind : {SceneNodeKind::View, SceneNodeKind::LayerSurface, SceneNodeKind::LockSurface}) {
    TaggedNode node(kind);
    SceneNode* recovered = sceneNodeFrom(&node);
    CHECK(recovered != nullptr);
    CHECK(recovered != nullptr && recovered->kind == kind);
  }
}

UMBRIEL_TEST(foreignDataYieldsNullInsteadOfBeingReinterpreted) {
  // A non-SceneNode pointer must be rejected rather than cast to SceneNode*
  // and its `kind` read regardless of what it points at.
  ForeignData foreign;
  CHECK(sceneNodeFrom(&foreign) == nullptr);
}

UMBRIEL_TEST(magicIsTheFirstMemberSoTheGuardReadsIt) {
  // sceneNodeFrom reads `magic` through a possibly-foreign pointer, so it must
  // sit at offset 0 for the check to be meaningful.
  SceneNode node(SceneNodeKind::View);
  CHECK_EQ(reinterpret_cast<const char*>(&node.magic), reinterpret_cast<const char*>(&node));
  CHECK_EQ(node.magic, SceneNode::kMagic);
}

UMBRIEL_TEST(sceneNodeDataRoundTripsThroughAPolymorphicDerivedClass) {
  // When the SceneNode subobject is not at offset 0 (a polymorphic base precedes it), storing a raw `this` would make
  // sceneNodeFrom read the vptr as `magic` and reject the node, breaking pointer hit-testing.
  TaggedPolymorphicNode node(SceneNodeKind::View);

  // The premise: the object does not begin with its SceneNode subobject.
  CHECK(static_cast<void*>(&node) != static_cast<void*>(static_cast<SceneNode*>(&node)));

  void* stored = umbriel::sceneNodeData(&node);
  SceneNode* recovered = sceneNodeFrom(stored);
  CHECK(recovered != nullptr);
  CHECK(recovered != nullptr && recovered->kind == SceneNodeKind::View);
  CHECK(static_cast<TaggedPolymorphicNode*>(recovered) == &node);
  CHECK(static_cast<TaggedPolymorphicNode*>(recovered)->payload == 7);
}

UMBRIEL_TEST(storingTheRawDerivedPointerIsRejected) {
  // Documents why sceneNodeData exists: bypassing it does not round-trip, and
  // the sentinel turns that into a clean null rather than a misread object.
  TaggedPolymorphicNode node(SceneNodeKind::View);
  void* raw = static_cast<void*>(&node);
  CHECK(sceneNodeFrom(raw) == nullptr);
}

UMBRIEL_TEST(sceneNodeDataIsIdentityForASingleBase) {
  TaggedNode node(SceneNodeKind::LayerSurface);
  CHECK(umbriel::sceneNodeData(&node) == static_cast<void*>(&node));
  CHECK(sceneNodeFrom(umbriel::sceneNodeData(&node)) != nullptr);
}

int main() { return RUN_TESTS(); }
