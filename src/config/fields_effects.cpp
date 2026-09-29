// [effects] presets, pools, and deferred reference validation.

#include "config/fields.h"
#include "config/store.h"

#include <algorithm>
#include <format>
#include <optional>
#include <set>
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

    bool validEffectName(std::string_view name, const std::string& path, const toml::source_region& source) {
      if (name.empty()) {
        warnAt(source, "ignoring {} (effect names must not be empty)", path);
        return false;
      }
      if (name == kEffectOff) {
        warnAt(source, "ignoring {} ('off' is reserved)", path);
        return false;
      }
      if (name.contains('/')) {
        warnAt(source, "ignoring {} ('/' is an action delimiter; rename this effect)", path);
        return false;
      }
      return true;
    }

    // Presets are named by the user, and which keys one takes depends on its kind.
    void readEffectPresets(Section& presets, Effects& effects, registry::ReadContext& context) {
      for (const auto& declaration : context.presetDeclarations) {
        const std::string& name = declaration.name;
        const toml::node* node = presets.table().get(name);
        if (node == nullptr) {
          continue;
        }
        const toml::node& entry = *node;
        const std::string path = "effects.preset." + name;
        const auto* table = entry.as_table();
        if (table == nullptr) {
          warnAt(entry.source(), "ignoring {} (expected table)", path);
          continue;
        }
        if (!validEffectName(name, path, declaration.source)) {
          continue;
        }
        Section keys(*table, path, configStore().mutableDiagnostics());
        const toml::node* kindNode = keys.take("kind");
        const std::optional<EffectKind> kind =
            kindNode != nullptr ? parseEffectKind(kindNode->value<std::string>().value_or("")) : std::nullopt;
        if (!kind) {
          warnAt(
              kindNode != nullptr ? kindNode->source() : declaration.source,
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
          warnAt(declaration.source, "{} has no shader; the preset is inert", path);
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

    void readEffectPools(Section& pools, Effects& effects, registry::ReadContext& context) {
      for (const auto& declaration : context.poolDeclarations) {
        const std::string& name = declaration.name;
        const std::string path = "effects.pool." + name;
        const toml::node* entry = pools.table().get(name);
        if (entry == nullptr) {
          continue;
        } // A colliding pool was rejected before parsing.
        if (!validEffectName(name, path, declaration.source)) {
          continue;
        }
        const auto* table = entry->as_table();
        if (table == nullptr) {
          warnAt(entry->source(), "ignoring {} (expected table)", path);
          continue;
        }
        Section keys(*table, path, configStore().mutableDiagnostics());
        const toml::node* kindNode = keys.take("kind");
        const auto kind = kindNode != nullptr && kindNode->is_string()
            ? parseEffectKind(kindNode->value<std::string>().value_or(""))
            : std::nullopt;
        if (!kind || *kind == EffectKind::Animation) {
          warnAt(
              kindNode != nullptr ? kindNode->source() : declaration.source,
              "ignoring {} (kind must be border|window|screen|cursor)", path
          );
          keys.freeform();
          continue;
        }
        const toml::node* chooseNode = keys.take("choose");
        const auto* choose = chooseNode != nullptr ? chooseNode->as_array() : nullptr;
        if (choose == nullptr) {
          warnAt(
              chooseNode != nullptr ? chooseNode->source() : declaration.source,
              "ignoring {} (choose is required and must be an array)", path
          );
          keys.freeform();
          continue;
        }
        EffectPool pool{.name = name, .kind = *kind, .members = {}};
        if (const toml::node* selection = keys.take("selection")) {
          const auto policy = selection->is_string()
              ? parseEffectSelectionPolicy(selection->value<std::string>().value_or(""))
              : std::nullopt;
          if (!policy) {
            warnAt(selection->source(), "ignoring {} (selection must be unused_first|round_robin|random)", path);
            keys.freeform();
            continue;
          }
          pool.selection = *policy;
        }
        if (choose->empty()) {
          warnAt(chooseNode->source(), "{} has no members; the pool is inert", path);
        }
        const size_t poolIndex = effects.pools.size();
        effects.pools.push_back(std::move(pool));
        std::set<std::string> seen;
        for (const auto& member : *choose) {
          const auto value = member.value<std::string>();
          if (!member.is_string() || !value || value->empty() || *value == kEffectOff) {
            warnAt(
                member.source(), "ignoring {}.choose member (expected a non-empty preset name other than 'off')", path
            );
            continue;
          }
          if (!seen.insert(*value).second) {
            warnAt(member.source(), "ignoring {}.choose member '{}' (duplicate member)", path, *value);
            continue;
          }
          auto& members = effects.pools[poolIndex].members;
          const size_t memberIndex = members.size();
          members.push_back(*value);
          addEffectReference(
              context.effectReferences, path + ".choose", {*value, member.source()}, *kind, false,
              [&effects, poolIndex, memberIndex] { effects.pools[poolIndex].members[memberIndex].clear(); }
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
              "preset", KeyDescription("table"), readEffectPresets,
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
          registry::map<Effects>("pool", KeyDescription("table"), readEffectPools, [] {
            registry::Descriptions keys;
            auto kind = KeyDescription("enum").withValues({"border", "window", "screen", "cursor"});
            kind.path = "kind";
            keys.push_back(std::move(kind));
            auto choose = KeyDescription("string_array").withFormat("effect");
            choose.path = "choose";
            keys.push_back(std::move(choose));
            auto selection = KeyDescription("enum").withValues({"unused_first", "round_robin", "random"});
            selection.path = "selection";
            selection.defaultValue = "unused_first";
            keys.push_back(std::move(selection));
            return keys;
          }()),
      };
      return fields;
    }

  } // namespace

  registry::Field<Config> effectsTable() { return registry::table("effects", &Config::effects, effectsFields()); }

  void validateEffectReferences(Config& loaded, std::vector<EffectReference>& references) {
    for (EffectReference& reference : references) {
      if (const auto error = effectReferenceError(
              loaded.effects, reference.name, reference.kind, reference.allowOff, reference.constraint
          )) {
        warnAt(reference.source, "ignoring {} ({})", reference.context, *error);
        reference.clear();
      }
    }
    // All rejection callbacks have run: only now may member indices change.
    for (auto& pool : loaded.effects.pools) {
      std::erase(pool.members, std::string{});
    }
  }

} // namespace umbriel
