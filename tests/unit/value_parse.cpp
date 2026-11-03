#include "config/value_parse.h"

#include "check.h"

#include <array>

UMBRIEL_TEST(parsesSixDigitColorAsOpaque) {
  std::array<float, 4> color{};
  CHECK(umbriel::parseColor("#ff8000", color));
  CHECK_NEAR(color[0], 1.0F, 0.001F);
  CHECK_NEAR(color[1], 0.50196F, 0.001F);
  CHECK_NEAR(color[2], 0.0F, 0.001F);
  CHECK_NEAR(color[3], 1.0F, 0.001F);
}

UMBRIEL_TEST(parsesEightDigitColorWithAlpha) {
  std::array<float, 4> color{};
  CHECK(umbriel::parseColor("#00000080", color));
  CHECK_NEAR(color[0], 0.0F, 0.001F);
  CHECK_NEAR(color[3], 0.50196F, 0.001F);
}

UMBRIEL_TEST(colorHexIsCaseInsensitive) {
  std::array<float, 4> lower{};
  std::array<float, 4> upper{};
  CHECK(umbriel::parseColor("#abcdef", lower));
  CHECK(umbriel::parseColor("#ABCDEF", upper));
  CHECK(lower == upper);
}

UMBRIEL_TEST(rejectsMalformedColors) {
  std::array<float, 4> color{0.25F, 0.25F, 0.25F, 0.25F};
  const std::array<float, 4> before = color;

  CHECK(!umbriel::parseColor("", color));
  CHECK(!umbriel::parseColor("#", color));
  CHECK(!umbriel::parseColor("ff8000", color));     // no leading '#'
  CHECK(!umbriel::parseColor("#fff", color));       // 3-digit shorthand is not supported
  CHECK(!umbriel::parseColor("#ffff", color));      // 4-digit shorthand is not supported
  CHECK(!umbriel::parseColor("#ff80", color));      // wrong length
  CHECK(!umbriel::parseColor("#ff800", color));     // wrong length
  CHECK(!umbriel::parseColor("#gg8000", color));    // non-hex digits
  CHECK(!umbriel::parseColor("#ff80000", color));   // wrong length
  CHECK(!umbriel::parseColor("#ff8000000", color)); // wrong length

  // A rejected parse must not disturb the caller's value.
  CHECK(color == before);
}

UMBRIEL_TEST(parsesOutputModeWithoutRefresh) {
  umbriel::OutputMode mode{};
  CHECK(umbriel::parseOutputMode("1920x1080", mode));
  CHECK_EQ(mode.width, 1920);
  CHECK_EQ(mode.height, 1080);
  CHECK_EQ(mode.refreshMHz, 0);
}

UMBRIEL_TEST(parsesOutputModeWithRefresh) {
  umbriel::OutputMode mode{};
  CHECK(umbriel::parseOutputMode("2560x1440@165", mode));
  CHECK_EQ(mode.width, 2560);
  CHECK_EQ(mode.height, 1440);
  CHECK_EQ(mode.refreshMHz, 165000);
}

UMBRIEL_TEST(parsesFractionalRefresh) {
  umbriel::OutputMode mode{};
  CHECK(umbriel::parseOutputMode("1920x1080@59.951", mode));
  CHECK_EQ(mode.refreshMHz, 59951);
}

UMBRIEL_TEST(clampsOutOfRangeOutputMode) {
  umbriel::OutputMode mode{};
  CHECK(umbriel::parseOutputMode("0x0", mode));
  CHECK_EQ(mode.width, 1);
  CHECK_EQ(mode.height, 1);

  CHECK(umbriel::parseOutputMode("99999x99999@5000", mode));
  CHECK_EQ(mode.width, 16384);
  CHECK_EQ(mode.height, 16384);
  CHECK_EQ(mode.refreshMHz, 1000000);
}

UMBRIEL_TEST(rejectsMalformedOutputModes) {
  umbriel::OutputMode mode{};
  CHECK(!umbriel::parseOutputMode("", mode));
  CHECK(!umbriel::parseOutputMode("1920", mode));       // no separator
  CHECK(!umbriel::parseOutputMode("x1080", mode));      // empty width
  CHECK(!umbriel::parseOutputMode("1920x", mode));      // empty height
  CHECK(!umbriel::parseOutputMode("1920x1080@", mode)); // empty refresh
  CHECK(!umbriel::parseOutputMode("axb", mode));        // non-numeric
  CHECK(!umbriel::parseOutputMode("1920x1080@abc", mode));
  CHECK(!umbriel::parseOutputMode("1920x1080p", mode)); // trailing garbage
  CHECK(!umbriel::parseOutputMode("1920 x 1080", mode));
}

UMBRIEL_TEST(validatesEnvironmentVariableNames) {
  CHECK(umbriel::isEnvironmentVariableName("DXVK_HDR"));
  CHECK(umbriel::isEnvironmentVariableName("_PRIVATE"));
  CHECK(umbriel::isEnvironmentVariableName("A1"));

  CHECK(!umbriel::isEnvironmentVariableName(""));
  CHECK(!umbriel::isEnvironmentVariableName("1STARTS_WITH_DIGIT"));
  CHECK(!umbriel::isEnvironmentVariableName("HAS-HYPHEN"));
  CHECK(!umbriel::isEnvironmentVariableName("HAS.DOT"));
  CHECK(!umbriel::isEnvironmentVariableName("NÄME"));
}

int main() { return RUN_TESTS(); }
