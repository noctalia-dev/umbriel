#include "check.h"
#include "output/gamma_transition.h"

#include <array>
#include <cmath>
#include <vector>

using umbriel::GammaOutputMode;
using umbriel::GammaTransitionOps;
using umbriel::hdrWhitePointFromGamma;
using umbriel::stageGammaForOutputMode;

namespace {

  std::vector<uint16_t> gammaTable(size_t size, const std::array<uint16_t, 3>& endpoints) {
    std::vector<uint16_t> table(size * 3);
    for (size_t channel = 0; channel < endpoints.size(); ++channel) {
      for (size_t i = 0; i < size; ++i) {
        table[channel * size + i] = static_cast<uint16_t>(static_cast<uint64_t>(endpoints[channel]) * i / (size - 1));
      }
    }
    return table;
  }

  void checkWhitePoint(const std::array<float, 3>& actual, const std::array<float, 3>& expected) {
    for (size_t channel = 0; channel < actual.size(); ++channel) {
      CHECK_NEAR(actual[channel], expected[channel], 0.000001F);
    }
  }

  struct Recorder {
    int clears = 0;
    int applies = 0;
    bool applyResult = true;

    GammaTransitionOps ops() {
      return GammaTransitionOps{
          .clear = [this] { ++clears; },
          .apply =
              [this] {
                ++applies;
                return applyResult;
              },
      };
    }
  };

} // namespace

UMBRIEL_TEST(hdrClearsGammaWithoutApplyingIt) {
  Recorder recorder;

  CHECK(stageGammaForOutputMode(GammaOutputMode::Hdr, recorder.ops()));
  CHECK_EQ(recorder.clears, 1);
  CHECK_EQ(recorder.applies, 0);
}

UMBRIEL_TEST(sdrAppliesLatestGammaWithoutClearingIt) {
  Recorder recorder;

  CHECK(stageGammaForOutputMode(GammaOutputMode::Sdr, recorder.ops()));
  CHECK_EQ(recorder.clears, 0);
  CHECK_EQ(recorder.applies, 1);
}

UMBRIEL_TEST(sdrClearsGammaWhenPreparingTheTableFails) {
  Recorder recorder;
  recorder.applyResult = false;

  CHECK(!stageGammaForOutputMode(GammaOutputMode::Sdr, recorder.ops()));
  CHECK_EQ(recorder.clears, 1);
  CHECK_EQ(recorder.applies, 1);
}

UMBRIEL_TEST(hdrWhitePointDefaultsToNeutralWithoutATable) {
  checkWhitePoint(hdrWhitePointFromGamma(nullptr, 256), {1.0F, 1.0F, 1.0F});
  const std::array<uint16_t, 3> table{0, 0, 0};
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 0), {1.0F, 1.0F, 1.0F});
}

UMBRIEL_TEST(hdrWhitePointRecognizesInclusiveIdentity) {
  const auto table = gammaTable(256, {65535, 65535, 65535});
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 256), {1.0F, 1.0F, 1.0F});
}

UMBRIEL_TEST(hdrWhitePointRecognizesSteppedIdentityAcrossAllChannels) {
  constexpr size_t size = 257;
  std::vector<uint16_t> table(size * 3);
  for (size_t channel = 0; channel < 3; ++channel) {
    for (size_t i = 0; i < size; ++i) {
      table[channel * size + i] = static_cast<uint16_t>(65536 * i / size);
    }
  }
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), size), {1.0F, 1.0F, 1.0F});
}

UMBRIEL_TEST(hdrWhitePointAllowsSingleUnitIdentityRoundingPerChannel) {
  constexpr size_t size = 256;
  auto table = gammaTable(size, {65535, 65535, 40000});
  for (size_t i = 0; i < size; ++i) {
    if (i > 0) {
      --table[i];
    }
    const auto steppedSample = static_cast<uint16_t>(65536 * i / size);
    table[size + i] = static_cast<uint16_t>(steppedSample + (i % 2 == 0 ? 0 : 1));
  }
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), size), {1.0F, 1.0F, 40000.0F / 65535.0F});
}

UMBRIEL_TEST(hdrWhitePointRejectsIdentityWhenOneInteriorEntryChanges) {
  constexpr size_t size = 256;
  std::vector<uint16_t> table(size * 3);
  for (size_t channel = 0; channel < 3; ++channel) {
    for (size_t i = 0; i < size; ++i) {
      table[channel * size + i] = static_cast<uint16_t>(65536 * i / size);
    }
  }
  table[size + 137] += 2;
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), size), {1.0F, 65280.0F / 65535.0F, 1.0F});
}

UMBRIEL_TEST(hdrWhitePointKeepsEncodedWarmChannelEndpoints) {
  const auto table = gammaTable(256, {65535, 50000, 30000});
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 256), {1.0F, 50000.0F / 65535.0F, 30000.0F / 65535.0F});
}

UMBRIEL_TEST(hdrWhitePointPreservesNeutralDimming) {
  const auto table = gammaTable(256, {32768, 32768, 32768});
  const float endpoint = 32768.0F / 65535.0F;
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 256), {endpoint, endpoint, endpoint});
}

UMBRIEL_TEST(hdrWhitePointPreservesBlack) {
  const auto table = gammaTable(256, {0, 0, 0});
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 256), {0.0F, 0.0F, 0.0F});
}

UMBRIEL_TEST(hdrWhitePointIgnoresCurvatureWithoutSnappingACustomEndpoint) {
  constexpr size_t size = 256;
  std::vector<uint16_t> table(size * 3);
  for (size_t channel = 0; channel < 3; ++channel) {
    for (size_t i = 0; i < size; ++i) {
      table[channel * size + i] = static_cast<uint16_t>(65280 * i * i / ((size - 1) * (size - 1)));
    }
  }
  const float endpoint = 65280.0F / 65535.0F;
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), size), {endpoint, endpoint, endpoint});
}

UMBRIEL_TEST(hdrWhitePointUsesSingleEntryDirectly) {
  const std::array<uint16_t, 3> table{65535, 32768, 0};
  checkWhitePoint(hdrWhitePointFromGamma(table.data(), 1), {1.0F, 32768.0F / 65535.0F, 0.0F});
}

int main() { return RUN_TESTS(); }
