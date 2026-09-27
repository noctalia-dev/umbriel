// [effects] and its user-named presets, and the check of every preset reference once all tables are read.

#include "config/fields.h"
#include "config/store.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    // Reads a string selector under `key`, reporting the section's own "(expected string)" warning through
    // Section::text so every caller shares one wording. Returns the value and its source location, or nullopt when
    // the key was absent or not a string.
    std::optional<std::pair<std::string, toml::source_region>> takeEffectSelector(Section& keys, std::string_view key) {
      const toml::node* node = keys.node(key);
      if (node == nullptr) {
        return std::nullopt;
      }
      std::string value;
      keys.text(key, value); // claims the key and warns if it is not a string
      if (!node->is_string()) {
        return std::nullopt;
      }
      return std::make_pair(std::move(value), node->source());
    }

    // Presets are named by the user, and which keys one takes depends on its kind.
    void readEffectPresets(Section& presets, Effects& effects, registry::ReadContext& context) {
      for (const auto& [key, entry] : presets.table()) {
        const std::string name(key.str());
        const std::string path = "effects.preset." + name;
        const auto* table = entry.as_table();
        if (table == nullptr) {
          warnAt(entry.source(), "ignoring {} (expected table)", path);
          continue;
        }
        if (name == kEffectOff) {
          warnAt(key.source(), "ignoring {} ('off' is reserved)", path);
          continue;
        }
        Section keys(*table, path, configStore().mutableDiagnostics());
        const toml::node* kindNode = keys.take("kind");
        const std::optional<EffectKind> kind =
            kindNode != nullptr ? parseEffectKind(kindNode->value<std::string>().value_or("")) : std::nullopt;
        if (!kind) {
          warnAt(
              kindNode != nullptr ? kindNode->source() : key.source(),
              "ignoring {} (kind must be animation|border|window|screen|cursor)", path
          );
          keys.freeform();
          continue;
        }
        EffectPreset preset;
        preset.name = name;
        preset.kind = *kind;
        auto shader = readShaderSource(keys, "shader", configStore().mutableDiagnostics());
        for (auto& watched : shader.watchPaths) {
          configStore().addWatchPath(std::move(watched));
        }
        if (shader.source) {
          preset.shader = std::move(*shader.source);
        } else if (keys.node("shader") == nullptr) {
          warnAt(key.source(), "{} has no shader; the preset is inert", path);
        }
        keys.boolean("palette", preset.palette);
        // The preset moves into the vector; register the overlay reference by index after the push.
        std::optional<std::pair<std::string, toml::source_region>> overlay;
        switch (*kind) {
        case EffectKind::Border: {
          double speed = preset.speed;
          keys.integer("padding", 0, 1024, preset.padding)
              .real("speed", 0.0, 10.0, speed)
              .boolean("animated", preset.animated);
          preset.speed = static_cast<float>(speed);
          overlay = takeEffectSelector(keys, "overlay");
          if (overlay) {
            preset.overlay = overlay->first;
          }
          keys.sub("light", [&](Section& light) {
            BorderLight settings;
            double intensity = settings.intensity;
            double threshold = settings.threshold;
            light.integer("spread", 1, 256, settings.spread)
                .real("intensity", 0.0, 4.0, intensity)
                .real("threshold", 0.0, 1.0, threshold);
            settings.intensity = static_cast<float>(intensity);
            settings.threshold = static_cast<float>(threshold);
            preset.light = settings;
          });
          break;
        }
        case EffectKind::Cursor:
          keys.integer("radius", 0, 4096, preset.radius);
          break;
        case EffectKind::Animation:
        case EffectKind::Window:
        case EffectKind::Screen:
          break;
        }
        effects.presets.push_back(std::move(preset));
        if (overlay) {
          const size_t index = effects.presets.size() - 1;
          addEffectReference(
              context.effectReferences, path + ".overlay", *overlay, EffectKind::Window, false,
              [&effects, index] { effects.presets[index].overlay.clear(); }
          );
        }
      }
    }

    const registry::Fields<Effects>& effectsFields() {
      using registry::KeyDescription;
      static const registry::Fields<Effects> fields{
          registry::integer("max_fps", 0, 240, &Effects::maxFps),
          registry::boolean("in_capture", &Effects::inCapture),
          effectField("border", &Effects::border, EffectKind::Border),
          effectField("window", &Effects::window, EffectKind::Window),
          effectField("screen", &Effects::screen, EffectKind::Screen),
          effectField("cursor", &Effects::cursor, EffectKind::Cursor),
          registry::map<Effects>(
              "preset", KeyDescription("table"),
              [](Section& presets, Effects& effects, registry::ReadContext& context) {
                readEffectPresets(presets, effects, context);
              },
              [] {
                registry::Descriptions keys;
                const auto add = [&keys](std::string_view key, KeyDescription description) {
                  description.path = key;
                  keys.push_back(std::move(description));
                };
                add("kind", KeyDescription("enum").withValues({"animation", "border", "window", "screen", "cursor"}));
                add("shader", KeyDescription("string").withFormat("path"));
                add("palette", KeyDescription("bool"));
                add("padding", KeyDescription("int").withRange(0, 1024));
                add("speed", KeyDescription("float").withRange(0.0, 10.0));
                add("animated", KeyDescription("bool"));
                add("overlay", KeyDescription("string").withFormat("effect"));
                add("light", KeyDescription("table"));
                add("light.spread", KeyDescription("int").withRange(1, 256));
                add("light.intensity", KeyDescription("float").withRange(0.0, 4.0));
                add("light.threshold", KeyDescription("float").withRange(0.0, 1.0));
                add("radius", KeyDescription("int").withRange(0, 4096));
                return keys;
              }()
          ),
      };
      return fields;
    }

  } // namespace

  registry::Field<Config> effectsTable() { return registry::table("effects", &Config::effects, effectsFields()); }

  // Every recorded reference is checked against the final preset table. `clear` mutates `loaded` through
  // references captured while parsing, so this takes it non-const to say so.
  void validateEffectReferences(Config& loaded, std::vector<EffectReference>& references) {
    for (EffectReference& reference : references) {
      if (const auto error = effectReferenceError(loaded.effects, reference.name, reference.kind, reference.allowOff)) {
        warnAt(reference.source, "ignoring {} ({})", reference.context, *error);
        reference.clear();
      }
    }
  }

} // namespace umbriel
