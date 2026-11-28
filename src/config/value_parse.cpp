#include "config/value_parse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace umbriel {

  namespace {

    int hexDigit(char character) {
      if (character >= '0' && character <= '9') {
        return character - '0';
      }
      if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
      }
      if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
      }
      return -1;
    }

  } // namespace

  bool parseColor(std::string_view text, std::array<float, 4>& output) {
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
      return false;
    }
    std::array<float, 4> parsed{0.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t component = 0; component < (text.size() - 1) / 2; ++component) {
      const int high = hexDigit(text[component * 2 + 1]);
      const int low = hexDigit(text[component * 2 + 2]);
      if (high < 0 || low < 0) {
        return false;
      }
      parsed[component] = static_cast<float>(high * 16 + low) / 255.0F;
    }
    output = parsed;
    return true;
  }

  bool parseOutputMode(std::string_view text, OutputMode& output, bool allowKeywords) {
    if (allowKeywords) {
      if (text == "highest_refresh") {
        output.target = OutputModeTarget::HighRefreshRate;
        return true;
      }

      if (text == "highest_resolution") {
        output.target = OutputModeTarget::HighResolution;
        return true;
      }
    }

    const size_t widthEnd = text.find('x');
    if (widthEnd == std::string_view::npos) {
      return false;
    }
    const size_t heightEnd = text.find('@', widthEnd + 1);
    const std::string_view widthText = text.substr(0, widthEnd);
    const std::string_view heightText =
        text.substr(widthEnd + 1, heightEnd == std::string_view::npos ? text.size() : heightEnd - widthEnd - 1);
    if (widthText.empty() || heightText.empty()) {
      return false;
    }

    int width = 0;
    int height = 0;
    const auto [widthPtr, widthError] = std::from_chars(widthText.data(), widthText.data() + widthText.size(), width);
    const auto [heightPtr, heightError] =
        std::from_chars(heightText.data(), heightText.data() + heightText.size(), height);
    if (widthError != std::errc{}
        || widthPtr != widthText.data() + widthText.size()
        || heightError != std::errc{}
        || heightPtr != heightText.data() + heightText.size()) {
      return false;
    }

    double refreshHz = 0.0;
    if (heightEnd != std::string_view::npos) {
      const std::string_view refreshText = text.substr(heightEnd + 1);
      if (refreshText.empty()) {
        return false;
      }
      const auto [refreshPtr, refreshError] =
          std::from_chars(refreshText.data(), refreshText.data() + refreshText.size(), refreshHz);
      if (refreshError != std::errc{}
          || refreshPtr != refreshText.data() + refreshText.size()
          || !std::isfinite(refreshHz)) {
        return false;
      }
    }

    output.width = std::clamp(width, 1, 16384);
    output.height = std::clamp(height, 1, 16384);
    output.refreshMHz = static_cast<int>(std::lround(std::clamp(refreshHz, 0.0, 1000.0) * 1000.0));
    return true;
  }

  bool isEnvironmentVariableName(std::string_view name) {
    const auto asciiLetter = [](char character) {
      return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    };
    if (name.empty() || (!asciiLetter(name.front()) && name.front() != '_')) {
      return false;
    }
    return std::ranges::all_of(name.substr(1), [&](char character) {
      return asciiLetter(character) || (character >= '0' && character <= '9') || character == '_';
    });
  }

} // namespace umbriel
