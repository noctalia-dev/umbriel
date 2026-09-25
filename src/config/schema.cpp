#include "config/schema.h"

#include "config/config.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

namespace umbriel {

  namespace {

    using Shapes = std::map<std::string, SchemaShape>;

    // What the readers cannot say for themselves: enum spellings, string formats, compound types, and the ranges and
    // defaults of keys read through `take` or into an empty target, plus keys read from raw tables that no Section
    // claims. Everything else in the schema comes from the readers. A key missing here still appears, with the type
    // the run could observe.
    struct OverlayEntry {
      std::string path;
      KeySpec spec;
      // Read without a Section, so the run cannot observe it and this entry is the only record.
      bool raw = false;
    };

    KeySpec enumOf(std::vector<std::string> values, std::optional<std::string> fallback = std::nullopt) {
      KeySpec spec = KeySpec(SchemaType::Enum).withValues(std::move(values));
      if (fallback) {
        spec.defaultValue = std::move(*fallback);
      }
      return spec;
    }

    std::vector<OverlayEntry> overlay() {
      std::vector<OverlayEntry> entries;
      const auto add = [&](std::string path, KeySpec spec) {
        entries.push_back({.path = std::move(path), .spec = std::move(spec), .raw = false});
      };
      const auto addRaw = [&](std::string path, KeySpec spec) {
        entries.push_back({.path = std::move(path), .spec = std::move(spec), .raw = true});
      };
      constexpr double kMaxWorkspaces = 64;

      // Layout, globally and per workspace.
      for (const std::string prefix : {"layout", "workspace[].layout"}) {
        const bool global = prefix == "layout";
        const auto fallback = [&](std::string value) { return global ? std::optional(value) : std::nullopt; };
        add(prefix + ".mode", enumOf({"scrolling", "dwindle", "master"}, fallback("scrolling")));
        add(prefix + ".master.position", enumOf({"left", "right", "center"}, fallback("left")));
        add(prefix + ".scrolling.center_focused", enumOf({"never", "always", "on_overflow"}, fallback("never")));
        add(prefix + ".new_exits_fullscreen",
            KeySpec(SchemaType::EnumOrArray)
                .withValues({"tiled", "floating", "pinned", "all"})
                .withDefault(global ? std::optional<SchemaValue>(std::vector<std::string>{}) : std::nullopt));
        add(prefix + ".extent_presets",
            KeySpec(SchemaType::FloatArray)
                .withRange(0.1, 1.0)
                .withDefault(
                    global ? std::optional<SchemaValue>(std::vector<double>{1.0 / 3, 0.5, 2.0 / 3}) : std::nullopt
                ));
      }
      add("workspace[].name", KeySpec(SchemaType::String));
      add("workspace[].output", KeySpec(SchemaType::String));
      add("workspace[].index", KeySpec(SchemaType::Int).withRange(1, kMaxWorkspaces));
      add("scratchpad[].name", KeySpec(SchemaType::String));

      // Animation.
      add("animation.duration_ms", KeySpec(SchemaType::Int).withRange(1, 10000).withDefault(std::int64_t{250}));
      add("animation.curve", KeySpec(SchemaType::String).withFormat("curve"));
      add("animation.beziers", KeySpec(SchemaType::Map));
      addRaw("animation.beziers.<name>", KeySpec(SchemaType::FloatArray).withFormat("bezier"));
      add("animation.springs", KeySpec(SchemaType::Map));
      addRaw("animation.springs.<name>", KeySpec(SchemaType::Table));
      addRaw("animation.springs.<name>.damping", KeySpec(SchemaType::Float).withRange(0.01, 5.0));
      addRaw("animation.springs.<name>.stiffness", KeySpec(SchemaType::Float).withRange(1.0, 10000.0));
      for (const std::string event :
           {"windows_in", "windows_out", "windows_move", "workspaces", "overview", "scratchpad", "border",
            "dim_unfocused", "layers"}) {
        add("animation." + event + ".curve", KeySpec(SchemaType::String).withFormat("curve"));
        add("animation." + event + ".shader", KeySpec(SchemaType::String).withFormat("path"));
      }
      add("animation.overview.workspace_curve", KeySpec(SchemaType::String).withFormat("curve"));
      add("animation.windows_in.style", enumOf({"popin", "zoom", "slide", "fade", "none"}));
      add("animation.windows_out.style", enumOf({"fade", "slide", "popin", "zoom"}));

      // General, overview, hot corners, DRM, environment.
      add("general.mod_key", enumOf({"Super", "Alt", "Ctrl", "Shift"}));
      add("overview.shortcut_keys", KeySpec(SchemaType::String).withDefault(std::string("1234567890")));
      for (const std::string corner : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
        add("hot_corners." + corner + ".action", KeySpec(SchemaType::String).withFormat("action"));
      }
      add("drm", KeySpec(SchemaType::Table));
      addRaw("drm.ignored_devices", KeySpec(SchemaType::StringArray).withFormat("path"));
      addRaw("drm.ignored_pci_addresses", KeySpec(SchemaType::StringArray).withFormat("pci_address"));
      add("environment.<name>", KeySpec(SchemaType::String));
      // Consumed by config_merge before any reader runs.
      addRaw("include", KeySpec(SchemaType::Table));
      addRaw("include.files", KeySpec(SchemaType::StringArray).withFormat("path"));
      addRaw("include.optional", KeySpec(SchemaType::Table));
      addRaw("include.optional.files", KeySpec(SchemaType::StringArray).withFormat("path"));

      // Input.
      add("input.window_drag_toggle", enumOf({"none", "floating", "pinned"}, "none"));
      add("input.keyboard.track_layout", enumOf({"global", "window"}, "global"));
      add("input.touchpad.scroll_factor", KeySpec(SchemaType::FloatOrTable).withRange(0.1, 10.0));
      add("input.tablet.calibration_matrix", KeySpec(SchemaType::FloatArray));
      for (const std::string device : {"input.touchpad", "input.mouse", "input.device[]"}) {
        add(device + ".accel_profile",
            KeySpec(SchemaType::String).withValues({"flat", "adaptive"}).withFormat("accel_profile"));
      }
      for (const std::string device : {"input.touchpad", "input.device[]"}) {
        add(device + ".click_method", enumOf({"button_areas", "clickfinger"}));
        add(device + ".tap_button_map", enumOf({"left_right_middle", "left_middle_right"}));
      }
      for (const std::string device : {"input.mouse", "input.device[]"}) {
        add(device + ".scroll_button", enumOf({"MouseLeft", "MouseRight", "MouseMiddle", "MouseBack", "MouseForward"}));
      }
      for (const std::string key : {"name", "layout", "variant", "options"}) {
        add("input.device[]." + key, KeySpec(SchemaType::String));
      }

      // Outputs.
      addRaw("output.<name>", KeySpec(SchemaType::Table));
      add("output.<name>.workspace_axis", enumOf({"vertical", "horizontal"}, "vertical"));
      add("output.<name>.workspaces",
          KeySpec(SchemaType::IntOrStringArray)
              .withValues({"dynamic"})
              .withRange(1, kMaxWorkspaces)
              .withDefault(std::string("dynamic")));
      add("output.<name>.mode", KeySpec(SchemaType::String).withFormat("output_mode"));
      add("output.<name>.position", KeySpec(SchemaType::IntArray).withRange(-100000, 100000));
      add("output.<name>.vrr", enumOf({"disabled", "always", "fullscreen"}, "disabled"));
      add("output.<name>.hdr", enumOf({"off", "on", "auto", "fullscreen"}, "off"));
      add("output.<name>.transform",
          enumOf({"normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270"}));

      // Keybinds.
      addRaw("keybinds.<name>", KeySpec(SchemaType::StringOrTable).withFormat("action"));
      add("keybinds.<name>.action", KeySpec(SchemaType::String).withFormat("action"));

      // Rules.
      for (const std::string key : {"app_id", "title", "xdg_tag"}) {
        add("window_rule[].match." + key, KeySpec(SchemaType::String).withFormat("regex"));
      }
      add("window_rule[].match.content_type", enumOf({"none", "photo", "video", "game"}));
      for (const std::string key :
           {"is_focused", "is_floating", "is_pinned", "is_scratchpad", "is_alone", "at_startup"}) {
        add("window_rule[].match." + key, KeySpec(SchemaType::Bool));
      }
      add("window_rule[].vrr", enumOf({"disabled", "always", "fullscreen"}));
      add("window_rule[].hdr", enumOf({"off", "on", "auto", "fullscreen"}));
      add("window_rule[].default_output", KeySpec(SchemaType::String));
      add("window_rule[].default_position.anchor",
          enumOf({"top_left", "top_right", "bottom_left", "bottom_right", "top", "bottom", "left", "right", "center"}));
      add("window_rule[].default_workspace", KeySpec(SchemaType::IntOrString).withRange(1, kMaxWorkspaces));
      add("window_rule[].default_scratchpad", KeySpec(SchemaType::String));
      add("window_rule[].default_scrolling_column", KeySpec(SchemaType::String));
      add("layer_rule[].match.namespace", KeySpec(SchemaType::String).withFormat("regex"));
      for (const std::string key : {"sandbox_engine", "app_id"}) {
        add("security_context_rule[].match." + key, KeySpec(SchemaType::String).withFormat("regex"));
      }
      return entries;
    }

    // Overlay fields replace what the run observed; unset ones keep it (a typed reader's default, say).
    void applyOverlay(KeySpec& target, KeySpec overlaySpec) {
      target.type = overlaySpec.type;
      if (!overlaySpec.values.empty()) {
        target.values = std::move(overlaySpec.values);
      }
      if (overlaySpec.min) {
        target.min = overlaySpec.min;
        target.max = overlaySpec.max;
      }
      if (overlaySpec.defaultValue) {
        target.defaultValue = std::move(overlaySpec.defaultValue);
      }
      if (!overlaySpec.format.empty()) {
        target.format = std::move(overlaySpec.format);
      }
    }

    std::vector<std::string_view> splitPath(std::string_view path) {
      std::vector<std::string_view> segments;
      size_t start = 0;
      while (true) {
        const size_t dot = path.find('.', start);
        segments.push_back(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
        if (dot == std::string_view::npos) {
          return segments;
        }
        start = dot + 1;
      }
    }

    std::string_view leafKey(std::string_view path) {
      const size_t lastDot = path.rfind('.');
      return lastDot == std::string_view::npos ? path : path.substr(lastDot + 1);
    }

    // The table holding the key at schema `path`. Walks existing tables only.
    toml::table* parentTable(toml::table& document, const Shapes& shapes, std::string_view path) {
      const size_t lastDot = path.rfind('.');
      if (lastDot == std::string_view::npos) {
        return &document;
      }
      toml::table* current = &document;
      std::string walked;
      for (const std::string_view segment : splitPath(path.substr(0, lastDot))) {
        std::string key(segment);
        const bool arrayEntry = key.ends_with("[]");
        if (arrayEntry) {
          key.resize(key.size() - 2);
        }
        if (key == "<name>") {
          const auto map = shapes.find(walked);
          if (map == shapes.end() || map->second.type != SchemaType::Map) {
            return nullptr;
          }
          key = map->second.sampleKey;
        }
        walked += walked.empty() ? std::string(segment) : "." + std::string(segment);
        toml::node* next = current->get(key);
        if (next != nullptr && arrayEntry) {
          toml::array* array = next->as_array();
          next = array == nullptr || array->empty() ? nullptr : array->get(0);
        }
        current = next == nullptr ? nullptr : next->as_table();
        if (current == nullptr) {
          return nullptr;
        }
      }
      return current;
    }

    toml::table emptyEntry(const SchemaShape& shape) {
      return shape.type == SchemaType::Map ? toml::table{{shape.sampleKey, toml::table{}}} : toml::table{};
    }

    bool placeShape(toml::table& document, const Shapes& shapes, std::string_view path, const SchemaShape& shape) {
      toml::table* parent = parentTable(document, shapes, path);
      if (parent == nullptr) {
        return false;
      }
      if (shape.type == SchemaType::ArrayOfTables) {
        parent->insert_or_assign(leafKey(path), toml::array{toml::table{}});
      } else {
        parent->insert_or_assign(leafKey(path), emptyEntry(shape));
      }
      return true;
    }

    constexpr std::string_view kLoadFailed = "cannot write a temporary config file for the schema run";

    // nullopt when the document could not be loaded at all: an empty recording would pass for a schema with no keys.
    std::optional<SchemaRecorder> recordLoad(const toml::table& document, const Shapes& shapes) {
      SchemaRecorder recorder;
      for (const auto& [path, shape] : shapes) {
        if (shape.type == SchemaType::ArrayOfTables) {
          recorder.markArray(path);
        } else if (shape.type == SchemaType::Map) {
          recorder.markMap(path, shape.sampleKey);
        }
      }
      const SchemaRecording recording(recorder);
      if (!loadConfigDocument(document)) {
        return std::nullopt;
      }
      return recorder;
    }

    bool claimsUnder(const SchemaRecorder& recorder, std::string_view prefix) {
      const auto found = recorder.entries().lower_bound(std::string(prefix));
      return found != recorder.entries().end() && found->first.starts_with(prefix);
    }

    // Entries that only parse as the right kind of container claim keys beneath them. The map sample is a keybind chord
    // because that reader validates entry names before it reads an entry's keys; the other maps accept it as a name
    // too.
    std::expected<std::optional<SchemaShape>, std::string>
    probeShape(const toml::table& document, const Shapes& shapes, const std::string& path) {
      const std::array candidates{
          SchemaShape{.type = SchemaType::Table, .sampleKey = {}},
          SchemaShape{.type = SchemaType::ArrayOfTables, .sampleKey = {}},
          SchemaShape{.type = SchemaType::Map, .sampleKey = "Super+Return"},
      };
      for (const SchemaShape& candidate : candidates) {
        toml::table probe = document;
        Shapes tentative = shapes;
        tentative.insert_or_assign(path, candidate);
        if (!placeShape(probe, shapes, path, candidate)) {
          return std::optional<SchemaShape>();
        }
        const std::string beneath = candidate.type == SchemaType::ArrayOfTables ? path + "[]."
            : candidate.type == SchemaType::Map                                 ? path + ".<name>."
                                                                                : path + ".";
        const auto recorded = recordLoad(probe, tentative);
        if (!recorded) {
          return std::unexpected(std::string(kLoadFailed));
        }
        if (claimsUnder(*recorded, beneath)) {
          return candidate;
        }
      }
      return std::optional<SchemaShape>();
    }

    nlohmann::ordered_json jsonBound(SchemaType type, double bound) {
      const bool integral = type == SchemaType::Int
          || type == SchemaType::IntArray
          || type == SchemaType::IntOrString
          || type == SchemaType::IntOrStringArray;
      return integral ? nlohmann::ordered_json(static_cast<std::int64_t>(bound)) : nlohmann::ordered_json(bound);
    }

  } // namespace

  bool loadConfigDocument(const toml::table& document) {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
      return false;
    }
    std::string file = (directory / "umbriel-schema-XXXXXX.toml").string();
    const int fd = mkstemps(file.data(), 5);
    if (fd < 0) {
      return false;
    }
    close(fd);
    bool written = false;
    {
      std::ofstream out(file);
      out << document;
      out.flush();
      written = out.good();
    }
    if (written) {
      (void)loadConfig(file.c_str());
    }
    std::filesystem::remove(file, error);
    return written;
  }

  bool setSchemaValue(toml::table& document, const Shapes& shapes, std::string_view path, const SchemaValue& value) {
    toml::table* parent = parentTable(document, shapes, path);
    if (parent == nullptr) {
      return false;
    }
    const std::string_view leaf = leafKey(path);
    std::visit(
        [&](const auto& held) {
          using T = std::decay_t<decltype(held)>;
          if constexpr (std::is_same_v<T, std::vector<double>> || std::is_same_v<T, std::vector<std::string>>) {
            toml::array array;
            for (const auto& item : held) {
              array.push_back(item);
            }
            parent->insert_or_assign(leaf, std::move(array));
          } else {
            parent->insert_or_assign(leaf, held);
          }
        },
        value
    );
    return true;
  }

  std::expected<ConfigSchema, std::string> buildConfigSchema() {
    toml::table document;
    Shapes shapes;
    std::vector<std::string> scalars;
    for (int round = 1; round <= kSchemaMaxRounds; ++round) {
      const auto recorded = recordLoad(document, shapes);
      if (!recorded) {
        return std::unexpected(std::string(kLoadFailed));
      }
      const SchemaRecorder& recorder = *recorded;
      bool grew = false;
      for (const auto& [path, spec] : recorder.entries()) {
        if (shapes.contains(path) || leafKey(path) == "<name>") {
          continue;
        }
        std::optional<SchemaShape> shape;
        if (spec.type == SchemaType::Table) {
          shape = SchemaShape{.type = SchemaType::Table, .sampleKey = {}};
        } else if (spec.type == SchemaType::Unknown && std::ranges::find(scalars, path) == scalars.end()) {
          auto probed = probeShape(document, shapes, path);
          if (!probed) {
            return std::unexpected(probed.error());
          }
          shape = *probed;
          if (!shape) {
            scalars.push_back(path);
          }
        }
        if (shape && placeShape(document, shapes, path, *shape)) {
          shapes.insert_or_assign(path, *shape);
          grew = true;
        }
      }
      if (grew) {
        continue;
      }

      ConfigSchema schema{
          .entries = recorder.entries(),
          .shapes = shapes,
          .skeleton = std::move(document),
          .staleOverlay = {},
      };
      for (const auto& [path, shape] : shapes) {
        schema.entries.try_emplace(path, KeySpec(shape.type)).first->second.type = shape.type;
      }
      for (OverlayEntry& entry : overlay()) {
        const auto found = schema.entries.find(entry.path);
        if (found != schema.entries.end()) {
          applyOverlay(found->second, std::move(entry.spec));
        } else if (entry.raw) {
          schema.entries.emplace(entry.path, std::move(entry.spec));
        } else {
          schema.staleOverlay.push_back(entry.path);
        }
      }
      return schema;
    }
    return std::unexpected(std::format("config schema did not settle within {} rounds", kSchemaMaxRounds));
  }

  std::string configSchemaSummary(const ConfigSchema& schema) {
    // A table, map, or array of tables only groups options; counting it would count the same setting twice.
    std::map<std::string, int> perSection;
    int total = 0;
    for (const auto& [path, spec] : schema.entries) {
      if (spec.type == SchemaType::Table || spec.type == SchemaType::Map || spec.type == SchemaType::ArrayOfTables) {
        continue;
      }
      perSection[path.substr(0, path.find_first_of(".["))] += 1;
      ++total;
    }
    std::string out =
        std::format("{} options in {} sections\n\n{:<24}{:>7}\n", total, perSection.size(), "section", "options");
    for (const auto& [section, count] : perSection) {
      out += std::format("{:<24}{:>7}\n", section, count);
    }
    return out;
  }

  std::string
  configSchemaJson(const ConfigSchema& schema, std::string_view version, std::optional<std::string_view> revision) {
    nlohmann::ordered_json options = nlohmann::ordered_json::array();
    for (const auto& [path, spec] : schema.entries) {
      nlohmann::ordered_json option;
      option["path"] = path;
      option["type"] = std::string(schemaTypeName(spec.type));
      if (!spec.values.empty()) {
        option["values"] = spec.values;
      }
      if (spec.min) {
        option["min"] = jsonBound(spec.type, *spec.min);
      }
      if (spec.max) {
        option["max"] = jsonBound(spec.type, *spec.max);
      }
      if (!spec.format.empty()) {
        option["format"] = spec.format;
      }
      if (spec.defaultValue) {
        option["default"] =
            std::visit([](const auto& held) { return nlohmann::ordered_json(held); }, *spec.defaultValue);
      }
      options.push_back(std::move(option));
    }
    nlohmann::ordered_json root;
    root["version"] = std::string(version);
    root["revision"] = revision ? nlohmann::ordered_json(std::string(*revision)) : nlohmann::ordered_json(nullptr);
    root["options"] = std::move(options);
    return root.dump(2) + "\n";
  }

} // namespace umbriel
