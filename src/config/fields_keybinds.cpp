// [keybinds], and the warning for binds the mouse scroll button takes over.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/store.h"

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
      bool repeat = true;
      bool allowWhenLocked = false;
      bool allowWhenInhibited = false;
      int cooldownMs = 0;
      std::string submap;
    };

    const registry::Fields<KeybindOptions>& keybindFields() {
      using K = KeybindOptions;
      static const registry::Fields<K> fields{
          // A missing or non-string action is reported against the whole bind.
          registry::custom<K>(
              "action", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string&, K& target, registry::ReadContext&) {
                target.action = node.value<std::string>();
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
          if (!options.action) {
            warnAt(entry.source(), "ignoring keybind '{}' (table needs an 'action' string)", chord);
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
          if (binding.keysym != XKB_KEY_NoSymbol && binding.modifiers == 0 && !binding.useMod) {
            warnAt(key.source(), "ignoring keybind '{}' (needs at least one modifier)", chord);
          } else {
            warnAt(key.source(), "ignoring keybind '{}' (bad chord)", chord);
          }
          continue;
        }
        if (hasSubmapAfter) {
          binding.submapAfter = SubmapArg{.name = std::move(options.submap)};
        }
        binding.repeat = options.repeat && !binding.modifierOnly && !binding.submapAfter.has_value();
        binding.allowWhenLocked = options.allowWhenLocked;
        binding.allowWhenInhibited = options.allowWhenInhibited;
        binding.cooldownMs = options.cooldownMs;
        if (!parseAction(*options.action, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' (unknown action '{}')", chord, *options.action);
          continue;
        }

        if (const auto invalid = scratchpadSelectorError(loaded, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' ({})", chord, *invalid);
          continue;
        }

        if (std::ranges::any_of(configured, [&](const Keybind& existing) { return sameChord(existing, binding); })) {
          warnAt(key.source(), "duplicate keybind {}", chord);
        }
        std::erase_if(configured, [&](const Keybind& existing) { return sameChord(existing, binding); });
        configured.push_back(binding);
        std::erase_if(loaded.keybinds, [&](const Keybind& existing) { return sameChord(existing, binding); });
        loaded.keybinds.push_back(std::move(binding));
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
