#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  // Prefix-free shortcut labels over keys in favorite-first order.
  // The first labels are single favorite keys. When count exceeds the key set,
  // the least-favorite keys are consumed as prefixes for longer labels.
  // Fewer than two keys yields an empty result.
  std::vector<std::string> shortcutLabels(size_t count, std::string_view keys);

} // namespace umbriel
