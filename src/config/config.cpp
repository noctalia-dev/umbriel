// What a config file holds: the top-level tables in reading order, and reading a merged document into a Config.

#include "config/config.h"

#include "config/config_merge.h"
#include "config/config_registry.h"
#include "config/fields.h"
#include "config/overrides.h"
#include "config/section.h"
#include "config/store.h"

#include <algorithm>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    // Every top-level table, in reading order: a table may depend on one read before it, as hot corner actions
    // depend on the scratchpads they name.
    const registry::Fields<Config>& configFields() {
      using registry::KeyDescription;
      static const registry::Fields<Config> fields{
          colorsTable(),
          effectsTable(),
          animationTable(),
          appearanceTable(),
          overviewTable(),
          scratchpadTable(),
          hotCornersTable(),
          layoutTable(),
          generalTable(),
          drmTable(),
          environmentTable(),
          eventsTable(),
          workspaceSettingsTable(),
          screencastTable(),
          inputTable(),
          outputTable(),
          keybindsTable(),
          windowRuleTable(),
          layerRuleTable(),
          securityContextRuleTable(),
          workspaceRulesTable(),
          // Read before any table, by the include merge; declared here so the schema lists it.
          registry::handRead<Config>(
              "include", KeyDescription("table"),
              [] {
                registry::Descriptions keys{
                    KeyDescription("string_array").withFormat("path"),
                    KeyDescription("table"),
                    KeyDescription("string_array").withFormat("path"),
                };
                keys[0].path = ".files";
                keys[1].path = ".optional";
                keys[2].path = ".optional.files";
                return keys;
              }(),
              [](Section&, Config&, registry::ReadContext&) {}
          ),
      };
      return fields;
    }

    // The effect readers read only declared names. Names a runtime override introduces are declared after the file's,
    // and a name the file already declares as the other kind is dropped, as the include merge drops a pool shadowed by
    // a preset.
    void declareOverrideEffects(configmerge::MergeResult& result, const toml::table& overrides) {
      const auto* effects = overrides.get_as<toml::table>("effects");
      if (effects == nullptr) {
        return;
      }
      const auto declared = [](const std::vector<configmerge::EffectDeclaration>& declarations, std::string_view name) {
        return std::ranges::find(declarations, name, &configmerge::EffectDeclaration::name) != declarations.end();
      };
      const auto declare = [&](std::string_view kind, std::vector<configmerge::EffectDeclaration>& own,
                               const std::vector<configmerge::EffectDeclaration>& other, std::string_view otherKind) {
        const auto* names = effects->get_as<toml::table>(kind);
        if (names == nullptr) {
          return;
        }
        for (const auto& [name, value] : *names) {
          if (declared(other, name.str())) {
            emitDiag(
                ConfigDiagnostic::Severity::Warning, nullptr,
                std::format("ignoring effects.{}.{} ({} with this name is declared)", kind, name.str(), otherKind)
            );
            if (auto* merged = result.merged["effects"][kind].as_table()) {
              merged->erase(name.str());
            }
            continue;
          }
          if (!declared(own, name.str())) {
            own.push_back({.name = std::string(name.str()), .source = {}});
          }
        }
      };
      declare("preset", result.presets, result.pools, "pool");
      declare("pool", result.pools, result.presets, "preset");
    }

  } // namespace

  ConfigParseOutcome parseConfig(Config& out, toml::table& effective, const std::filesystem::path* rootPath) {
    ConfigStore& store = configStore();
    try {
      configmerge::MergeResult result;
      if (rootPath != nullptr) {
        result = configmerge::mergeWithIncludes(*rootPath);
      }
      store.setMissingIncludes(result.missingIncludes);
      for (auto& diagnostic : result.diagnostics) {
        store.addDiagnostic(std::move(diagnostic));
      }
      for (const auto& path : result.loadedFiles) {
        store.addWatchPath(path);
      }
      configoverrides::overlay(result.merged, store.overrides());
      declareOverrideEffects(result, store.overrides());
      if ((result.missingIncludes || result.missingOptionalIncludes) && result.merged.contains("drm")) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr, "cannot safely load DRM policy while an include is missing"
        );
        return ConfigParseOutcome::Failed;
      }
      if (result.hadError) {
        return ConfigParseOutcome::Failed;
      }

      Config loaded;
      if (rootPath == nullptr) {
        // Without a file the session runs the default binds, which overrides only add to.
        loaded.keybinds = defaultKeybinds();
      }
      std::vector<EffectReference> effectReferences;
      {
        Section root(result.merged, "", store.mutableDiagnostics());
        registry::ReadContext context{
            .loaded = loaded,
            .effectReferences = effectReferences,
            .presetDeclarations = result.presets,
            .poolDeclarations = result.pools
        };
        registry::readFields(root, configFields(), loaded, context);
        auto& titleBar = loaded.appearance.titleBar;
        if (titleBar.buttonSize == 0 || titleBar.buttonSize > titleBar.height) {
          const int used = std::clamp(titleBar.buttonSize, 1, titleBar.height);
          warnAt(
              result.merged.at_path("appearance.title_bar.button_size").node()->source(),
              "appearance.title_bar.button_size = {} out of range (-1 or 1 to height {}), clamped to {}",
              titleBar.buttonSize, titleBar.height, used
          );
          titleBar.buttonSize = used;
        }
        if (titleBar.buttonGlyphSize == 0) {
          warnAt(
              result.merged.at_path("appearance.title_bar.button_glyph_size").node()->source(),
              "appearance.title_bar.button_glyph_size = 0 out of range (-1 or 1 to 128), clamped to 1"
          );
          titleBar.buttonGlyphSize = 1;
        }
        // A thinner stroke antialiases to nothing visible at scale 1.
        constexpr double kMinGlyphStroke = 0.5;
        if (titleBar.buttonGlyphStroke != -1.0 && titleBar.buttonGlyphStroke < kMinGlyphStroke) {
          warnAt(
              result.merged.at_path("appearance.title_bar.button_glyph_stroke").node()->source(),
              "appearance.title_bar.button_glyph_stroke = {} out of range (-1 or {} to 16), clamped to {}",
              titleBar.buttonGlyphStroke, kMinGlyphStroke, kMinGlyphStroke
          );
          titleBar.buttonGlyphStroke = kMinGlyphStroke;
        }
        warnScrollButtonBinds(loaded);
        validateEffectReferences(loaded, effectReferences);
        auto& workspaces = loaded.animation.workspaces;
        if (workspaces.style == "reveal") {
          const auto* preset = findEffectPreset(loaded.effects, workspaces.effect);
          if (preset == nullptr || preset->inert()) {
            warnAt(
                result.merged.at_path("animation.workspaces.style").node()->source(),
                "ignoring animation.workspaces.style (reveal requires an animation preset with a shader)"
            );
            workspaces.style = "slide";
          }
        }
      }

      // Reject config if any error-level diagnostics were emitted.
      const bool hasErrors = std::ranges::any_of(store.diagnostics(), [](const ConfigDiagnostic& d) {
        return d.severity == ConfigDiagnostic::Severity::Error;
      });
      if (hasErrors) {
        return ConfigParseOutcome::Failed;
      }

      out = std::move(loaded);
      effective = std::move(result.merged);
      return ConfigParseOutcome::Loaded;
    } catch (const std::exception& exception) {
      emitDiag(ConfigDiagnostic::Severity::Error, nullptr, std::format("config load error: {}", exception.what()));
    } catch (...) {
      emitDiag(ConfigDiagnostic::Severity::Error, nullptr, "config load error: unknown error");
    }
    return ConfigParseOutcome::Failed;
  }

  registry::Descriptions registry::describeConfig(const Config& values) {
    Descriptions keys;
    describeFields(configFields(), values, "", keys);
    return keys;
  }

  const Config::Input::Device* Config::Input::findDevice(std::string_view name) const {
    const auto found = std::ranges::find_if(devices, [name](const Device& device) { return device.name == name; });
    return found == devices.end() ? nullptr : &*found;
  }

} // namespace umbriel
