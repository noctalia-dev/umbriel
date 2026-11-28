#include "scene/title_bar_contrast.h"

#include "check.h"

#include <array>

namespace {
  constexpr std::array<float, 4> color(float red, float green, float blue, float alpha = 1.0F) {
    return {red, green, blue, alpha};
  }
} // namespace

UMBRIEL_TEST(luminanceRanksColoursByPerceivedBrightness) {
  CHECK(umbriel::luminance(color(0.0F, 0.0F, 0.0F)) < umbriel::luminance(color(0.5F, 0.5F, 0.5F)));
  CHECK(umbriel::luminance(color(0.5F, 0.5F, 0.5F)) < umbriel::luminance(color(1.0F, 1.0F, 1.0F)));
  // Green reads brighter than blue at the same intensity, which is why the weights are not equal.
  CHECK(umbriel::luminance(color(0.0F, 1.0F, 0.0F)) > umbriel::luminance(color(0.0F, 0.0F, 1.0F)));
}

UMBRIEL_TEST(contrastAdjustedKeepsAColourThatAlreadyReads) {
  // Light text on a dark bar is the ordinary case: it survives untouched, alpha included.
  const auto light = color(0.93F, 0.93F, 0.95F, 0.8F);
  CHECK(umbriel::contrastAdjusted(light, color(0.16F, 0.16F, 0.2F)) == light);
  const auto dark = color(0.1F, 0.1F, 0.12F, 1.0F);
  CHECK(umbriel::contrastAdjusted(dark, color(0.9F, 0.9F, 0.9F)) == dark);
}

UMBRIEL_TEST(contrastAdjustedReversesIntoALightBar) {
  // Dark text on a bar the window painted dark: the title flips to white, keeping its own alpha.
  const auto text = color(0.15F, 0.15F, 0.15F, 0.75F);
  const auto adjusted = umbriel::contrastAdjusted(text, color(0.2F, 0.2F, 0.25F));
  CHECK_EQ(adjusted[0], 1.0F);
  CHECK_EQ(adjusted[1], 1.0F);
  CHECK_EQ(adjusted[2], 1.0F);
  CHECK_EQ(adjusted[3], 0.75F);
}

UMBRIEL_TEST(contrastAdjustedReversesIntoADarkBar) {
  const auto text = color(0.85F, 0.85F, 0.85F, 1.0F);
  const auto adjusted = umbriel::contrastAdjusted(text, color(0.8F, 0.82F, 0.8F));
  CHECK_EQ(adjusted[0], 0.0F);
  CHECK_EQ(adjusted[1], 0.0F);
  CHECK_EQ(adjusted[2], 0.0F);
  CHECK_EQ(adjusted[3], 1.0F);
}

UMBRIEL_TEST(contrastAdjustedFlipsOnlyBelowTheThreshold) {
  // Just under the threshold reverses; just above it the configured color stands. The boundary itself is not pinned:
  // both sides of it are computed luminances, so no case sits exactly on it.
  const auto text = color(0.0F, 0.0F, 0.0F, 1.0F);
  const float justUnder = umbriel::kTitleContrastThreshold - 0.01F;
  const float justOver = umbriel::kTitleContrastThreshold + 0.01F;
  const auto reversed = umbriel::contrastAdjusted(text, color(justUnder, justUnder, justUnder));
  CHECK_EQ(reversed[0], 1.0F);
  CHECK(umbriel::contrastAdjusted(text, color(justOver, justOver, justOver)) == text);
}

UMBRIEL_TEST(contrastAdjustedLeavesAnInvisibleTitleAlone) {
  // An alpha of zero is how a palette hides the title. Reversing it would leave a bright color behind a transparent
  // composite, which the renderer cannot mask away.
  const auto hidden = color(0.1F, 0.1F, 0.1F, 0.0F);
  CHECK(umbriel::contrastAdjusted(hidden, color(0.1F, 0.1F, 0.1F)) == hidden);
}

UMBRIEL_TEST(contrastAdjustedLeavesATitleAloneOnNoBackground) {
  // A bar with no alpha is not what shows, so its color says nothing about what the title sits on.
  const auto text = color(0.1F, 0.1F, 0.1F, 1.0F);
  CHECK(umbriel::contrastAdjusted(text, color(0.1F, 0.1F, 0.1F, 0.0F)) == text);
}

UMBRIEL_TEST(asShownBarColorKeepsAnOpaqueFillAtFullOpacity) {
  // The focused ordinary case: an opaque fill is what shows, whatever the palette behind it would have said.
  const auto fill = color(0.2F, 0.35F, 0.8F);
  CHECK(umbriel::asShownBarColor(fill, 1.0F, color(0.9F, 0.1F, 0.1F)) == fill);
}

UMBRIEL_TEST(asShownBarColorBlendsATranslucentFillWithThePalette) {
  // A translucent edge drawn over a white palette reads grey, not the edge's straight black.
  const auto shown = umbriel::asShownBarColor(color(0.0F, 0.0F, 0.0F, 0.5F), 1.0F, color(1.0F, 1.0F, 1.0F));
  CHECK_EQ(shown[0], 0.5F);
  CHECK_EQ(shown[1], 0.5F);
  CHECK_EQ(shown[2], 0.5F);
  CHECK_EQ(shown[3], 1.0F);
}

UMBRIEL_TEST(asShownBarColorDimsAnInactiveFillOverTheMask) {
  // A half-strength dim over a mask of the same tone lands the composite exactly between the two.
  const auto shown = umbriel::asShownBarColor(color(0.25F, 0.25F, 0.25F), 0.5F, color(0.5F, 0.5F, 0.5F));
  CHECK_EQ(shown[0], 0.375F);
  CHECK_EQ(shown[3], 1.0F);
}

UMBRIEL_TEST(asShownBarColorLeavesThePaletteOverItself) {
  // An edge read as empty shows the palette alone; compositing the palette over itself must not move it at all. The
  // general formula would inflate the coverage alpha, which then corrupts a translucent button's own composite.
  const auto palette = color(0.4F, 0.4F, 0.45F, 0.8F);
  CHECK(umbriel::asShownBarColor(palette, 0.65F, palette) == palette);
  CHECK(umbriel::asShownBarColor(palette, 1.0F, palette) == palette);
}

UMBRIEL_TEST(contrastAdjustedJudgesTheInactiveBarAsShown) {
  // The regression: the dim mask carries the title's own color, so the bar as shown sinks the title even though the
  // raw edge reads fine against it. The verdict must come from the composite.
  const auto text = color(0.5F, 0.5F, 0.5F);
  const auto edge = color(0.14F, 0.14F, 0.14F);
  const auto mask = color(0.5F, 0.5F, 0.5F);
  CHECK(umbriel::contrastAdjusted(text, edge) == text); // the raw edge reads fine: the old, wrong verdict
  const auto adjusted = umbriel::contrastAdjusted(text, umbriel::asShownBarColor(edge, 0.65F, mask));
  CHECK(adjusted != text);
  CHECK_EQ(adjusted[0], 1.0F);
  CHECK_EQ(adjusted[1], 1.0F);
  CHECK_EQ(adjusted[2], 1.0F);
}

UMBRIEL_TEST(contrastAdjustedJudgesAPaintedButtonAtItsOpacity) {
  // A disabled button is painted at 0.35, so a black one on a white bar reads as light grey: a 0.6 grey symbol is
  // kept against the raw black and vanishes on screen. The verdict must follow the painted opacity.
  const auto glyph = color(0.6F, 0.6F, 0.6F);
  const auto button = color(0.0F, 0.0F, 0.0F);
  CHECK(umbriel::contrastAdjusted(glyph, button) == glyph); // reads on the raw color: the old, wrong verdict
  const auto adjusted =
      umbriel::contrastAdjusted(glyph, umbriel::asShownBarColor(button, 0.35F, color(1.0F, 1.0F, 1.0F)));
  CHECK(adjusted != glyph);
  CHECK_EQ(adjusted[0], 0.0F);
  CHECK_EQ(adjusted[1], 0.0F);
  CHECK_EQ(adjusted[2], 0.0F);
}

int main() { return RUN_TESTS(); }
