#include "output/hdr_metadata.h"

#include "check.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace {

  std::array<uint8_t, 256> hdrEdid() {
    std::array<uint8_t, 256> edid{};
    edid[0] = 0x00;
    edid[1] = 0xFF;
    edid[2] = 0xFF;
    edid[3] = 0xFF;
    edid[4] = 0xFF;
    edid[5] = 0xFF;
    edid[6] = 0xFF;
    edid[7] = 0x00;
    edid[8] = 0x4C;
    edid[9] = 0x2D;
    edid[10] = 0x01;
    edid[11] = 0x00;
    edid[18] = 0x01;
    edid[19] = 0x04;
    edid[20] = 0x80;
    edid[21] = 60;
    edid[22] = 34;
    edid[23] = 120;
    edid[24] = 0x0A;
    edid[126] = 1;

    constexpr size_t extension = 128;
    edid[extension] = 0x02;
    edid[extension + 1] = 0x03;
    edid[extension + 2] = 11;
    edid[extension + 4] = 0xE6;
    edid[extension + 5] = 0x06;
    edid[extension + 6] = 0x05;
    edid[extension + 7] = 0x01;
    edid[extension + 8] = 96;
    edid[extension + 9] = 64;
    edid[extension + 10] = 18;

    const auto finishChecksum = [&](size_t offset) {
      uint8_t checksum = 0;
      for (size_t index = 0; index < 127; ++index) {
        checksum = static_cast<uint8_t>(checksum + edid[offset + index]);
      }
      edid[offset + 127] = static_cast<uint8_t>(0 - checksum);
    };
    finishChecksum(0);
    finishChecksum(extension);
    return edid;
  }

} // namespace

UMBRIEL_TEST(edidStaticMetadataPopulatesHdrOutputDescription) {
  const auto edid = hdrEdid();
  const auto metadata = umbriel::hdrStaticMetadataFromEdid(edid);
  CHECK(metadata.has_value());
  if (!metadata) {
    return;
  }

  CHECK_NEAR(metadata->maxLuminance, 400.0, 0.01);
  CHECK_NEAR(metadata->maxFrameAverageLuminance, 200.0, 0.01);
  CHECK(metadata->minLuminance > 0.0);

  const wlr_color_primaries displayPrimaries{
      .red = {.x = 0.68F, .y = 0.32F},
      .green = {.x = 0.265F, .y = 0.69F},
      .blue = {.x = 0.15F, .y = 0.06F},
      .white = {.x = 0.3127F, .y = 0.329F},
  };
  const wlr_output_image_description description = umbriel::makeHdrOutputDescription(&displayPrimaries, metadata);
  CHECK(description.primaries == WLR_COLOR_NAMED_PRIMARIES_BT2020);
  CHECK(description.transfer_function == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
  CHECK_NEAR(description.mastering_luminance.min, metadata->minLuminance, 0.01);
  CHECK_NEAR(description.mastering_luminance.max, 400.0, 0.01);
  CHECK_NEAR(description.max_cll, 400.0, 0.01);
  CHECK_NEAR(description.max_fall, 200.0, 0.01);
  CHECK_NEAR(description.mastering_display_primaries.red.x, displayPrimaries.red.x, 0.01);
  CHECK_NEAR(description.mastering_display_primaries.white.y, displayPrimaries.white.y, 0.01);
}

UMBRIEL_TEST(hdrDescriptionFallsBackToBt2020TargetPrimaries) {
  const wlr_output_image_description description = umbriel::makeHdrOutputDescription(nullptr, std::nullopt);
  wlr_color_primaries bt2020{};
  wlr_color_primaries_from_named(&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);

  CHECK_NEAR(description.mastering_display_primaries.red.x, bt2020.red.x, 0.01);
  CHECK_NEAR(description.mastering_display_primaries.green.y, bt2020.green.y, 0.01);
  CHECK_NEAR(description.mastering_display_primaries.blue.x, bt2020.blue.x, 0.01);
  CHECK_NEAR(description.mastering_display_primaries.white.y, bt2020.white.y, 0.01);
  CHECK_EQ(description.mastering_luminance.max, 0.0);
  CHECK_EQ(description.max_cll, 0.0);
  CHECK_EQ(description.max_fall, 0.0);
}

UMBRIEL_TEST(nonHdrEdidDoesNotProduceStaticMetadata) {
  std::array<uint8_t, 128> edid{};
  CHECK(!umbriel::hdrStaticMetadataFromEdid(edid).has_value());
}

int main() { return RUN_TESTS(); }
