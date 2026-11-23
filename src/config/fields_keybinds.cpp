// [keybinds], and the warning for binds the mouse scroll button takes over.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/store.h"
#include "input/global_shortcuts.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

// clang-format off
#include <xkbcommon/xkbcommon.h>
// clang-format on

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    // The keys a keybind written as a table takes besides its chord.
    struct KeybindOptions {
      std::optional<std::string> action;
      std::optional<std::string> release;
      bool repeat = true;
      bool allowWhenLocked = false;
      bool allowWhenInhibited = false;
      int cooldownMs = 0;
      std::string submap;
    };

    const registry::Fields<KeybindOptions>& keybindFields() {
      using K = KeybindOptions;
      static const registry::Fields<K> fields{
          // Missing or non-string actions are reported against the whole bind.
          registry::custom<K>(
              "action", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string&, K& target, registry::ReadContext&) {
                target.action = node.value<std::string>();
              }
          ),
          registry::custom<K>(
              "release", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string&, K& target, registry::ReadContext&) {
                target.release = node.value<std::string>();
              }
          ),
          registry::boolean("repeat", &K::repeat),
          registry::boolean("allow_when_locked", &K::allowWhenLocked),
          registry::boolean("allow_when_inhibited", &K::allowWhenInhibited),
          registry::integer("cooldown_ms", 0, 3600000, &K::cooldownMs),
          registry::text("submap", &K::submap),
      };
      return fields;
    }

    void readKeybinds(Section& section, Config& loaded, registry::ReadContext& context) {
      std::vector<Keybind> configured;
      auto sameChord = [](const Keybind& left, const Keybind& right) {
        return left.submap == right.submap
            && left.modifiers == right.modifiers
            && left.useMod == right.useMod
            && left.modifierOnly == right.modifierOnly
            && left.keysym == right.keysym
            && left.wheel == right.wheel
            && left.mouseButton == right.mouseButton;
      };
      for (const auto& [key, entry] : section.table()) {
        const std::string chord(key.str());
        KeybindOptions options;
        bool hasSubmapAfter = false;

        if (const auto* tbl = entry.as_table()) {
          Section bind(*tbl, "keybinds." + chord, configStore().mutableDiagnostics());
          registry::readFields(bind, keybindFields(), options, context);
          const toml::node* submapNode = bind.node("submap");
          hasSubmapAfter = submapNode != nullptr && submapNode->is_string();
          bool invalidAction = false;
          for (const std::string_view field : {"action", "release"}) {
            if (const toml::node* node = bind.node(field); node != nullptr && !node->is_string()) {
              warnAt(node->source(), "ignoring keybind '{}' ('{}' must be a string)", chord, field);
              invalidAction = true;
            }
          }
          if (invalidAction) {
            continue;
          }
          if (!options.action && !options.release) {
            warnAt(entry.source(), "ignoring keybind '{}' (table needs an 'action' or 'release' string)", chord);
            continue;
          }
        } else {
          options.action = entry.value<std::string>();
          if (!options.action) {
            warnAt(entry.source(), "ignoring keybind '{}' (expected string or table)", chord);
            continue;
          }
        }

        if (hasSubmapAfter && !validSubmapName(options.submap)) {
          warnAt(
              entry.source(),
              "ignoring keybind '{}' (submap must be a non-empty name without ']' and may not be 'disable')", chord
          );
          continue;
        }

        Keybind binding;
        if (!parseChord(chord, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' (bad chord)", chord);
          continue;
        }
        if (options.release && (binding.wheel != WheelDirection::None || binding.modifierOnly)) {
          warnAt(key.source(), "ignoring keybind '{}' (release requires a keyboard key or mouse button)", chord);
          continue;
        }
        if (hasSubmapAfter) {
          binding.submapAfter = SubmapArg{.name = std::move(options.submap)};
        }
        binding.repeat = options.repeat && !binding.modifierOnly && !binding.submapAfter.has_value();
        binding.allowWhenLocked = options.allowWhenLocked;
        binding.allowWhenInhibited = options.allowWhenInhibited;
        binding.cooldownMs = options.cooldownMs;
        // Validate both phases before replacing an inherited entry.
        std::vector<Keybind> phases;
        auto addPhase = [&](const std::string& action, bool onRelease) {
          Keybind phase = binding;
          phase.onRelease = onRelease;
          phase.repeat = phase.repeat && !onRelease;
          if (!parseAction(action, phase)) {
            warnAt(key.source(), "ignoring keybind '{}' (unknown action '{}')", chord, action);
            return false;
          }
          if (onRelease && phase.action == KeybindAction::LayoutScrollDrag) {
            warnAt(key.source(), "ignoring keybind '{}' (layout-scroll-drag requires a press binding)", chord);
            return false;
          }
          if (const auto invalid =
                  scratchpadSelectorError(loaded, phase).or_else([&] { return effectActionError(loaded, phase); })) {
            warnAt(key.source(), "ignoring keybind '{}' ({})", chord, *invalid);
            return false;
          }
          phases.push_back(std::move(phase));
          return true;
        };
        if ((options.action && !addPhase(*options.action, false))
            || (options.release && !addPhase(*options.release, true))) {
          continue;
        }

        if (std::ranges::any_of(configured, [&](const Keybind& existing) { return sameChord(existing, binding); })) {
          warnAt(key.source(), "duplicate keybind {}", chord);
        }
        std::erase_if(configured, [&](const Keybind& existing) { return sameChord(existing, binding); });
        configured.push_back(binding);
        std::erase_if(loaded.keybinds, [&](const Keybind& existing) { return sameChord(existing, binding); });
        for (auto& phase : phases) {
          loaded.keybinds.push_back(std::move(phase));
        }
      }
    }

  } // namespace

  registry::Field<Config> keybindsTable() {
    return registry::map<Config>(
        "keybinds", registry::KeyDescription("string_or_table").withFormat("action"), readKeybinds, [] {
          registry::Descriptions keys;
          registry::describeFields(keybindFields(), KeybindOptions{}, "", keys);
          return keys;
        }()
    );
  }

  registry::Field<Config> globalShortcutsTable() {
    using registry::KeyDescription;
    return registry::handRead<Config>(
        "global_shortcuts", KeyDescription("map"),
        [] {
          registry::Descriptions keys{KeyDescription("map"), KeyDescription("string").withFormat("chord")};
          keys[0].path = ".<name>";
          keys[1].path = ".<name>.<name>";
          return keys;
        }(),
        [](Section& root, Config& loaded, registry::ReadContext&) {
          const auto* node = root.take("global_shortcuts");
          if (node == nullptr) {
            return;
          }
          const auto* applications = node->as_table();
          if (applications == nullptr) {
            root.error(*node, "global_shortcuts must be a table of application tables");
            return;
          }
          for (const auto& [application, entries] : *applications) {
            const std::string path = std::format("global_shortcuts.{}", application.str());
            if (!validGlobalShortcutId(application.str())) {
              errorAt(application.source(), "{} requires a non-empty application ID of at most 256 bytes", path);
              continue;
            }
            const auto* shortcuts = entries.as_table();
            if (shortcuts == nullptr) {
              errorAt(entries.source(), "{} must be a table of shortcut IDs", path);
              continue;
            }
            for (const auto& [id, value] : *shortcuts) {
              const std::string key = std::format("{}.{}", path, id.str());
              if (!validGlobalShortcutId(id.str())) {
                errorAt(id.source(), "{} requires a non-empty shortcut ID of at most 256 bytes", key);
                continue;
              }
              const auto chord = value.value<std::string>();
              // The server resolves symbolic Mod; any supported mask validates its syntax here.
              if (!chord) {
                errorAt(value.source(), "{} must be a string", key);
              } else if (!chord->empty() && !parseGlobalShortcutOverride(*chord, WLR_MODIFIER_LOGO)) {
                errorAt(value.source(), "{} must be a keyboard chord or an empty string to disable it", key);
              } else {
                loaded.globalShortcuts[std::string(application.str())][std::string(id.str())] = *chord;
              }
            }
          }
        }
    );
  }

  // libinput swallows the scroll button while it turns motion into scrolling, but a press released without any
  // motion still reaches the compositor as a click, so a bind on that button fires only in that case.
  void warnScrollButtonBinds(const Config& loaded) {
    const auto report = [&loaded](std::optional<uint32_t> button, std::string_view context) {
      if (!button) {
        return;
      }
      if (std::ranges::none_of(loaded.keybinds, [&](const Keybind& bind) { return bind.mouseButton == *button; })) {
        return;
      }
      const char* name = mouseButtonName(*button);
      warnNoSrc(
          "{} claims {} for scrolling, so binds on it fire only when it is released without motion", context,
          name != nullptr ? name : "it"
      );
    };
    report(loaded.input.mouse.scrollButton, "input.mouse.scroll_button");
    for (const Config::Input::Device& device : loaded.input.devices) {
      report(device.scrollButton, "input.device.scroll_button");
    }
  }

} // namespace umbriel
