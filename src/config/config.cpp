// What a config file holds: the top-level tables in reading order, and reading a merged document into a Config.

#include "config/config.h"

#include "config/config_merge.h"
#include "config/config_registry.h"
#include "config/fields.h"
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

  } // namespace

  ConfigParseOutcome parseConfig(Config& out, const std::filesystem::path& rootPath) {
    ConfigStore& store = configStore();
    bool drmPolicyRequested = false;
    try {
      auto result = configmerge::mergeWithIncludes(rootPath);
      drmPolicyRequested = hasRequestedDrmPolicy(result.merged);
      store.setMissingIncludes(result.missingIncludes);
      for (auto& diagnostic : result.diagnostics) {
        store.addDiagnostic(std::move(diagnostic));
      }
      for (const auto& path : result.loadedFiles) {
        store.addWatchPath(path);
      }
      if ((result.missingIncludes || result.missingOptionalIncludes) && result.merged.contains("drm")) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr, "cannot safely load DRM policy while an include is missing"
        );
        return ConfigParseOutcome::Fatal;
      }
      if (result.hadError) {
        // Invalid syntax or include directives can hide DRM policy intent.
        // Do not silently start with defaults or a partial exclusion list.
        return ConfigParseOutcome::Fatal;
      }

      Config loaded;
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
        warnScrollButtonBinds(loaded);
        validateEffectReferences(loaded, effectReferences);
      }

      // Reject config if any error-level diagnostics were emitted.
      const bool hasErrors = std::ranges::any_of(store.diagnostics(), [](const ConfigDiagnostic& d) {
        return d.severity == ConfigDiagnostic::Severity::Error;
      });
      if (hasErrors) {
        return drmPolicyRequested ? ConfigParseOutcome::Fatal : ConfigParseOutcome::DefaultsAllowed;
      }

      out = std::move(loaded);
      return ConfigParseOutcome::Loaded;
    } catch (const std::exception& exception) {
      emitDiag(ConfigDiagnostic::Severity::Error, nullptr, std::format("config load error: {}", exception.what()));
    } catch (...) {
      emitDiag(ConfigDiagnostic::Severity::Error, nullptr, "config load error: unknown error");
    }
    return drmPolicyRequested ? ConfigParseOutcome::Fatal : ConfigParseOutcome::DefaultsAllowed;
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
