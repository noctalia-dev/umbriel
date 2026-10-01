#include "config/scene_effects.h"

#include "check.h"
#include "config/section.h"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace {
  using namespace umbriel::scene_experiment;

  class Fixture {
  public:
    Fixture() {
      char pattern[] = "/tmp/umbriel-scene-sources-XXXXXX";
      const char* created = mkdtemp(pattern);
      CHECK(created != nullptr);
      if (created == nullptr) {
        std::abort();
      }
      directory = created;
      std::filesystem::create_directory(directory / "included");
    }
    ~Fixture() { std::filesystem::remove_all(directory); }

    void write(std::string_view name, const std::string& content) const {
      std::ofstream stream(directory / "included" / name);
      stream << content;
      CHECK(stream.good());
    }

    SourceReadResult read(std::string_view text, Scope scope) {
      diagnostics.clear();
      const auto table = toml::parse(text, (directory / "included/preset.toml").string());
      umbriel::Section section(table, "experiment", diagnostics);
      return readSources(section, scope, diagnostics);
    }

    std::filesystem::path directory;
    std::vector<umbriel::ConfigDiagnostic> diagnostics;
  };
} // namespace

UMBRIEL_TEST(sceneScopeRejectsEveryWrongTrigger) {
  CHECK(parseScope("workspace_pair") == Scope::WorkspacePair);
  CHECK(!parseScope("workspace_set"));
  CHECK(!parseScope("window_scene"));
  CHECK(acceptsBinding(Scope::WorkspacePair, Binding::WorkspaceSwitch));
  CHECK(!acceptsBinding(Scope::WorkspacePair, Binding::Other));
}

UMBRIEL_TEST(sceneSourceReadIsAtomicAndKeepsAllRepairWatches) {
  Fixture fixture;
  fixture.write("fragment.glsl", "fragment one");
  constexpr std::string_view declaration = "common_shader = 'common.glsl'\nshader = 'fragment.glsl'";
  const auto broken = fixture.read(declaration, Scope::WorkspacePair);
  CHECK(!broken.sources);
  CHECK_EQ(broken.watchPaths.size(), 2U);
  fixture.write("common.glsl", "common one");
  const auto repaired = fixture.read(declaration, Scope::WorkspacePair);
  CHECK(repaired.sources.has_value());
  CHECK(fixture.diagnostics.empty());
  CHECK(repaired.watchPaths == broken.watchPaths);
  fixture.write("fragment.glsl", "fragment two");
  const auto edited = fixture.read(declaration, Scope::WorkspacePair);
  CHECK(edited.sources && edited.sources != repaired.sources);
  fixture.write("common.glsl", "   ");
  CHECK(!fixture.read(declaration, Scope::WorkspacePair).sources);
}

UMBRIEL_TEST(sceneSourcesEnforceRequiredStagesAndAggregateBudget) {
  Fixture fixture;
  fixture.write("source.glsl", "source");
  CHECK(fixture.read("shader = 'source.glsl'", Scope::WorkspacePair).sources.has_value());
  CHECK(!fixture.read("common_shader = 'source.glsl'", Scope::WorkspacePair).sources);
  CHECK(!fixture.read("shader = 'source.glsl'\nvertex_shader = 'source.glsl'", Scope::WorkspacePair).sources);
  CHECK(!fixture.read("shader = 'source.glsl'\ncomposite_shader = 'source.glsl'", Scope::WorkspacePair).sources);
  fixture.write("source.glsl", std::string(umbriel::kShaderSourceLimit, 'x'));
  CHECK(
      fixture.read("shader = 'source.glsl'\ncommon_shader = 'source.glsl'", Scope::WorkspacePair).sources.has_value()
  );
  fixture.write("source.glsl", std::string(umbriel::kShaderSourceLimit + 1, 'x'));
  CHECK(!fixture.read("shader = 'source.glsl'", Scope::WorkspacePair).sources);
}

UMBRIEL_TEST(sceneParametersRejectReservedDuplicateAndInvalidValues) {
  std::array parameters{Parameter{.name = "strength", .components = 1, .values = {0.5F, 0, 0, 0}}};
  CHECK(!validateParameters(parameters));
  for (std::string_view invalid :
       {"", "1name", "a.b", "umbriel_time", "gl_Position", "_fx_wrapper", "name__reserved", "float", "uniform", "main",
        "transition", "transition_vertex"}) {
    parameters[0].name = invalid;
    CHECK(validateParameters(parameters).has_value());
  }
  parameters[0].name = std::string("strength\0suffix", 15);
  CHECK(validateParameters(parameters).has_value());
  parameters[0].name = "strength";
  parameters[0].values[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(validateParameters(parameters).has_value());
  parameters[0].values[0] = 0;
  parameters[0].values[1] = 1;
  CHECK(validateParameters(parameters).has_value());
  parameters[0].values[1] = 0;
  parameters[0].components = 5;
  CHECK(validateParameters(parameters).has_value());
  parameters[0].components = 1;
  const std::array duplicates{parameters[0], parameters[0]};
  CHECK(validateParameters(duplicates).has_value());
  const std::vector<Parameter> excess(kParameterLimit + 1);
  CHECK(validateParameters(excess).has_value());
  parameters[0].name = std::string(kParameterNameLimit, 'a');
  CHECK(!validateParameters(parameters));
  parameters[0].name += 'a';
  CHECK(validateParameters(parameters).has_value());
}

UMBRIEL_TEST(sceneParameterTablesAreAtomicAndTyped) {
  std::vector<umbriel::ConfigDiagnostic> diagnostics;
  const auto read = [&](std::string_view text) {
    diagnostics.clear();
    const auto table = toml::parse(text);
    umbriel::Section section(table, "experiment", diagnostics);
    return readParameters(section);
  };
  const auto empty = read("");
  CHECK(empty && empty->empty());
  const auto values = read("[parameters]\nscalar = 2\nvector = [1.5, -2, 0, 4]\nsingle = [0.25]");
  CHECK(values && values->size() == 3);
  CHECK(diagnostics.empty());
  if (values && values->size() == 3) {
    CHECK((*values)[0] == (Parameter{.name = "scalar", .components = 1, .values = {2, 0, 0, 0}}));
    CHECK((*values)[1] == (Parameter{.name = "single", .components = 1, .values = {0.25F, 0, 0, 0}}));
    CHECK((*values)[2] == (Parameter{.name = "vector", .components = 4, .values = {1.5F, -2, 0, 4}}));
  }
  for (std::string_view invalid :
       {"true", "'1'", "[]", "[1,2,3,4,5]", "[1,true]", "[[1]]", "{nested=1}", "inf", "nan", "1e100"}) {
    CHECK(!read("[parameters]\nvalid = 1\nbroken = " + std::string(invalid)));
    CHECK(!diagnostics.empty());
  }
  CHECK(!read("parameters = 1"));
  CHECK(!read("[parameters]\ntransition = 1"));
  std::string excess = "[parameters]\n";
  for (size_t i = 0; i <= kParameterLimit; ++i) {
    excess += "p" + std::to_string(i) + " = 0\n";
  }
  CHECK(!read(excess));
}

int main() { return RUN_TESTS(); }
