#include "check.h"
#include "scene/composition.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"

#include <umbrielfx/render/effect.h>
}

// Exercise ownership and final-submit arbitration independently of the GPU
// pixel contracts in UmbrielFX's scene-pair tests.
struct fx_scene_target {
  bool prepared = false;
  wlr_buffer* buffer = nullptr;
};

namespace {
  unsigned buffers = 0;
  unsigned targets = 0;
  unsigned textures = 0;
  unsigned renders = 0;
  unsigned allocations = 0;
  unsigned failAllocation = 0;
  int failRole = -1;
  std::array<fx_scene_frame, 2> rendered{};
  std::unordered_map<wlr_texture*, wlr_buffer*> textureSources;

  struct Fixture {
    wlr_renderer renderer{};
    wlr_allocator allocator{};
    fx_scene_resource_pool output{FX_SCENE_OUTPUT_BUDGET, 0};
    fx_scene_resource_pool aggregate{FX_SCENE_TOTAL_BUDGET, 0};
    std::shared_ptr<umbriel::scene_experiment::ProgramBundle> bundle =
        std::make_shared<umbriel::scene_experiment::ProgramBundle>();
    Fixture() {
      CHECK_EQ(buffers, 0U);
      CHECK_EQ(targets, 0U);
      CHECK_EQ(textures, 0U);
      CHECK(textureSources.empty());
      allocations = renders = 0;
      failAllocation = 0;
      failRole = -1;
      bundle->readsRole = true;
      wl_signal_init(&renderer.events.destroy);
      bundle->program = std::shared_ptr<fx_scene_program>(reinterpret_cast<fx_scene_program*>(this), [](auto*) {});
    }
    ~Fixture() {
      CHECK_EQ(buffers, 0U);
      CHECK_EQ(targets, 0U);
      CHECK_EQ(textures, 0U);
      CHECK(textureSources.empty());
      CHECK_EQ(output.used, 0U);
      CHECK_EQ(aggregate.used, 0U);
    }
    std::unique_ptr<umbriel::SceneComposition> create() {
      return umbriel::SceneComposition::create(&renderer, &allocator, output, aggregate, bundle, 64, 48, false);
    }
  };
} // namespace

extern "C" {
bool __wrap_fx_scene_program_get_limits(wlr_renderer*, fx_scene_limits* limits) {
  *limits = {8192, 256, 256, 8};
  return true;
}
wlr_buffer* __wrap_fx_scene_buffer_create(wlr_renderer*, wlr_allocator*, int width, int height, bool) {
  if (++allocations == failAllocation) {
    return nullptr;
  }
  auto* buffer = new wlr_buffer{};
  buffer->width = width;
  buffer->height = height;
  ++buffers;
  return buffer;
}
void __wrap_wlr_buffer_drop(wlr_buffer* buffer) {
  if (buffer != nullptr) {
    --buffers;
    delete buffer;
  }
}
fx_scene_target* __wrap_fx_scene_target_create_with_color(wlr_renderer*, wlr_buffer* buffer, bool) {
  ++targets;
  return new fx_scene_target{.buffer = buffer};
}
void __wrap_fx_scene_target_destroy(fx_scene_target* target) {
  if (target != nullptr) {
    --targets;
    delete target;
  }
}
wlr_texture* __wrap_wlr_texture_from_buffer(wlr_renderer*, wlr_buffer* buffer) {
  ++textures;
  auto* texture = new wlr_texture{};
  textureSources.emplace(texture, buffer);
  return texture;
}
void __wrap_wlr_texture_destroy(wlr_texture* texture) {
  CHECK_EQ(textureSources.erase(texture), 1U);
  --textures;
  delete texture;
}
bool __wrap_fx_scene_program_render(
    fx_scene_program*, fx_scene_target*, const fx_scene_frame* frame, const fx_scene_input*
) {
  ++renders;
  rendered[static_cast<unsigned>(frame->role)] = *frame;
  return frame->role != failRole;
}
}

UMBRIEL_TEST(sceneCompositionRetriesCompletedPairWithoutRelatching) {
  Fixture fixture;
  auto composition = fixture.create();
  CHECK(composition != nullptr);
  CHECK_EQ(buffers, 4U);
  CHECK_EQ(targets, 4U);
  CHECK_EQ(fixture.output.used, composition->reservedBytes());
  CHECK_EQ(fixture.aggregate.used, composition->reservedBytes());
  wlr_buffer sourceA{}, sourceB{};
  const std::array<umbriel::SceneComposition::Source, 2> sources{{{&sourceA, &sourceA}, {&sourceB, &sourceB}}};
  fx_scene_frame frame{};
  frame.progress = 0.25F;
  CHECK(composition->render(frame, sources));
  CHECK_EQ(renders, 2U);
  CHECK_EQ(textures, 0U);
  const auto pending = composition->candidate();
  CHECK(pending.display != nullptr);
  CHECK(pending.display != pending.unfiltered);
  CHECK(composition->committed().display == nullptr);
  composition->submitted(false);
  frame.progress = 0.75F;
  CHECK(composition->render(frame, sources));
  CHECK_EQ(renders, 2U);
  CHECK_EQ(rendered[0].progress, 0.25F);
  CHECK(composition->candidate().display == pending.display);
  composition->submitted(true);
  CHECK(!composition->pending());
  CHECK(composition->committed().display == pending.display);
  CHECK(composition->render(frame, sources));
  CHECK_EQ(renders, 4U);
  CHECK(composition->candidate().display != pending.display);
  CHECK(composition->committed().display == pending.display);
}

UMBRIEL_TEST(sceneCompositionRoleFailureNeverPublishesPartialPair) {
  Fixture fixture;
  auto composition = fixture.create();
  CHECK(composition != nullptr);
  wlr_buffer source{};
  const std::array<umbriel::SceneComposition::Source, 2> sources{{{&source, &source}, {&source, &source}}};
  fx_scene_frame frame{};
  CHECK(composition->render(frame, sources));
  composition->submitted(true);
  const auto committed = composition->committed();
  failRole = 1;
  CHECK(!composition->render(frame, sources));
  CHECK(!composition->pending());
  CHECK(composition->candidate().display == nullptr);
  CHECK(composition->committed().display == committed.display);
  CHECK_EQ(textures, 0U);
  failRole = -1;
  CHECK(composition->render(frame, sources));
  wl_signal_emit_mutable(&fixture.renderer.events.destroy, &fixture.renderer);
  CHECK(!composition->render(frame, sources));
  CHECK(composition->candidate().display == nullptr);
  CHECK(composition->committed().display == nullptr);
}

UMBRIEL_TEST(sceneCompositionReservationAndAllocationFailAtomically) {
  Fixture fixture;
  fixture.output.limit = 1;
  CHECK(fixture.create() == nullptr);
  CHECK_EQ(allocations, 0U);
  CHECK_EQ(fixture.aggregate.used, 0U);
  fixture.output.limit = FX_SCENE_OUTPUT_BUDGET;
  for (unsigned index = 1; index <= 4; ++index) {
    allocations = 0;
    failAllocation = index;
    CHECK(fixture.create() == nullptr);
    CHECK_EQ(buffers, 0U);
    CHECK_EQ(targets, 0U);
    CHECK_EQ(fixture.output.used, 0U);
    CHECK_EQ(fixture.aggregate.used, 0U);
  }
}

UMBRIEL_TEST(sceneCompositionSharesIdenticalRolesAndSplitsWhenInputsChange) {
  Fixture fixture;
  fixture.bundle->readsRole = false;
  auto composition = fixture.create();
  CHECK(composition != nullptr);
  CHECK_EQ(buffers, 2U);
  wlr_buffer sourceA{}, sourceB{}, unfiltered{};
  std::array<umbriel::SceneComposition::Source, 2> sources{{{&sourceA, &sourceA}, {&sourceB, &sourceB}}};
  fx_scene_frame frame{};
  CHECK(composition->render(frame, sources));
  CHECK_EQ(renders, 1U);
  CHECK(composition->candidate().display == composition->candidate().unfiltered);
  composition->submitted(true);
  const auto committed = composition->committed();
  sources[1].unfiltered = &unfiltered;
  CHECK(composition->render(frame, sources));
  CHECK_EQ(buffers, 4U);
  CHECK_EQ(renders, 3U);
  CHECK(composition->candidate().display != composition->candidate().unfiltered);
  composition->submitted(false);
  CHECK(composition->committed().display == committed.display);
  CHECK_EQ(fixture.output.used, composition->reservedBytes());
}

UMBRIEL_TEST(sceneCompositionRoleSplitFailurePreservesCommittedImageAndBudget) {
  for (unsigned index = 1; index <= 2; ++index) {
    Fixture fixture;
    fixture.bundle->readsRole = false;
    auto composition = fixture.create();
    wlr_buffer source{}, unfiltered{};
    std::array<umbriel::SceneComposition::Source, 2> sources{{{&source, &source}, {&source, &source}}};
    fx_scene_frame frame{};
    CHECK(composition->render(frame, sources));
    composition->submitted(true);
    const auto committed = composition->committed();
    const auto budget = fixture.output.used;
    sources[1].unfiltered = &unfiltered;
    failAllocation = allocations + index;
    CHECK(!composition->render(frame, sources));
    CHECK(composition->committed().display == committed.display);
    CHECK_EQ(buffers, 2U);
    CHECK_EQ(fixture.output.used, budget);
    failAllocation = 0;
    CHECK(composition->render(frame, sources));
    CHECK_EQ(buffers, 4U);
  }
}

int main() { return RUN_TESTS(); }
