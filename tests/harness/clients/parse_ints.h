#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace harness {

  // Exactly N integers joined by `separator`, with nothing else in `text`.
  template <typename T, size_t N>
  std::optional<std::array<T, N>> parseInts(std::string_view text, char separator, int base = 10) {
    std::array<T, N> values{};
    for (size_t i = 0; i < N; ++i) {
      const char* const begin = text.data();
      const auto [end, error] = std::from_chars(begin, begin + text.size(), values[i], base);
      if (error != std::errc{}) {
        return std::nullopt;
      }
      text.remove_prefix(static_cast<size_t>(end - begin));
      if (i + 1 < N) {
        if (text.empty() || text.front() != separator) {
          return std::nullopt;
        }
        text.remove_prefix(1);
      }
    }
    if (!text.empty()) {
      return std::nullopt;
    }
    return values;
  }

} // namespace harness
