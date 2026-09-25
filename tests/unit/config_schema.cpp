// `umbriel config schema` runs the real config loader under a recorder. These invariants hold it to the loader without
// asking anything of a contributor who adds an ordinary key: they only fail when the schema would say something the
// loader does not do.

#include "check.h"
#include "config/config.h"
#include "config/schema.h"
#include "config/store.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>

using umbriel::Config;
using umbriel::ConfigSchema;
using umbriel::SchemaType;

namespace {

  // A run that does not settle within kSchemaMaxRounds, or cannot load its skeleton, fails every test here.
  const ConfigSchema& schema() {
    static const ConfigSchema built = [] {
      auto result = umbriel::buildConfigSchema();
      if (!result) {
        std::println(stderr, "  {}", result.error());
        std::abort();
      }
      return std::move(*result);
    }();
    return built;
  }

  struct Loaded {
    Config config;
    std::vector<std::string> messages;
  };

  Loaded load(const toml::table& document) {
    CHECK(umbriel::loadConfigDocument(document));
    Loaded loaded{.config = umbriel::config(), .messages = {}};
    for (const auto& diagnostic : umbriel::configDiagnostics()) {
      loaded.messages.push_back(diagnostic.message);
    }
    return loaded;
  }

  bool familyPath(std::string_view path) { return path.contains("[]") || path.contains("<name>"); }

  // The skeleton's tables without its array and map entries: every section present, nothing set, no rule or output.
  toml::table sectionsOnly() {
    toml::table document = schema().skeleton;
    for (const auto& [path, shape] : schema().shapes) {
      if (shape.type == SchemaType::Table || familyPath(path)) {
        continue;
      }
      const size_t dot = path.rfind('.');
      toml::node* parent = dot == std::string::npos ? &document : document.at_path(path.substr(0, dot)).node();
      if (parent != nullptr && parent->is_table()) {
        parent->as_table()->erase(dot == std::string::npos ? path : path.substr(dot + 1));
      }
    }
    return document;
  }

  std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
      out += "\n    " + line;
    }
    return out;
  }

  // The built-in colors are written as 7-digit decimals of n/255 (0.0784314F for 20/255), while a parsed hex color is
  // n/255.0F exactly, so the documented default loads about 1e-7 away. A hex color says no more than 8 bits; compare at
  // that precision.
  Config quantizedColors(Config config) {
    auto& colors = config.colors;
    for (auto* color :
         {&colors.background, &colors.textPrimary, &colors.textMuted, &colors.accentPrimary, &colors.accentSecondary,
          &colors.warning, &colors.error, &colors.insertHint, &colors.backdrop, &colors.shadow, &colors.border.focused,
          &colors.border.unfocused, &colors.border.outer, &colors.overview.backgroundTint,
          &colors.overview.workspaceBackground, &colors.overview.badge}) {
      for (float& component : *color) {
        component = std::round(component * 255.0F) / 255.0F;
      }
    }
    return config;
  }

} // namespace

UMBRIEL_TEST(pathsDescribeEveryEntryAtOnce) {
  const std::regex index(R"(\[[0-9]+\])");
  for (const auto& [path, spec] : schema().entries) {
    // Indices and sample names exist only in the skeleton document.
    if (std::regex_search(path, index) || path.contains("Super+Return")) {
      umbriel::test::reportFailure(__FILE__, __LINE__, std::format("{} is not normalized", path));
    }
  }
}

// An overlay entry that no reader claims describes a key that was renamed or removed.
UMBRIEL_TEST(overlayDescribesOnlyKeysTheReadersClaim) {
  for (const auto& path : schema().staleOverlay) {
    umbriel::test::reportFailure(__FILE__, __LINE__, std::format("overlay describes unclaimed key {}", path));
  }
}

// Every stated default, written into a config, must load as the built-in default and draw no diagnostic.
UMBRIEL_TEST(statedDefaultsLoadAsTheBuiltInDefaults) {
  const Loaded builtIn = load(sectionsOnly());
  CHECK(builtIn.messages.empty());
  toml::table document = sectionsOnly();
  int written = 0;
  for (const auto& [path, spec] : schema().entries) {
    if (!spec.defaultValue || familyPath(path)) {
      continue;
    }
    CHECK(umbriel::setSchemaValue(document, schema().shapes, path, *spec.defaultValue));
    ++written;
  }
  CHECK(written > 100);
  const Loaded withDefaults = load(document);
  CHECK(withDefaults.messages.empty());
  if (!withDefaults.messages.empty()) {
    umbriel::test::reportFailure(
        __FILE__, __LINE__, "stated defaults draw diagnostics:" + joined(withDefaults.messages)
    );
  }
  if (quantizedColors(withDefaults.config) == quantizedColors(builtIn.config)) {
    return;
  }
  // Narrow it down to the keys at fault.
  for (const auto& [path, spec] : schema().entries) {
    if (!spec.defaultValue || familyPath(path)) {
      continue;
    }
    toml::table single = sectionsOnly();
    (void)umbriel::setSchemaValue(single, schema().shapes, path, *spec.defaultValue);
    if (!(quantizedColors(load(single).config) == quantizedColors(builtIn.config))) {
      umbriel::test::reportFailure(__FILE__, __LINE__, std::format("default of {} is not the built-in one", path));
    }
  }
}

// Enum spellings the overlay states must be ones the reader accepts.
UMBRIEL_TEST(statedEnumValuesAreAccepted) {
  const Loaded baseline = load(schema().skeleton);
  int tried = 0;
  for (const auto& [path, spec] : schema().entries) {
    if (spec.type != SchemaType::Enum && spec.type != SchemaType::EnumOrArray) {
      continue;
    }
    for (const auto& value : spec.values) {
      toml::table document = schema().skeleton;
      CHECK(umbriel::setSchemaValue(document, schema().shapes, path, value));
      const Loaded loaded = load(document);
      ++tried;
      if (loaded.messages != baseline.messages) {
        umbriel::test::reportFailure(
            __FILE__, __LINE__,
            std::format("{} = \"{}\" is stated but rejected:{}", path, value, joined(loaded.messages))
        );
      }
    }
  }
  CHECK(tried > 50);
}

// A consumer caches the schema by revision: `version` stays the same across many commits.
UMBRIEL_TEST(jsonCarriesTheRevisionOrNull) {
  const auto withRevision = nlohmann::json::parse(umbriel::configSchemaJson(schema(), "0.1.0", "810711bc8009"));
  CHECK(withRevision["version"] == "0.1.0");
  CHECK(withRevision["revision"] == "810711bc8009");
  CHECK(withRevision["options"].size() == schema().entries.size());
  const auto withoutRevision = nlohmann::json::parse(umbriel::configSchemaJson(schema(), "0.1.0", std::nullopt));
  CHECK(withoutRevision["revision"].is_null());
}

// The readable summary counts each option once: under its top-level section, and never the tables that group them.
UMBRIEL_TEST(summaryCountsEveryOptionOnce) {
  const std::string summary = umbriel::configSchemaSummary(schema());
  const auto options = std::ranges::count_if(schema().entries, [](const auto& entry) {
    const SchemaType type = entry.second.type;
    return type != SchemaType::Table && type != SchemaType::Map && type != SchemaType::ArrayOfTables;
  });
  CHECK(summary.starts_with(std::format("{} options in ", options)));
  CHECK(summary.contains("\nwindow_rule "));
  CHECK(!summary.contains("window_rule[]"));
}

// Without a place to write its skeleton the run must fail, not return a schema with almost no keys.
UMBRIEL_TEST(unwritableTemporaryDirectoryFailsTheRun) {
  const char* previous = std::getenv("TMPDIR");
  const std::string saved = previous != nullptr ? previous : "";
  setenv("TMPDIR", "/nonexistent/umbriel-schema-test", 1);
  const auto result = umbriel::buildConfigSchema();
  if (previous != nullptr) {
    setenv("TMPDIR", saved.c_str(), 1);
  } else {
    unsetenv("TMPDIR");
  }
  CHECK(!result.has_value());
}

int main() {
  // Skeleton entries lack required fields by design; their diagnostics are compared, not worth printing.
  setConsoleLogging(false);
  return RUN_TESTS();
}
