// [colors], [appearance], [overview], [hot_corners], [general], [environment], [events], [workspaces], and
// [screencast].

#include "config/fields.h"
#include "config/keybind_parse.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    constexpr std::array<std::string_view, 7> kReservedEnvironmentNames{
        "WAYLAND_DISPLAY",     "WAYLAND_SOCKET",      "DISPLAY",          "UMBRIEL_SOCKET",
        "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
    };

    const registry::Fields<Config::Colors>& colorFields() {
      using registry::color;
      using C = Config::Colors;
      static const registry::Fields<C::Border> border{
          color("focused", &C::Border::focused),
          color("unfocused", &C::Border::unfocused),
          color("outer", &C::Border::outer),
      };
      static const registry::Fields<C::Overview> overview{
          color("background_tint", &C::Overview::backgroundTint),
          color("workspace_background", &C::Overview::workspaceBackground),
          color("badge", &C::Overview::badge),
      };
      static const registry::Fields<C> fields{
          color("background", &C::background),
          color("text_primary", &C::textPrimary),
          color("text_muted", &C::textMuted),
          color("accent_primary", &C::accentPrimary),
          color("accent_secondary", &C::accentSecondary),
          color("warning", &C::warning),
          color("error", &C::error),
          color("insert_hint", &C::insertHint),
          color("backdrop", &C::backdrop),
          color("shadow", &C::shadow),
          registry::table("border", &C::border, border),
          registry::table("overview", &C::overview, overview),
      };
      return fields;
    }

    const registry::Fields<Config::Appearance>& appearanceFields() {
      using registry::boolean;
      using registry::integer;
      using registry::real;
      using A = Config::Appearance;
      static const registry::Fields<A::Blur> blur{
          boolean("enabled", &A::Blur::enabled),          boolean("optimized", &A::Blur::optimized),
          integer("passes", 0, 8, &A::Blur::passes),      integer("radius", 0, 100, &A::Blur::radius),
          real("noise", 0.0, 1.0, &A::Blur::noise),       real("brightness", 0.0, 2.0, &A::Blur::brightness),
          real("contrast", 0.0, 2.0, &A::Blur::contrast), real("saturation", 0.0, 2.0, &A::Blur::saturation),
      };
      static const registry::Fields<A::Shadow> shadow{
          boolean("enabled", &A::Shadow::enabled),
          integer("softness", 0, 200, &A::Shadow::softness),
          integer("offset_x", -200, 200, &A::Shadow::offsetX),
          integer("offset_y", -200, 200, &A::Shadow::offsetY),
      };
      static const registry::Fields<A> fields{
          integer("border_width", 0, 100, &A::borderWidth),
          integer("outer_border_width", 0, 100, &A::outerBorderWidth),
          integer("corner_radius", 0, 100, &A::cornerRadius),
          real("drag_opacity", 0.0, 1.0, &A::dragOpacity),
          boolean("prefer_no_csd", &A::preferNoCsd),
          boolean("opaque_fullscreen", &A::opaqueFullscreen),
          registry::table("blur", &A::blur, blur),
          registry::table("shadow", &A::shadow, shadow),
      };
      return fields;
    }

    // Overview badge keys: printable ASCII, unique ignoring case.
    std::optional<std::string> parseShortcutKeys(const toml::node& node, const std::string& path) {
      const auto value = node.value<std::string>();
      if (!value) {
        warnAt(node.source(), "ignoring {} (expected string)", path);
        return std::nullopt;
      }
      if (value->size() < 2) {
        warnAt(node.source(), "ignoring {} (expected at least 2 characters)", path);
        return std::nullopt;
      }

      std::string normalized;
      normalized.reserve(value->size());
      for (const unsigned char character : *value) {
        if (character < 0x21 || character > 0x7E) {
          warnAt(node.source(), "ignoring {} (invalid character 0x{:02X})", path, static_cast<unsigned int>(character));
          return std::nullopt;
        }
        const char lowered =
            character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        if (normalized.contains(lowered)) {
          warnAt(
              node.source(), R"(ignoring {} (duplicate key "{}" ignoring ASCII case))", path,
              static_cast<char>(character)
          );
          return std::nullopt;
        }
        normalized.push_back(lowered);
      }
      return value;
    }

    const registry::Fields<Config::Overview>& overviewFields() {
      using registry::boolean;
      using registry::real;
      using O = Config::Overview;
      static const registry::Fields<O> fields{
          real("zoom", 0.1, 0.75, &O::zoom),
          real("scroll_factor_horizontal", 0.1, 10.0, &O::scrollFactorHorizontal),
          real("scroll_factor_vertical", 0.1, 10.0, &O::scrollFactorVertical),
          boolean("background_blur", &O::backgroundBlur),
          boolean("workspace_wallpaper", &O::workspaceWallpaper),
          boolean("shortcuts", &O::shortcuts),
          registry::custom<O>(
              "shortcut_keys", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                if (auto keys = parseShortcutKeys(node, path)) {
                  target.shortcutKeys = std::move(*keys);
                }
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.shortcutKeys); }
          ),
      };
      return fields;
    }

    // A compositor action, as a keybind names it. Scratchpads it names must already be declared.
    std::optional<Keybind> parseActionKey(const toml::node& node, const std::string& path, const Config& loaded) {
      const auto value = node.value<std::string>();
      if (!value) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      Keybind bind;
      if (!parseAction(*value, bind)) {
        warnAt(node.source(), R"(invalid {} "{}")", path, *value);
        return std::nullopt;
      }
      if (const auto invalid =
              scratchpadSelectorError(loaded, bind).or_else([&] { return effectActionError(loaded, bind); })) {
        warnAt(node.source(), "ignoring {} ({})", path, *invalid);
        return std::nullopt;
      }
      return bind;
    }

    const registry::Fields<Config::HotCorners>& hotCornerFields() {
      using Corners = Config::HotCorners;
      using Corner = Config::HotCorner;
      static const registry::Fields<Corner> corner{
          registry::boolean("enabled", &Corner::enabled),
          registry::integer("delay_ms", 0, 10000, &Corner::delayMs),
          registry::custom<Corner>(
              "action", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string& path, Corner& target, registry::ReadContext& context) {
                if (auto bind = parseActionKey(node, path, context.loaded)) {
                  target.action = std::move(*bind);
                }
              }
          ),
      };
      // Corners are ordered top-left, top-right, bottom-left, bottom-right.
      static const registry::Fields<Corners> fields{
          registry::table<Corners>(
              "top_left", [](auto& c) -> auto& { return c.corners[0]; }, corner
          ),
          registry::table<Corners>(
              "top_right", [](auto& c) -> auto& { return c.corners[1]; }, corner
          ),
          registry::table<Corners>(
              "bottom_left", [](auto& c) -> auto& { return c.corners[2]; }, corner
          ),
          registry::table<Corners>("bottom_right", [](auto& c) -> auto& { return c.corners[3]; }, corner),
      };
      return fields;
    }

    const registry::Fields<Config::Workspaces>& workspaceSettingFields() {
      using W = Config::Workspaces;
      static const registry::Fields<W> fields{
          registry::boolean("back_and_forth", &W::backAndForth),
          registry::boolean("empty_above", &W::emptyAbove),
      };
      return fields;
    }

    const registry::Fields<Config::ScreenCast>& screenCastFields() {
      using S = Config::ScreenCast;
      static const registry::Fields<S> fields{
          registry::boolean("disable_dynamic_confirmation", &S::disableDynamicConfirmation),
      };
      return fields;
    }

    const registry::Fields<Config::General>& generalFields() {
      using registry::boolean;
      using G = Config::General;
      static const registry::Fields<G> fields{
          registry::choice(
              "mod_key", &G::modKey,
              {
                  {.name = "Super", .value = ModifierKey::Super},
                  {.name = "Logo", .value = ModifierKey::Super, .alias = true},
                  {.name = "Win", .value = ModifierKey::Super, .alias = true},
                  {.name = "Alt", .value = ModifierKey::Alt},
                  {.name = "Ctrl", .value = ModifierKey::Control},
                  {.name = "Control", .value = ModifierKey::Control, .alias = true},
                  {.name = "Shift", .value = ModifierKey::Shift},
              },
              registry::Case::Fold
          ),
          boolean("xwayland", &G::xwayland),
          boolean("show_cheatsheet", &G::showCheatsheet),
          boolean("focus_on_activate", &G::focusOnActivate),
          boolean("honor_restored_maximize", &G::honorRestoredMaximize),
          registry::strings("autostart", &G::autostart),
      };
      return fields;
    }

    void readEnvironmentVariables(Section& s, Config::Environment& environment) {
      std::vector<std::pair<std::string, std::string>> parsed;
      parsed.reserve(s.table().size());
      for (const auto& [key, value] : s.table()) {
        const auto entry = value.value<std::string>();
        if (!entry) {
          warnAt(value.source(), "ignoring environment.{} (expected string)", key.str());
          continue;
        }
        if (!isEnvironmentVariableName(key.str())) {
          warnAt(key.source(), R"(ignoring environment key "{}" (expected [A-Za-z_][A-Za-z0-9_]*))", key.str());
          continue;
        }
        if (std::ranges::find(kReservedEnvironmentNames, key.str()) != kReservedEnvironmentNames.end()) {
          warnAt(key.source(), "ignoring environment.{} (reserved by Umbriel)", key.str());
          continue;
        }
        if (entry->contains('\0')) {
          warnAt(value.source(), "ignoring environment.{} (value contains NUL)", key.str());
          continue;
        }
        parsed.emplace_back(std::string(key.str()), *entry);
      }
      environment.variables = std::move(parsed);
    }

    const registry::Fields<Config::Events>& eventFields() {
      using E = Config::Events;
      static const registry::Fields<E> fields{
          registry::text("lid_close", &E::lidClose),
          registry::text("lid_open", &E::lidOpen),
      };
      return fields;
    }

  } // namespace

  registry::Field<Config> colorsTable() { return registry::table("colors", &Config::colors, colorFields()); }

  registry::Field<Config> appearanceTable() {
    return registry::table("appearance", &Config::appearance, appearanceFields());
  }

  registry::Field<Config> overviewTable() { return registry::table("overview", &Config::overview, overviewFields()); }

  registry::Field<Config> hotCornersTable() {
    return registry::table("hot_corners", &Config::hotCorners, hotCornerFields());
  }

  registry::Field<Config> generalTable() { return registry::table("general", &Config::general, generalFields()); }

  registry::Field<Config> environmentTable() {
    return registry::map<Config>(
        "environment", registry::KeyDescription("string"),
        [](Section& s, Config& c, registry::ReadContext&) { readEnvironmentVariables(s, c.environment); }, {}, "table"
    );
  }

  registry::Field<Config> eventsTable() { return registry::table("events", &Config::events, eventFields()); }

  registry::Field<Config> workspaceSettingsTable() {
    return registry::table("workspaces", &Config::workspaces, workspaceSettingFields());
  }

  registry::Field<Config> screencastTable() {
    return registry::table("screencast", &Config::screenCast, screenCastFields());
  }

} // namespace umbriel
