#pragma once

#include "config/config.h"

#include <optional>
#include <string>
#include <string_view>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  std::optional<xkb_layout_index_t> layoutGroupNamed(xkb_keymap* keymap, std::string_view name);

  // Find the target group with the same XKB layout name as sourceGroup.
  // Nullopt keeps deliberately different per-device layout vocabularies apart.
  std::optional<xkb_layout_index_t>
  matchingLayoutGroup(xkb_keymap* target, xkb_keymap* source, xkb_layout_index_t sourceGroup);

  // XKB rule names a keymap is compiled from. An empty field leaves it to libxkbcommon's default.
  struct XkbNames {
    std::string layout;
    std::string variant;
    std::string options;
    std::string model;
    bool empty() const { return layout.empty() && variant.empty() && options.empty() && model.empty(); }
    bool operator==(const XkbNames&) const = default;
  };

  // A device rule's XKB keys over [input.keyboard]. When that leaves all four empty, the system keymap applies.
  XkbNames
  resolveXkbNames(const Config::Input::Keyboard& keyboard, const Config::Input::Device* device, const XkbNames& system);
  // Whether any keyboard, with or without a device rule, resolves to the system keymap.
  bool usesSystemXkbNames(const Config::Input& input);

} // namespace umbriel
