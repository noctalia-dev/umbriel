// [layout], and [[workspace]] rules with their per-workspace layout overrides.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/resolve.h"
#include "config/store.h"
#include "output/identity.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    const registry::Choices<LayoutMode>& layoutModes() {
      static const registry::Choices<LayoutMode> choices{
          {.name = "scrolling", .value = LayoutMode::Scrolling},
          {.name = "dwindle", .value = LayoutMode::Dwindle},
          {.name = "master", .value = LayoutMode::Master},
      };
      return choices;
    }

    const registry::Choices<MasterPosition>& masterPositions() {
      static const registry::Choices<MasterPosition> choices{
          {.name = "left", .value = MasterPosition::Left},
          {.name = "right", .value = MasterPosition::Right},
          {.name = "center", .value = MasterPosition::Center},
      };
      return choices;
    }

    const registry::Choices<CenterFocusedColumn>& centerFocusedModes() {
      static const registry::Choices<CenterFocusedColumn> choices{
          {.name = "never", .value = CenterFocusedColumn::Never},
          {.name = "always", .value = CenterFocusedColumn::Always},
          {.name = "on_overflow", .value = CenterFocusedColumn::OnOverflow},
      };
      return choices;
    }

    const registry::Choices<NewTabPosition>& newTabPositions() {
      static const registry::Choices<NewTabPosition> choices{
          {.name = "end", .value = NewTabPosition::End},
          {.name = "after_active", .value = NewTabPosition::AfterActive},
      };
      return choices;
    }

    const registry::Choices<FullscreenExitScope>& fullscreenExitScopes() {
      static const registry::Choices<FullscreenExitScope> choices{
          {.name = "tiled", .value = FullscreenExitScope::Tiled},
          {.name = "floating", .value = FullscreenExitScope::Floating},
          {.name = "pinned", .value = FullscreenExitScope::Pinned},
          {.name = "all", .value = FullscreenExitScope::All},
      };
      return choices;
    }

    // A string names one scope; an array combines several, and an empty array disables the behavior.
    std::optional<FullscreenExitScope> parseFullscreenExitScope(const toml::node& node, const std::string& path) {
      const std::string expected = registry::quotedList(registry::choiceNames(fullscreenExitScopes()));
      const auto find = [](std::string_view token) -> std::optional<FullscreenExitScope> {
        const auto& choices = fullscreenExitScopes();
        const auto found = std::ranges::find(choices, token, &registry::Choice<FullscreenExitScope>::name);
        return found == choices.end() ? std::nullopt : std::optional(found->value);
      };
      if (const auto* value = node.as_string()) {
        const std::optional<FullscreenExitScope> scope = find(value->get());
        if (!scope) {
          warnAt(node.source(), R"(ignoring {} "{}" (expected {}))", path, value->get(), expected);
        }
        return scope;
      }
      const auto* array = node.as_array();
      if (array == nullptr) {
        warnAt(node.source(), "ignoring {} (expected a string or an array of strings, each {})", path, expected);
        return std::nullopt;
      }
      auto combined = static_cast<uint8_t>(FullscreenExitScope::None);
      for (const toml::node& entry : *array) {
        const auto* value = entry.as_string();
        const std::optional<FullscreenExitScope> scope = value != nullptr ? find(value->get()) : std::nullopt;
        if (!scope) {
          warnAt(entry.source(), "ignoring {} entry (expected {})", path, expected);
          continue;
        }
        combined |= static_cast<uint8_t>(*scope);
      }
      return static_cast<FullscreenExitScope>(combined);
    }

    // The scope as it is written: `all` when it covers every scope, otherwise each scope it names.
    nlohmann::ordered_json fullscreenExitScopeJson(FullscreenExitScope scope) {
      nlohmann::ordered_json names = nlohmann::ordered_json::array();
      if (scope == FullscreenExitScope::All) {
        names.push_back("all");
        return names;
      }
      for (const auto& option : fullscreenExitScopes()) {
        if (option.value != FullscreenExitScope::All
            && (static_cast<uint8_t>(scope) & static_cast<uint8_t>(option.value)) != 0) {
          names.push_back(option.name);
        }
      }
      return names;
    }

    std::optional<std::vector<double>> parseExtentPresets(const toml::node& node, const std::string& path) {
      const auto* array = node.as_array();
      if (array == nullptr || array->empty()) {
        warnAt(node.source(), "ignoring {} (expected non-empty array of numbers)", path);
        return std::nullopt;
      }

      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          warnAt(node.source(), "ignoring {} (expected non-empty array of numbers)", path);
          return std::nullopt;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          warnAt(entry.source(), "{} = {} out of range, clamped to {}", path, *value, used);
        }
        parsed.push_back(used);
      }
      return parsed;
    }

    // The global `[layout]` and a workspace's `layout` override share their keys; `L` is Config::Layout or
    // WorkspaceLayoutOverrides, whose members are the same, optional in the override.
    template <typename L> const registry::Fields<L>& layoutFields() {
      using registry::boolean;
      using registry::choice;
      using registry::custom;
      using registry::Fields;
      using registry::integer;
      using registry::real;
      using registry::table;
      using Struts = decltype(L::struts);
      using Scrolling = decltype(L::scrolling);
      using Dwindle = decltype(L::dwindle);
      using Master = decltype(L::master);
      using Tabs = decltype(L::tabs);
      constexpr int kStrutLimit = 65535;

      static const Fields<Struts> struts{
          integer("left", -kStrutLimit, kStrutLimit, &Struts::left),
          integer("right", -kStrutLimit, kStrutLimit, &Struts::right),
          integer("top", -kStrutLimit, kStrutLimit, &Struts::top),
          integer("bottom", -kStrutLimit, kStrutLimit, &Struts::bottom),
      };
      static const Fields<Scrolling> scrolling{
          real("default_extent_fraction", 0.1, 1.0, &Scrolling::defaultExtentFraction),
          boolean("center_underfull_strip", &Scrolling::centerUnderfullStrip),
          choice("center_focused", &Scrolling::centerFocused, centerFocusedModes()),
      };
      static const Fields<Dwindle> dwindle{
          boolean("preserve_split", &Dwindle::preserveSplit),
          boolean("new_toward_cursor", &Dwindle::newTowardCursor),
      };
      static const Fields<Master> master{
          choice("position", &Master::position, masterPositions()),
          real("default_width_fraction", 0.1, 0.9, &Master::defaultWidthFraction),
          boolean("new_on_top", &Master::newOnTop),
          boolean("new_becomes_master", &Master::newBecomesMaster),
      };
      static const Fields<Tabs> tabs{
          choice("default_display", &Tabs::defaultDisplay, columnDisplays()),
          choice("new_tab_position", &Tabs::newTabPosition, newTabPositions()),
          boolean("wrap_focus", &Tabs::wrapFocus),
          boolean("scroll_switches_tabs", &Tabs::scrollSwitchesTabs),
          boolean("middle_click_closes", &Tabs::middleClickCloses),
      };
      static const Fields<L> fields{
          choice("mode", &L::mode, layoutModes()),
          integer("gap", 0, 500, &L::gap),
          table("struts", &L::struts, struts),
          custom<L>(
              "extent_presets", registry::KeyDescription("float_array").withRange(0.1, 1.0),
              [](const toml::node& node, const std::string& path, L& target, registry::ReadContext&) {
                if (auto presets = parseExtentPresets(node, path)) {
                  target.extentPresets = std::move(*presets);
                }
              },
              [](const L& defaults) { return registry::detail::toJson(defaults.extentPresets); }
          ),
          custom<L>(
              "new_exits_fullscreen",
              registry::KeyDescription("enum_or_array").withValues(registry::choiceNames(fullscreenExitScopes())),
              [](const toml::node& node, const std::string& path, L& target, registry::ReadContext&) {
                registry::assign(target.newExitsFullscreen, parseFullscreenExitScope(node, path));
              },
              [](const L& defaults) -> nlohmann::ordered_json {
                if constexpr (std::is_same_v<L, Config::Layout>) {
                  return fullscreenExitScopeJson(defaults.newExitsFullscreen);
                } else {
                  return defaults.newExitsFullscreen ? fullscreenExitScopeJson(*defaults.newExitsFullscreen) : nullptr;
                }
              }
          ),
          table("scrolling", &L::scrolling, scrolling),
          table("dwindle", &L::dwindle, dwindle),
          table("master", &L::master, master),
          table("tabs", &L::tabs, tabs),
      };
      return fields;
    }

    // A non-empty string a workspace rule selects by; anything else is an error.
    registry::Field<WorkspaceConfig> workspaceSelector(std::string_view key, std::string WorkspaceConfig::* member) {
      return registry::custom<WorkspaceConfig>(
          key, registry::KeyDescription("string"),
          [member](const toml::node& node, const std::string& path, WorkspaceConfig& target, registry::ReadContext&) {
            if (const auto value = node.value<std::string>()) {
              if (value->empty()) {
                errorAt(node.source(), "{} must not be empty", path);
              } else {
                target.*member = *value;
              }
            } else {
              errorAt(node.source(), "{} must be a string", path);
            }
          }
      );
    }

    const registry::Fields<WorkspaceConfig>& workspaceFields() {
      static const registry::Fields<WorkspaceConfig> fields{
          workspaceSelector("name", &WorkspaceConfig::name),
          workspaceSelector("output", &WorkspaceConfig::output),
          registry::custom<WorkspaceConfig>(
              "index", registry::KeyDescription("int").withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, WorkspaceConfig& target, registry::ReadContext&) {
                const auto value = node.value<std::int64_t>();
                if (!value || *value < 1 || *value > static_cast<std::int64_t>(kMaxWorkspaces)) {
                  errorAt(node.source(), "{} must be an integer from 1 to {}", path, kMaxWorkspaces);
                } else {
                  target.index = static_cast<int>(*value);
                }
              }
          ),
          registry::table("layout", &WorkspaceConfig::layout, layoutFields<WorkspaceLayoutOverrides>()),
      };
      return fields;
    }

    void readWorkspaces(Section& root, Config& loaded, registry::ReadContext& read) {
      const toml::node* node = root.take("workspace");
      if (node == nullptr) {
        return;
      }
      const auto* workspaces = node->as_array();
      if (workspaces == nullptr) {
        errorAt(node->source(), "workspace must be a [[workspace]] array of tables");
        return;
      }

      struct ParsedEntry {
        WorkspaceConfig ws;
        toml::source_region source;
        int arrayIndex;
      };
      std::vector<ParsedEntry> entries;
      entries.reserve(workspaces->size());

      int entryIndex = 0;
      for (const auto& entry : *workspaces) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          errorAt(entry.source(), "workspace[{}] must be a table", entryIndex);
          ++entryIndex;
          continue;
        }

        const std::string context = std::format("workspace[{}]", entryIndex);
        WorkspaceConfig ws;
        {
          Section keys(*section, context, configStore().mutableDiagnostics());
          registry::readFields(keys, workspaceFields(), ws, read);
        }
        const bool hasName = !ws.name.empty();
        const bool hasIndex = ws.index.has_value();
        if (hasName == hasIndex) {
          errorAt(entry.source(), "{} must set exactly one of name or index", context);
        }

        entries.push_back({std::move(ws), entry.source(), entryIndex});
        ++entryIndex;
      }

      const auto sameSelector = [](const WorkspaceConfig& left, const WorkspaceConfig& right) {
        if (!outputNamesEqual(left.output, right.output) || left.index.has_value() != right.index.has_value()) {
          return false;
        }
        return left.index ? left.index == right.index : left.name == right.name;
      };

      for (size_t i = 0; i < entries.size(); ++i) {
        const auto& current = entries[i];
        const auto& ws = current.ws;
        const std::string context = std::format("workspace[{}]", current.arrayIndex);
        if (ws.name.empty() != ws.index.has_value()) {
          continue;
        }

        for (size_t j = 0; j < i; ++j) {
          if (sameSelector(entries[j].ws, ws)) {
            errorAt(current.source, "{} duplicates workspace rule {}", context, entries[j].arrayIndex);
            break;
          }
        }

        const bool targetExists = workspaceRuleTargetExists(loaded, ws);
        if (!targetExists) {
          const std::string selector =
              ws.index ? std::format("index {}", *ws.index) : std::format("name '{}'", ws.name);
          if (ws.output.empty()) {
            errorAt(current.source, "{}: {} does not match any workspace inventory", context, selector);
          } else {
            errorAt(current.source, "{}: {} does not exist on output '{}'", context, selector, ws.output);
          }
        }
      }

      const size_t sentinelCount = loaded.workspaces.emptyAbove ? 2 : 1;
      const size_t namedCapacity = kMaxWorkspaces - sentinelCount;
      const auto reportDynamicNameOverflow = [&](std::string_view output) {
        std::vector<std::string> names;
        for (const ParsedEntry& entry : entries) {
          const WorkspaceConfig& ws = entry.ws;
          if (ws.name.empty() || ws.index) {
            continue;
          }
          const bool applies =
              output.empty() ? ws.output.empty() : ws.output.empty() || outputNamesEqual(ws.output, output);
          if (!applies || std::ranges::find(names, ws.name) != names.end()) {
            continue;
          }
          names.push_back(ws.name);
          if (names.size() <= namedCapacity) {
            continue;
          }
          const std::string context = std::format("workspace[{}]", entry.arrayIndex);
          const std::string target =
              output.empty() ? "unscoped dynamic outputs" : std::format("dynamic output '{}'", output);
          const std::string_view reservation = sentinelCount == 1
              ? "one workspace slot is reserved for the empty sentinel"
              : "two workspace slots are reserved for empty sentinels";
          errorAt(
              entry.source, "{} exceeds the limit of {} named workspaces for {} because {}", context, namedCapacity,
              target, reservation
          );
          return true;
        }
        return false;
      };

      const bool globalOverflow = reportDynamicNameOverflow({});
      if (!globalOverflow) {
        std::vector<std::string> checkedOutputs;
        for (const ParsedEntry& entry : entries) {
          if (entry.ws.name.empty()
              || entry.ws.output.empty()
              || std::ranges::any_of(checkedOutputs, [&](const std::string& output) {
                   return outputNamesEqual(output, entry.ws.output);
                 })) {
            continue;
          }
          checkedOutputs.push_back(entry.ws.output);
          const auto configured = std::ranges::find_if(loaded.outputs, [&](const OutputRule& output) {
            return outputNamesEqual(output.name, entry.ws.output);
          });
          if (configured == loaded.outputs.end() || !configured->workspaces) {
            reportDynamicNameOverflow(entry.ws.output);
          }
        }
      }

      for (auto& entry : entries) {
        loaded.workspaceRules.push_back(std::move(entry.ws));
      }
    }

  } // namespace

  const registry::Choices<ColumnDisplay>& columnDisplays() {
    static const registry::Choices<ColumnDisplay> choices{
        {.name = "normal", .value = ColumnDisplay::Normal},
        {.name = "tabbed", .value = ColumnDisplay::Tabbed},
    };
    return choices;
  }

  registry::Field<Config> layoutTable() {
    static const registry::Fields<Config::Layout> fields = [] {
      registry::Fields<Config::Layout> parsed = layoutFields<Config::Layout>();
      parsed.push_back(registry::boolean("maximize_to_edges", &Config::Layout::maximizeToEdges));
      return parsed;
    }();
    return registry::table("layout", &Config::layout, fields);
  }

  registry::Field<Config> workspaceRulesTable() {
    return registry::handRead<Config>(
        "workspace", registry::KeyDescription("array_of_tables"),
        [] {
          registry::Descriptions keys;
          registry::describeFields(workspaceFields(), WorkspaceConfig{}, "[]", keys);
          return keys;
        }(),
        readWorkspaces
    );
  }

} // namespace umbriel
