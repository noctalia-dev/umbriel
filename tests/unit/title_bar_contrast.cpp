#include "scene/title_bar_contrast.h"

#include "check.h"

#include <array>

namespace {
  constexpr std::array<float, 4> colour(float red, float green, float blue, float alpha = 1.0F) {
    return {red, green, blue, alpha};
  }
} // namespace

UMBRIEL_TEST(luminanceRanksColoursByPerceivedBrightness) {
  CHECK(umbriel::luminance(colour(0.0F, 0.0F, 0.0F)) < umbriel::luminance(colour(0.5F, 0.5F, 0.5F)));
  CHECK(umbriel::luminance(colour(0.5F, 0.5F, 0.5F)) < umbriel::luminance(colour(1.0F, 1.0F, 1.0F)));
  // Green reads brighter than blue at the same intensity, which is why the weights are not equal.
  CHECK(umbriel::luminance(colour(0.0F, 1.0F, 0.0F)) > umbriel::luminance(colour(0.0F, 0.0F, 1.0F)));
}

UMBRIEL_TEST(contrastAdjustedKeepsAColourThatAlreadyReads) {
  // Light text on a dark bar is the ordinary case: it survives untouched, alpha included.
  const auto light = colour(0.93F, 0.93F, 0.95F, 0.8F);
  CHECK(umbriel::contrastAdjusted(light, colour(0.16F, 0.16F, 0.2F)) == light);
  const auto dark = colour(0.1F, 0.1F, 0.12F, 1.0F);
  CHECK(umbriel::contrastAdjusted(dark, colour(0.9F, 0.9F, 0.9F)) == dark);
}

UMBRIEL_TEST(contrastAdjustedReversesIntoALightBar) {
  // Dark text on a bar the window painted dark: the title flips to white, keeping its own alpha.
  const auto text = colour(0.15F, 0.15F, 0.15F, 0.75F);
  const auto adjusted = umbriel::contrastAdjusted(text, colour(0.2F, 0.2F, 0.25F));
  CHECK_EQ(adjusted[0], 1.0F);
  CHECK_EQ(adjusted[1], 1.0F);
  CHECK_EQ(adjusted[2], 1.0F);
  CHECK_EQ(adjusted[3], 0.75F);
}

UMBRIEL_TEST(contrastAdjustedReversesIntoADarkBar) {
  const auto text = colour(0.85F, 0.85F, 0.85F, 1.0F);
  const auto adjusted = umbriel::contrastAdjusted(text, colour(0.8F, 0.82F, 0.8F));
  CHECK_EQ(adjusted[0], 0.0F);
  CHECK_EQ(adjusted[1], 0.0F);
  CHECK_EQ(adjusted[2], 0.0F);
  CHECK_EQ(adjusted[3], 1.0F);
}

UMBRIEL_TEST(contrastAdjustedFlipsOnlyBelowTheThreshold) {
  // Just under the threshold reverses; just above it the configured colour stands. The boundary itself is not pinned:
  // both sides of it are computed luminances, so no case sits exactly on it.
  const auto text = colour(0.0F, 0.0F, 0.0F, 1.0F);
  const float justUnder = umbriel::kTitleContrastThreshold - 0.01F;
  const float justOver = umbriel::kTitleContrastThreshold + 0.01F;
  const auto reversed = umbriel::contrastAdjusted(text, colour(justUnder, justUnder, justUnder));
  CHECK_EQ(reversed[0], 1.0F);
  CHECK(umbriel::contrastAdjusted(text, colour(justOver, justOver, justOver)) == text);
}

UMBRIEL_TEST(contrastAdjustedLeavesAnInvisibleTitleAlone) {
  // An alpha of zero is how a palette hides the title. Reversing it would leave a bright colour behind a transparent
  // composite, which the renderer cannot mask away.
  const auto hidden = colour(0.1F, 0.1F, 0.1F, 0.0F);
  CHECK(umbriel::contrastAdjusted(hidden, colour(0.1F, 0.1F, 0.1F)) == hidden);
}

UMBRIEL_TEST(contrastAdjustedLeavesATitleAloneOnNoBackground) {
  // A bar with no alpha is not what shows, so its colour says nothing about what the title sits on.
  const auto text = colour(0.1F, 0.1F, 0.1F, 1.0F);
  CHECK(umbriel::contrastAdjusted(text, colour(0.1F, 0.1F, 0.1F, 0.0F)) == text);
}

int main() { return RUN_TESTS(); }
