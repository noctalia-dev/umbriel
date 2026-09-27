// [[scratchpad]], [[window_rule]], [[layer_rule]], and [[security_context_rule]].

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/store.h"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    const registry::Fields<ScratchpadConfig>& scratchpadFields() {
      static const registry::Fields<ScratchpadConfig> fields{
          registry::checked<ScratchpadConfig>(
              "name", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, ScratchpadConfig& target, registry::ReadContext&) {
                const auto name = node.value<std::string>();
                if (!name) {
                  errorAt(node.source(), "{} must be a string", path);
                } else if (name->empty()) {
                  errorAt(node.source(), "{} must not be empty", path);
                } else if (*name == "default") {
                  errorAt(node.source(), "{} 'default' is reserved for the implicit scratchpad", path);
                } else {
                  target.name = *name;
                  return true;
                }
                return false;
              }
          ),
          registry::text("spawn_when_empty", &ScratchpadConfig::spawnWhenEmpty),
      };
      return fields;
    }

    bool acceptScratchpad(
        Section& keys, const toml::node& entry, ScratchpadConfig& scratchpad, Config& loaded, registry::ReadContext&,
        bool fieldsAccepted
    ) {
      if (!fieldsAccepted) {
        return false;
      }
      const toml::node* nameNode = keys.node("name");
      if (nameNode == nullptr) {
        errorAt(entry.source(), "{} must set name", keys.name());
        return false;
      }
      if (std::ranges::any_of(loaded.scratchpads, [&](const ScratchpadConfig& existing) {
            return existing.name == scratchpad.name;
          })) {
        errorAt(nameNode->source(), "{}.name duplicates scratchpad name '{}'", keys.name(), scratchpad.name);
        return false;
      }
      return true;
    }

    // A regex pattern a rule matches with. One that does not compile rejects the rule.
    template <typename T>
    registry::Field<T> regexField(std::string_view key, std::string T::* pattern, std::regex T::* regex) {
      return registry::checked<T>(
          key, registry::KeyDescription("string").withFormat("regex"),
          [pattern, regex](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            const auto value = node.value<std::string>();
            if (!value) {
              warnAt(node.source(), "ignoring {} (expected string)", path);
              return true;
            }
            target.*pattern = *value;
            try {
              target.*regex = std::regex(*value);
            } catch (const std::regex_error& error) {
              warnAt(node.source(), "invalid regex in {}: {}", path, error.what());
              return false;
            }
            return true;
          }
      );
    }

    // A match table that is not a table leaves its rule selecting nothing it meant to, so the rule is dropped.
    template <typename T> bool rejectUnlessTable(const toml::node& node, T&, registry::ReadContext&) {
      return node.is_table();
    }

    // `{ x, y, anchor }`, placed only when both coordinates are set and the anchor is valid.
    void parseDefaultPosition(const toml::node& node, const std::string& path, WindowRule& rule) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        warnAt(node.source(), "ignoring {} (expected table)", path);
        return;
      }
      static const registry::Choices<WindowPositionAnchor> anchors{
          {.name = "top_left", .value = WindowPositionAnchor::TopLeft},
          {.name = "top_right", .value = WindowPositionAnchor::TopRight},
          {.name = "bottom_left", .value = WindowPositionAnchor::BottomLeft},
          {.name = "bottom_right", .value = WindowPositionAnchor::BottomRight},
          {.name = "top", .value = WindowPositionAnchor::Top},
          {.name = "bottom", .value = WindowPositionAnchor::Bottom},
          {.name = "left", .value = WindowPositionAnchor::Left},
          {.name = "right", .value = WindowPositionAnchor::Right},
          {.name = "center", .value = WindowPositionAnchor::Center},
      };
      Section position(*table, path, configStore().mutableDiagnostics());
      std::optional<int> x;
      std::optional<int> y;
      position.integer("x", -100000, 100000, x).integer("y", -100000, 100000, y);
      std::optional<WindowPositionAnchor> anchor = WindowPositionAnchor::Center;
      if (const toml::node* anchorNode = position.take("anchor")) {
        anchor =
            registry::readChoice(position, *anchorNode, position.qualified("anchor"), anchors, registry::Case::Fold);
      }
      if (!x || !y) {
        warnAt(node.source(), "ignoring {} (x and y are required integers)", path);
      } else if (anchor) {
        rule.defaultPosition = WindowPosition{.x = *x, .y = *y, .anchor = *anchor};
      }
    }

    // A string that must not be empty.
    template <typename T>
    registry::Field<T> nonEmptyText(std::string_view key, std::optional<std::string> T::* member) {
      return registry::custom<T>(
          key, registry::KeyDescription("string"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            const auto value = node.value<std::string>();
            if (!value || value->empty()) {
              warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
            } else {
              target.*member = *value;
            }
          }
      );
    }

    const registry::Fields<WindowRule>& windowRuleFields() {
      using registry::boolean;
      using registry::color;
      using registry::custom;
      using registry::integer;
      using registry::KeyDescription;
      using registry::real;
      using registry::strict;
      using W = WindowRule;
      const auto self = [](auto& rule) -> auto& { return rule; };
      // Any mistake in the match rejects the rule: one that selects less than it says would restyle other windows.
      static const registry::Fields<W> match{
          strict(regexField("app_id", &W::appIdPattern, &W::appIdRegex)),
          strict(regexField("title", &W::titlePattern, &W::titleRegex)),
          strict(regexField("xdg_tag", &W::xdgTagPattern, &W::xdgTagRegex)),
          strict(registry::choice("content_type", &W::matchContentType, contentTypes())),
          strict(boolean("is_focused", &W::matchFocused)),
          strict(boolean("is_floating", &W::matchFloating)),
          strict(boolean("is_pinned", &W::matchPinned)),
          strict(boolean("is_scratchpad", &W::matchScratchpad)),
          strict(boolean("is_alone", &W::matchAlone)),
          strict(boolean("at_startup", &W::matchAtStartup)),
      };
      static const registry::Fields<W> floatingSize{
          real("width", 0.1, 1.0, &W::defaultFloatingWidth),
          real("height", 0.1, 1.0, &W::defaultFloatingHeight),
      };
      static const registry::Fields<W> floatingSizePx{
          integer("width", 1, 100000, &W::defaultFloatingWidthPx),
          integer("height", 1, 100000, &W::defaultFloatingHeightPx),
      };
      static const registry::Fields<W> fields{
          registry::table<W>("match", self, match, rejectUnlessTable<W>),
          boolean("default_floating", &W::defaultFloating),
          boolean("default_fullscreen", &W::defaultFullscreen),
          boolean("default_maximize_to_edges", &W::defaultMaximizeToEdges),
          boolean("default_maximize", &W::defaultMaximize),
          boolean("default_focused", &W::defaultFocused),
          boolean("default_pinned", &W::defaultPinned),
          boolean("focus_on_activate", &W::focusOnActivate),
          boolean("tearing", &W::allowTearing),
          boolean("blur", &W::blur),
          boolean("blur_popups", &W::blurPopups),
          boolean("blur_optimized", &W::blurOptimized),
          real("opacity", 0.0, 1.0, &W::opacity),
          real("blur_ignore_alpha", 0.0, 1.0, &W::blurIgnoreAlpha),
          color("border_color_focused", &W::borderColorFocused),
          color("border_color_unfocused", &W::borderColorUnfocused),
          color("border_color_outer", &W::borderColorOuter),
          integer("border_width", 0, 100, &W::borderWidth),
          integer("outer_border_width", 0, 100, &W::outerBorderWidth),
          integer("corner_radius", 0, 100, &W::cornerRadius),
          boolean("shadow", &W::shadow),
          optionalEffectField("border_effect", &W::borderEffect),
          optionalEffectField("window_effect", &W::windowEffect),
          registry::table<W>("default_floating_size", self, floatingSize),
          registry::table<W>("default_floating_size_px", self, floatingSizePx),
          registry::choice("vrr", &W::vrr, vrrModes()),
          registry::choice("hdr", &W::hdr, hdrModes()),
          registry::text("default_output", &W::defaultOutput),
          custom<W>(
              "default_position", KeyDescription("table"),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext&) {
                parseDefaultPosition(node, path, target);
              },
              nullptr,
              [] {
                registry::Descriptions keys{
                    KeyDescription("int").withRange(-100000, 100000),
                    KeyDescription("int").withRange(-100000, 100000),
                    KeyDescription("enum").withValues(
                        {"top_left", "top_right", "bottom_left", "bottom_right", "top", "bottom", "left", "right",
                         "center"}
                    ),
                };
                keys[0].path = "x";
                keys[1].path = "y";
                keys[2].path = "anchor";
                keys[2].defaultValue = "center";
                return keys;
              }()
          ),
          integer("default_scrolling_extent_px", 1, 100000, &W::defaultScrollingExtentPx),
          real("default_scrolling_extent", 0.1, 1.0, &W::defaultScrollingExtent),
          custom<W>(
              "default_workspace", KeyDescription("int_or_string").withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext&) {
                if (const auto value = node.value<std::int64_t>()) {
                  if (*value >= 1 && *value <= static_cast<std::int64_t>(kMaxWorkspaces)) {
                    target.defaultWorkspace = WorkspaceReference{WorkspaceIndex{static_cast<size_t>(*value)}};
                    return;
                  }
                } else if (const auto name = node.value<std::string>(); name && !name->empty()) {
                  target.defaultWorkspace = WorkspaceReference{WorkspaceName{*name}};
                  return;
                }
                warnAt(node.source(), "ignoring {} (expected integer 1-{} or non-empty string)", path, kMaxWorkspaces);
              }
          ),
          custom<W>(
              "default_scratchpad", KeyDescription("string"),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext& context) {
                const auto value = node.value<std::string>();
                if (!value || value->empty()) {
                  warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
                } else if (const auto invalid = scratchpadTargetError(context.loaded, *value)) {
                  warnAt(node.source(), "ignoring {} ({})", path, *invalid);
                } else {
                  target.defaultScratchpad = *value;
                }
              }
          ),
          nonEmptyText("default_scrolling_column", &W::defaultScrollingColumn),
          integer(
              "default_scrolling_column_order", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
              &W::defaultScrollingColumnOrder
          ),
      };
      return fields;
    }

    // The rule's effects are recorded once its place in the array is known.
    bool acceptWindowRule(
        Section& keys, const toml::node&, WindowRule&, Config& loaded, registry::ReadContext& context,
        bool fieldsAccepted
    ) {
      if (!fieldsAccepted) {
        return false;
      }
      const size_t index = loaded.windowRules.size();
      recordRuleEffect(context, keys, "border_effect", EffectKind::Border, [&loaded, index] {
        loaded.windowRules[index].borderEffect.reset();
      });
      recordRuleEffect(context, keys, "window_effect", EffectKind::Window, [&loaded, index] {
        loaded.windowRules[index].windowEffect.reset();
      });
      return true;
    }

    const registry::Fields<LayerRule>& layerRuleFields() {
      using L = LayerRule;
      static const registry::Fields<L> match{
          regexField("namespace", &L::namespacePattern, &L::namespaceRegex),
      };
      static const registry::Fields<L> fields{
          registry::table<L>(
              "match", [](auto& rule) -> auto& { return rule; }, match
          ),
          registry::boolean("blur", &L::blur),
          registry::boolean("blur_popups", &L::blurPopups),
          registry::real("blur_ignore_alpha", 0.0, 1.0, &L::ignoreAlpha),
          registry::boolean("blur_optimized", &L::optimized),
      };
      return fields;
    }

    // A security-context selector: a non-empty pattern that compiles, or the rule is dropped.
    registry::Field<SecurityContextRule> securityPattern(
        std::string_view key, std::string SecurityContextRule::* pattern, std::regex SecurityContextRule::* regex
    ) {
      return registry::checked<SecurityContextRule>(
          key, registry::KeyDescription("string").withFormat("regex"),
          [pattern, regex](
              const toml::node& node, const std::string& path, SecurityContextRule& target, registry::ReadContext&
          ) {
            const auto value = node.value<std::string>();
            if (!value || value->empty()) {
              warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
              return false;
            }
            target.*pattern = *value;
            try {
              target.*regex = std::regex(*value);
            } catch (const std::regex_error& error) {
              warnAt(node.source(), "invalid regex in {}: {}", path, error.what());
              return false;
            }
            return true;
          }
      );
    }

    // Any mistake rejects the whole entry: a rule missing its selector would apply to every restricted client.
    const registry::Fields<SecurityContextRule>& securityContextRuleFields() {
      using S = SecurityContextRule;
      static const registry::Fields<S> match{
          securityPattern("sandbox_engine", &S::sandboxEnginePattern, &S::sandboxEngineRegex),
          securityPattern("app_id", &S::appIdPattern, &S::appIdRegex),
      };
      static const registry::Fields<S> fields{
          registry::table<S>(
              "match", [](auto& rule) -> auto& { return rule; }, match,
              [](const toml::node& node, S&, registry::ReadContext&) {
                const toml::table* table = node.as_table();
                if (table == nullptr) {
                  return false;
                }
                if (!registry::declaresAll(match, *table)) {
                  warnAt(node.source(), "ignoring security_context_rule (unknown key in match)");
                  return false;
                }
                return true;
              }
          ),
          registry::strings("allow_globals", &S::allowGlobals),
      };
      return fields;
    }

    bool acceptSecurityContextRule(
        Section& keys, const toml::node& entry, SecurityContextRule& rule, Config&, registry::ReadContext&,
        bool fieldsAccepted
    ) {
      // The filter refuses this global regardless; warning here tells the user why.
      if (std::erase(rule.allowGlobals, "wp_security_context_manager_v1") > 0) {
        warnAt(
            entry.source(),
            "ignoring wp_security_context_manager_v1 in security_context_rule.allow_globals (nested contexts stay "
            "blocked)"
        );
      }
      bool valid = fieldsAccepted;
      if (rule.allowGlobals.empty()) {
        warnAt(entry.source(), "ignoring security_context_rule (allow_globals is empty)");
        valid = false;
      }
      if (!keys.allKeysKnown()) {
        warnAt(entry.source(), "ignoring security_context_rule (unknown key)");
        valid = false;
      }
      return valid;
    }

  } // namespace

  registry::Field<Config> scratchpadTable() {
    return registry::rules(
        "scratchpad", &Config::scratchpads, scratchpadFields(), registry::Shape::Error, acceptScratchpad
    );
  }

  registry::Field<Config> windowRuleTable() {
    return registry::rules(
        "window_rule", &Config::windowRules, windowRuleFields(), registry::Shape::Warning, acceptWindowRule
    );
  }

  registry::Field<Config> layerRuleTable() {
    return registry::rules("layer_rule", &Config::layerRules, layerRuleFields(), registry::Shape::Warning);
  }

  registry::Field<Config> securityContextRuleTable() {
    return registry::rules(
        "security_context_rule", &Config::securityContextRules, securityContextRuleFields(), registry::Shape::Warning,
        acceptSecurityContextRule
    );
  }

} // namespace umbriel
