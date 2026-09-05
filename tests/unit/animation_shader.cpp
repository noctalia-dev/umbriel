#include "config/animation_shader.h"

#include "check.h"
#include "config/section.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  class Fixture {
  public:
    Fixture() {
      char pattern[] = "/tmp/umbriel-shader-source-XXXXXX";
      const char* created = mkdtemp(pattern);
      CHECK(created != nullptr);
      if (created == nullptr) {
        std::abort();
      }
      directory = created;
      std::filesystem::create_directory(directory / "theme");
    }
    ~Fixture() { std::filesystem::remove_all(directory); }

    void write(const std::filesystem::path& relative, const std::string& contents) const {
      std::ofstream stream(directory / relative, std::ios::binary);
      stream << contents;
      CHECK(stream.good());
    }

    umbriel::AnimationShaderReadResult read(const std::string& text, std::string_view relative = "theme/config.toml") {
      diagnostics.clear();
      const auto table = toml::parse(text, (directory / relative).string());
      umbriel::Section section(table, "animation.windows_in", diagnostics);
      return umbriel::readAnimationShader(section, diagnostics);
    }

    bool warned(std::string_view text) const {
      return std::ranges::any_of(diagnostics, [&](const auto& diagnostic) {
        return diagnostic.message.contains(text);
      });
    }

    std::filesystem::path directory;
    std::vector<umbriel::ConfigDiagnostic> diagnostics;
  };
} // namespace

UMBRIEL_TEST(shaderIsOptionalAndRemovedKeysAreUnknown) {
  Fixture fixture;
  const auto empty = fixture.read("");
  CHECK(!empty.source);
  CHECK(empty.watchPaths.empty());
  CHECK(fixture.diagnostics.empty());

  for (const std::string key : {"custom_shader", "custom_shader_file", "shader_file", "shader_inline"}) {
    const auto result = fixture.read(key + " = 'effect.glsl'");
    CHECK(!result.source);
    CHECK(result.watchPaths.empty());
    CHECK(fixture.warned("unknown key"));
  }
}

UMBRIEL_TEST(shaderFileUsesDeclaringConfigDirectoryAndReloadsItsContents) {
  Fixture fixture;
  fixture.write("theme/effect.glsl", "first shader\n");
  fixture.write("effect.glsl", "wrong directory\n");
  const auto first = fixture.read("shader = 'effect.glsl'");
  CHECK(first.source.has_value());
  if (first.source) {
    CHECK_EQ(first.source->code, std::string("first shader\n"));
    CHECK(first.source->file == fixture.directory / "theme/effect.glsl");
  }
  CHECK(first.watchPaths == std::vector<std::filesystem::path>{fixture.directory / "theme/effect.glsl"});
  CHECK(fixture.diagnostics.empty());

  fixture.write("theme/effect.glsl", "second shader\n");
  const auto second = fixture.read("shader = 'effect.glsl'");
  CHECK(second.source.has_value());
  if (second.source) {
    CHECK_EQ(second.source->code, std::string("second shader\n"));
    CHECK(second.source != first.source);
  }

  const auto absolute = fixture.read("shader = '" + (fixture.directory / "theme/effect.glsl").string() + "'");
  CHECK(absolute.source == second.source);
}

UMBRIEL_TEST(shaderMissingFileRemainsAWatchDependencyAndCanAppearLater) {
  Fixture fixture;
  const auto missing = fixture.read("shader = 'new.glsl'");
  CHECK(!missing.source);
  CHECK(missing.watchPaths == std::vector<std::filesystem::path>{fixture.directory / "theme/new.glsl"});
  CHECK(fixture.warned("cannot read shader file"));
  CHECK_EQ(fixture.diagnostics.front().file, (fixture.directory / "theme/config.toml").string());
  CHECK_EQ(fixture.diagnostics.front().line, 1U);

  fixture.write("theme/new.glsl", "new shader");
  const auto created = fixture.read("shader = 'new.glsl'");
  CHECK(created.source.has_value());
  if (created.source) {
    CHECK_EQ(created.source->code, std::string("new shader"));
  }
  CHECK(created.watchPaths == missing.watchPaths);
  CHECK(fixture.diagnostics.empty());
}

UMBRIEL_TEST(shaderRequiresAPathNotInlineSource) {
  Fixture fixture;
  CHECK(!fixture.read("shader = false").source);
  CHECK(fixture.warned("must be a string"));
  CHECK(!fixture.warned("unknown key"));
  CHECK(!fixture.read("shader = 123").source);
  CHECK(fixture.warned("must be a string"));
  CHECK(!fixture.read("shader = 'vec4 animation(vec2 uv) { return vec4(1.0); }'").source);
  CHECK(fixture.warned("cannot read shader file"));
}

UMBRIEL_TEST(shaderInputIsBoundedAndRejectsBlankNulAndNonRegularFiles) {
  Fixture fixture;
  fixture.write("theme/blank.glsl", "   ");
  CHECK(!fixture.read("shader = 'blank.glsl'").source);
  CHECK(fixture.warned("must not be blank"));
  CHECK(!fixture.read("shader = ''").source);
  CHECK(fixture.warned("must not be empty"));

  fixture.write("theme/effect.glsl", std::string("abc\0def", 7));
  CHECK(!fixture.read("shader = 'effect.glsl'").source);
  CHECK(fixture.warned("NUL"));

  fixture.write("theme/effect.glsl", std::string(umbriel::kAnimationShaderSourceLimit, 'x'));
  CHECK(fixture.read("shader = 'effect.glsl'").source.has_value());
  fixture.write("theme/effect.glsl", std::string(umbriel::kAnimationShaderSourceLimit + 1, 'x'));
  CHECK(!fixture.read("shader = 'effect.glsl'").source);
  CHECK(fixture.warned("exceeds 256 KiB"));

  CHECK_EQ(mkfifo((fixture.directory / "theme/pipe.glsl").c_str(), 0600), 0);
  CHECK(!fixture.read("shader = 'pipe.glsl'").source);
  CHECK(fixture.warned("regular file"));
  CHECK(!fixture.read("shader = '.'").source);
  CHECK(fixture.warned("regular file"));
}

int main() { return RUN_TESTS(); }
