// [output.<name>] rules.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/value_parse.h"
#include "output/identity.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    OutputRule* findOutputRuleMutable(Config& loaded, const std::string& name) {
      const auto it = std::ranges::find_if(loaded.outputs, [&](const OutputRule& rule) {
        return outputNamesEqual(rule.name, name);
      });
      return it != loaded.outputs.end() ? &*it : nullptr;
    }

    // A count, a list of names, or "dynamic".
    void parseOutputWorkspaces(const toml::node& node, const std::string& path, OutputRule& rule) {
      if (const auto count = node.value<std::int64_t>()) {
        if (*count < 1 || *count > static_cast<std::int64_t>(kMaxWorkspaces)) {
          errorAt(node.source(), "{} must be an integer from 1 to {}", path, kMaxWorkspaces);
        } else {
          rule.workspaces = static_cast<size_t>(*count);
        }
        return;
      }
      if (const auto* names = node.as_array()) {
        bool valid = true;
        if (names->empty() || names->size() > kMaxWorkspaces) {
          errorAt(node.source(), "{} must contain 1 to {} names", path, kMaxWorkspaces);
          valid = false;
        }
        std::vector<std::string> parsed;
        parsed.reserve(names->size());
        for (const auto& item : *names) {
          const auto value = item.value<std::string>();
          if (!value || value->empty()) {
            errorAt(item.source(), "{} entries must be non-empty strings", path);
            valid = false;
            continue;
          }
          if (std::ranges::find(parsed, *value) != parsed.end()) {
            errorAt(item.source(), "{} contains duplicate name '{}'", path, *value);
            valid = false;
            continue;
          }
          parsed.push_back(*value);
        }
        if (valid) {
          rule.workspaces = std::move(parsed);
        }
        return;
      }
      if (node.value<std::string>() != "dynamic") {
        errorAt(node.source(), R"({} must be a count, a name array, or "dynamic")", path);
      }
    }

    const registry::Fields<OutputRule>& outputFields() {
      using registry::boolean;
      using registry::custom;
      using registry::KeyDescription;
      using O = OutputRule;
      static const registry::Fields<O::Layout::Scrolling> scrolling{
          registry::real("default_extent_fraction", 0.1, 1.0, &O::Layout::Scrolling::defaultExtentFraction),
      };
      static const registry::Fields<O::Layout> layout{
          registry::table("scrolling", &O::Layout::scrolling, scrolling),
      };
      static const registry::Fields<O> fields{
          boolean("enabled", &O::enabled),
          boolean("tearing", &O::allowTearing),
          boolean("direct_scanout", &O::directScanout),
          optionalEffectField("screen_effect", &O::screenEffect),
          registry::table("layout", &O::layout, layout),
          registry::integer("min_workspaces", 1, static_cast<int>(kMaxWorkspaces), &O::minWorkspaces),
          boolean("cyclic_workspaces", &O::cyclicWorkspaces),
          registry::choice(
              "workspace_axis", &O::workspaceAxis,
              {{.name = "vertical", .value = WorkspaceAxis::Vertical},
               {.name = "horizontal", .value = WorkspaceAxis::Horizontal}}
          ),
          custom<O>(
              "workspaces", KeyDescription("int_or_string_array").withValues({"dynamic"}).withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                parseOutputWorkspaces(node, path, target);
              },
              [](const O&) { return nlohmann::ordered_json("dynamic"); }
          ),
          custom<O>(
              "mode", KeyDescription("string").withFormat("output_mode"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::string>();
                OutputMode mode;
                if (!value || !parseOutputMode(*value, mode)) {
                  warnAt(node.source(), R"(ignoring {} (expected "WIDTHxHEIGHT" or "WIDTHxHEIGHT@HZ"))", path);
                } else {
                  target.mode = mode;
                }
              }
          ),
          custom<O>(
              "position", KeyDescription("int_array").withRange(-100000, 100000),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto* position = node.as_array();
                bool valid = position != nullptr && position->size() == 2;
                std::array<int, 2> parsed{};
                for (size_t index = 0; valid && index < parsed.size(); ++index) {
                  const auto value = (*position)[index].value<std::int64_t>();
                  valid = value.has_value();
                  if (valid) {
                    parsed[index] = static_cast<int>(
                        std::clamp(*value, static_cast<std::int64_t>(-100000), static_cast<std::int64_t>(100000))
                    );
                  }
                }
                if (!valid) {
                  warnAt(node.source(), "ignoring {} (expected [x, y] integers)", path);
                } else {
                  target.position = parsed;
                }
              }
          ),
          registry::real("scale", 0.25, 4.0, &O::scale),
          registry::choice("vrr", &O::vrr, vrrModes()),
          registry::choice("hdr", &O::hdr, hdrModes()),
          registry::real("sdr_white", 80.0, 1000.0, &O::sdrWhite),
          custom<O>(
              "bit_depth", KeyDescription("int").withRange(8, 10),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::int64_t>();
                if (value && (*value == 8 || *value == 10)) {
                  target.bitDepth = static_cast<int>(*value);
                } else {
                  warnAt(node.source(), "ignoring {} (expected 8 or 10)", path);
                }
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.bitDepth); }
          ),
          registry::choice(
              "transform", &O::transform,
              {{.name = "normal", .value = 0},
               {.name = "90", .value = 1},
               {.name = "180", .value = 2},
               {.name = "270", .value = 3},
               {.name = "flipped", .value = 4},
               {.name = "flipped-90", .value = 5},
               {.name = "flipped-180", .value = 6},
               {.name = "flipped-270", .value = 7}}
          ),
      };
      return fields;
    }

    // A later section for the same output replaces an earlier one.
    void acceptOutput(
        const toml::key& key, Section& keys, const toml::node&, OutputRule& rule, Config& loaded,
        registry::ReadContext& context
    ) {
      const std::string name(key.str());
      rule.name = name;
      if (std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name);
          })) {
        warnAt(key.source(), "duplicate output section '{}'", name);
        // Drop the discarded section's references so they cannot clear the surviving rule's value by name.
        std::erase_if(context.effectReferences, [&](const EffectReference& reference) {
          return std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name)
                && reference.context == "output." + existing.name + ".screen_effect";
          });
        });
        std::erase_if(loaded.outputs, [&](const OutputRule& existing) {
          return outputNamesEqual(existing.name, name);
        });
      }
      if (const toml::node* minNode = keys.node("min_workspaces"); minNode != nullptr && rule.workspaces) {
        errorAt(minNode->source(), "{} requires dynamic workspaces", keys.qualified("min_workspaces"));
      }
      loaded.outputs.push_back(std::move(rule));
      recordRuleEffect(context, keys, "screen_effect", EffectKind::Screen, [&loaded, name] {
        if (OutputRule* output = findOutputRuleMutable(loaded, name)) {
          output->screenEffect.reset();
        }
      });
    }

  } // namespace

  registry::Field<Config> outputTable() {
    return registry::namedTables<Config>("output", outputFields(), acceptOutput);
  }

} // namespace umbriel
