// `umbriel config schema` prints what the registry declares. Values a declaration states by hand, rather than taking
// from the field it reads, must agree with the reader.

#include "check.h"
#include "config/config.h"
#include "config/config_registry.h"
#include "config/schema.h"
#include "core/log.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using umbriel::registry::Descriptions;
using umbriel::registry::KeyDescription;

namespace {

  const std::filesystem::path& configPath() {
    static const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("umbriel-config-schema-" + std::to_string(getpid()) + ".toml");
    return path;
  }

  struct Loaded {
    umbriel::Config config;
    std::vector<std::string> messages;
  };

  Loaded load(const toml::table& document) {
    {
      std::ofstream out(configPath());
      out << document;
    }
    (void)umbriel::loadConfig(configPath().c_str());
    Loaded loaded{.config = umbriel::config(), .messages = {}};
    for (const auto& diagnostic : umbriel::configDiagnostics()) {
      loaded.messages.push_back(diagnostic.message);
    }
    std::filesystem::remove(configPath());
    return loaded;
  }

  // Store a JSON scalar or array of scalars in `parent` under `key`.
  template <typename Parent, typename Key> void put(Parent& parent, Key&& key, const nlohmann::ordered_json& value) {
    const auto store = [&](auto&& node) {
      if constexpr (std::is_same_v<Parent, toml::array>) {
        parent.push_back(std::forward<decltype(node)>(node));
      } else {
        parent.insert_or_assign(std::forward<Key>(key), std::forward<decltype(node)>(node));
      }
    };
    if (value.is_boolean()) {
      store(value.get<bool>());
    } else if (value.is_number_integer()) {
      store(value.get<int64_t>());
    } else if (value.is_number()) {
      store(value.get<double>());
    } else if (value.is_string()) {
      store(value.get<std::string>());
    } else {
      toml::array array;
      for (const auto& item : value) {
        put(array, 0, item);
      }
      store(std::move(array));
    }
  }

  // The key a schema path names, in a document of its own. `name[]` becomes a one-entry array and `<name>` a sample
  // name; `concrete` receives the path as diagnostics spell it.
  toml::table documentSetting(std::string_view path, const nlohmann::ordered_json& value, std::string& concrete) {
    toml::table document;
    toml::table* current = &document;
    std::string_view rest = path;
    std::string previous;
    concrete.clear();
    while (true) {
      const size_t dot = rest.find('.');
      std::string segment(rest.substr(0, dot));
      const bool last = dot == std::string_view::npos;
      const bool entry = segment.ends_with("[]");
      if (entry) {
        segment.resize(segment.size() - 2);
      }
      if (segment == "<name>") {
        segment = previous == "keybinds" ? "Super+Return" : "sample";
      }
      concrete += (concrete.empty() ? "" : ".") + segment + (entry ? "[0]" : "");
      if (last) {
        put(*current, segment, value);
        return document;
      }
      if (entry) {
        current->insert_or_assign(segment, toml::array{toml::table{}});
        current = current->get(segment)->as_array()->get(0)->as_table();
      } else {
        current = current->insert(segment, toml::table{}).first->second.as_table();
      }
      previous = segment;
      rest = rest.substr(dot + 1);
    }
  }

  bool familyPath(std::string_view path) { return path.contains("[]") || path.contains("<name>"); }

  std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
      out += "\n    " + line;
    }
    return out;
  }

} // namespace

// Every stated default, written into a config, draws no diagnostic and loads as the built-in value.
UMBRIEL_TEST(statedDefaultsLoadAsTheBuiltInValues) {
  const Descriptions builtIn = umbriel::registry::describeConfig(umbriel::Config{});
  toml::table document;
  int written = 0;
  for (const KeyDescription& key : builtIn) {
    if (key.defaultValue.is_null() || familyPath(key.path)) {
      continue;
    }
    std::string concrete;
    const toml::table single = documentSetting(key.path, key.defaultValue, concrete);
    // Merge the single setting into one document.
    toml::table* target = &document;
    const toml::table* source = &single;
    while (true) {
      const auto& [name, node] = *source->begin();
      if (const toml::table* nested = node.as_table()) {
        target = target->insert(name, toml::table{}).first->second.as_table();
        source = nested;
        continue;
      }
      target->insert_or_assign(name, node);
      break;
    }
    ++written;
  }
  CHECK(written > 100);

  const Loaded loaded = load(document);
  if (!loaded.messages.empty()) {
    umbriel::test::reportFailure(__FILE__, __LINE__, "stated defaults draw diagnostics:" + joined(loaded.messages));
  }
  const Descriptions reloaded = umbriel::registry::describeConfig(loaded.config);
  CHECK_EQ(reloaded.size(), builtIn.size());
  for (size_t index = 0; index < builtIn.size() && index < reloaded.size(); ++index) {
    if (reloaded[index].defaultValue != builtIn[index].defaultValue) {
      umbriel::test::reportFailure(
          __FILE__, __LINE__,
          std::format(
              "{} states {} but loads as {}", builtIn[index].path, builtIn[index].defaultValue.dump(),
              reloaded[index].defaultValue.dump()
          )
      );
    }
  }
  // A default a declaration states by hand is checked by the config it loads into, not by describing that config
  // back through the same declaration. Colors are compared above: a built-in color is not an exact n/255.
  umbriel::Config withDefaults = loaded.config;
  const umbriel::Config empty = load(toml::table{}).config;
  withDefaults.colors = empty.colors;
  CHECK(withDefaults == empty);
}

// The JSON a tool caches by revision: fixed root keys, one entry per path in sorted order, integer bounds for integer
// keys, and no default where a key has none.
UMBRIEL_TEST(jsonIsSortedAndTyped) {
  const auto json = nlohmann::ordered_json::parse(
      umbriel::configSchemaJson(umbriel::registry::describeConfig(umbriel::Config{}), "0.1.0", std::nullopt)
  );
  std::vector<std::string> roots;
  for (const auto& [key, value] : json.items()) {
    roots.push_back(key);
  }
  CHECK(roots == std::vector<std::string>({"version", "revision", "options"}));
  CHECK(json["revision"].is_null());
  std::string previous;
  bool sawUnsetKey = false;
  for (const auto& option : json["options"]) {
    const std::string path = option["path"];
    if (!(previous < path)) {
      umbriel::test::reportFailure(__FILE__, __LINE__, std::format("{} is out of order or repeated", path));
    }
    previous = path;
    for (const char* bound : {"min", "max"}) {
      if (option["type"] == "int" && option.contains(bound) && !option[bound].is_number_integer()) {
        umbriel::test::reportFailure(__FILE__, __LINE__, std::format("{} has a non-integer {}", path, bound));
      }
    }
    if (path == "input.touchpad.natural_scroll") {
      sawUnsetKey = true;
      CHECK(!option.contains("default"));
    }
  }
  CHECK(sawUnsetKey);
}

// Every value a key lists is one its reader accepts without complaint.
UMBRIEL_TEST(listedValuesAreAccepted) {
  int tried = 0;
  for (const KeyDescription& key : umbriel::registry::describeConfig(umbriel::Config{})) {
    for (const std::string_view value : key.values) {
      std::string concrete;
      const toml::table document = documentSetting(key.path, nlohmann::ordered_json(value), concrete);
      const Loaded loaded = load(document);
      ++tried;
      for (const std::string& message : loaded.messages) {
        if (message.contains(concrete)) {
          umbriel::test::reportFailure(
              __FILE__, __LINE__, std::format(R"({} = "{}" is listed but rejected: {})", key.path, value, message)
          );
        }
      }
    }
  }
  CHECK(tried > 50);
}

int main() {
  // The documents are partial configs; their unrelated complaints are filtered, not worth printing.
  setConsoleLogging(false);
  return RUN_TESTS();
}
