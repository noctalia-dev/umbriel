#include "scene/scene_program.h"

#include "check.h"

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
}

// Replace only the compiler boundary: these tests exercise registry cache and
// ownership, while UmbrielFX's scene-program tests execute the actual GPU stages.
struct fx_scene_program {
  bool time;
};

namespace {
  using namespace umbriel::scene_experiment;
  unsigned compiles = 0;
  unsigned live = 0;
  bool fail = false;
  bool readsTime = false;
  fx_scene_limits limits{8192, 256, 256, 8};
  std::array<std::string, 2> lastSources;
  std::vector<fx_scene_parameter> lastParameters;
  int rendererStorage[2]{};

  wlr_renderer* renderer(unsigned index = 0) { return reinterpret_cast<wlr_renderer*>(&rendererStorage[index]); }

  ProgramDefinition definition() {
    ProgramDefinition result;
    result.name = "water";
    result.sources.scope = Scope::WorkspacePair;
    for (std::size_t i = 0; i < result.sources.stages.size(); ++i) {
      result.sources.stages[i] = umbriel::ShaderSource{.code = "stage " + std::to_string(i), .file = {}};
    }
    result.parameters.push_back({.name = "amplitude", .components = 2, .values = {0.25F, 0.5F, 0, 0}});
    result.palette = true;
    return result;
  }

  struct Fixture {
    Fixture() {
      CHECK_EQ(live, 0U);
      compiles = 0;
      fail = false;
      readsTime = false;
      limits = {8192, 256, 256, 8};
    }
    void prepare(unsigned rendererIndex = 0) { cache.prepare(renderer(rendererIndex), definitions, roots); }
    std::array<ProgramDefinition, 1> definitions{definition()};
    std::array<std::string, 1> roots{"water"};
    ScenePrograms cache;
  };
} // namespace

extern "C" {
bool __wrap_fx_scene_program_get_limits(wlr_renderer*, fx_scene_limits* result) {
  *result = limits;
  return true;
}

fx_scene_program* __wrap_fx_scene_program_create(
    wlr_renderer*, const fx_scene_sources* sources, const fx_scene_parameter* parameters, unsigned count
) {
  ++compiles;
  lastSources = {sources->common ? sources->common : "", sources->fragment ? sources->fragment : ""};
  lastParameters.assign(parameters, parameters + count);
  if (fail) {
    return nullptr;
  }
  ++live;
  return new fx_scene_program{readsTime};
}

void __wrap_fx_scene_program_unref(fx_scene_program* program) {
  --live;
  delete program;
}

bool __wrap_fx_scene_program_reads_role(const fx_scene_program*) { return false; }
bool __wrap_fx_scene_program_reads_time(const fx_scene_program* program) { return program->time; }
}

UMBRIEL_TEST(sceneProgramsOnlyPrepareReferencedDefinitionsAndInspectionIsPure) {
  Fixture fixture;
  fixture.cache.prepare(renderer(), fixture.definitions, {});
  CHECK_EQ(fixture.cache.size(), 0U);
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Unreferenced);
  CHECK(!fixture.cache.find("water"));
  CHECK_EQ(compiles, 0U);
  fixture.prepare();
  const auto bundle = fixture.cache.find("water");
  CHECK(bundle != nullptr);
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Ready);
  fixture.prepare();
  CHECK(fixture.cache.find("water") == bundle);
  CHECK_EQ(compiles, 1U);
  for (std::size_t i = 0; i < lastSources.size(); ++i) {
    CHECK_EQ(lastSources[i], "stage " + std::to_string(i));
  }
  CHECK_EQ(lastParameters.size(), 1U);
  CHECK_EQ(std::string(lastParameters[0].name), "amplitude");
  CHECK_EQ(lastParameters[0].components, 2U);
  CHECK_EQ(lastParameters[0].value[1], 0.5F);
  fixture.cache.prepare(renderer(), {}, fixture.roots);
  CHECK_EQ(fixture.cache.size(), 0U);
  CHECK_EQ(live, 1U); // The in-flight bundle alone now owns this program.
}

UMBRIEL_TEST(sceneProgramsCacheFailedReplacementsAndRetainWholeInflightVersion) {
  Fixture fixture;
  fixture.prepare();
  auto retained = fixture.cache.find("water");
  const auto original = fixture.definitions[0];
  fail = true;
  fixture.definitions[0].sources.stages[1]->code = "broken replacement fragment";
  fixture.definitions[0].parameters[0].values[0] = 0.75F;
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::CompileFailed);
  CHECK(!fixture.cache.find("water"));
  CHECK(retained->definition == original);
  CHECK_EQ(live, 1U);
  fixture.prepare();
  CHECK_EQ(compiles, 2U);
  fail = false;
  fixture.definitions[0].sources.stages[1]->code = "repaired fragment";
  fixture.prepare();
  const auto repaired = fixture.cache.find("water");
  CHECK(repaired != nullptr);
  CHECK(repaired->definition == fixture.definitions[0]);
  CHECK_EQ(compiles, 3U);
  CHECK_EQ(live, 2U);
  retained.reset();
  CHECK_EQ(live, 1U);
}

UMBRIEL_TEST(sceneProgramsEachStageAndParameterChangeReplacesAtomically) {
  Fixture fixture;
  fixture.prepare();
  unsigned expected = 1;
  for (auto& stage : fixture.definitions[0].sources.stages) {
    const auto old = fixture.cache.find("water");
    stage->code += " edited";
    fixture.prepare();
    CHECK(fixture.cache.find("water") != old);
    CHECK_EQ(compiles, ++expected);
    CHECK_EQ(live, 2U);
  }
  fixture.definitions[0].parameters[0].values[1] = 1.0F;
  fixture.prepare();
  CHECK_EQ(compiles, ++expected);
  CHECK_EQ(fixture.cache.find("water")->definition.parameters[0].values[1], 1.0F);
}

UMBRIEL_TEST(sceneProgramsRendererReplacementAndRemovalReleaseCacheOwnership) {
  Fixture fixture;
  fixture.prepare();
  auto retained = fixture.cache.find("water");
  fixture.prepare(1);
  CHECK(fixture.cache.find("water") != retained);
  CHECK_EQ(compiles, 2U);
  CHECK_EQ(live, 2U);
  fixture.cache.prepare(nullptr, fixture.definitions, fixture.roots);
  CHECK_EQ(fixture.cache.size(), 0U);
  CHECK_EQ(live, 1U);
  retained.reset();
  CHECK_EQ(live, 0U);
  fixture.prepare(1);
  CHECK_EQ(compiles, 3U);
}

UMBRIEL_TEST(sceneProgramsRejectLimitsAndInvalidBundlesBeforeCompiler) {
  Fixture fixture;
  limits.vertex_vectors = FX_SCENE_VERTEX_VECTORS - 1;
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Unsupported);
  CHECK_EQ(compiles, 0U);
  fixture.cache.clear();
  limits.vertex_vectors = 256;
  limits.fragment_vectors = FX_SCENE_FRAGMENT_VECTORS;
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Unsupported);
  CHECK_EQ(compiles, 0U);
  fixture.cache.clear();
  limits.fragment_vectors = 256;
  limits.fragment_texture_units = 1;
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Unsupported);
  fixture.cache.clear();
  limits.fragment_texture_units = 8;
  fixture.definitions[0].sources.stages[1].reset();
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Invalid);
  fixture.definitions[0] = definition();
  fixture.definitions[0].parameters[0].name = "umbriel_reserved";
  fixture.prepare();
  CHECK_EQ(fixture.cache.state("water"), ProgramState::Invalid);
  CHECK_EQ(compiles, 0U);
}

int main() { return RUN_TESTS(); }

UMBRIEL_TEST(sceneProgramsRetainReflectedTimeUsageWithTheirBundleVersion) {
  Fixture fixture;
  readsTime = true;
  fixture.prepare();
  const auto original = fixture.cache.find("water");
  CHECK(original->readsTime);
  readsTime = false;
  fixture.definitions[0].sources.stages[1]->code = "no time fragment";
  fixture.prepare();
  CHECK(!fixture.cache.find("water")->readsTime);
  CHECK(original->readsTime);
}
