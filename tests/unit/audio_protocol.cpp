#include "audio/protocol.h"
#include "check.h"

#include <limits>

using namespace umbriel::audio;

UMBRIEL_TEST(snapshotWireBytesAreIndependentOfStructLayout) {
  Snapshot snapshot{
      .epoch = 0x0807060504030201ULL,
      .generation = 9,
      .sequence = 10,
      .observationNs = 11,
      .features = {.rms = 0.5F, .peak = 1, .envelope = 0.25F, .bands = {}}
  };
  snapshot.features.bands[15] = 1;
  const auto bytes = encode(snapshot);
  CHECK_EQ(bytes.size(), 124U);
  const std::vector<uint8_t> prefix{'U', 'A', 'F', '1', 1, 0, 3, 0, 124, 0, 1, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(std::equal(prefix.begin(), prefix.end(), bytes.begin()));
  CHECK_EQ(bytes[50], 0);
  CHECK_EQ(bytes[51], 0x3f);
  CHECK_EQ(bytes[54], 0x80);
  CHECK_EQ(bytes[55], 0x3f);
  CHECK_EQ(bytes[122], 0x80);
  CHECK_EQ(bytes[123], 0x3f);
  const auto parsed = decode(bytes);
  CHECK(parsed.has_value());
  CHECK_EQ(std::get<Snapshot>(*parsed), snapshot);
}

UMBRIEL_TEST(configurationNegotiatesSourceTypeAndSelector) {
  for (SourceType source : {SourceType::Playback, SourceType::Microphone}) {
    Configuration config{.epoch = 7, .source = source, .selector = Selector::Fixed, .target = "target-\xc3\xa9"};
    const auto bytes = encode(config);
    CHECK_EQ(std::get<Configuration>(*decode(bytes)), config);
    CHECK_EQ(bytes.size(), 41U);
    CHECK_EQ(bytes[30], 0);
    CHECK_EQ(bytes[31], 0);
    config.target.clear();
    CHECK(encode(config).empty());
    config.selector = Selector::FollowDefault;
    CHECK_EQ(encode(config).size(), 32U);
    config.target = "ambiguous";
    CHECK(encode(config).empty());
  }
  Configuration config{
      .epoch = 1, .source = SourceType::Playback, .selector = Selector::Fixed, .target = std::string(1024, 'a')
  };
  CHECK_EQ(encode(config).size(), kMaxConfigurationBytes);
  config.target.push_back('a');
  CHECK(encode(config).empty());
  for (const std::string& target :
       {std::string("bad\0name", 8), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
        std::string("\xf4\x90\x80\x80"), std::string("\xc3")}) {
    config.target = target;
    CHECK(encode(config).empty());
  }
}

UMBRIEL_TEST(malformedLengthsHeadersFloatsAndReservedBytesReject) {
  const auto good = encode(Snapshot{.epoch = 1});
  for (size_t length = 0; length < good.size(); ++length) {
    CHECK(!decode(std::span(good.data(), length)));
  }
  for (size_t offset : {0U, 4U, 6U, 8U, 10U, 12U}) {
    auto bytes = good;
    bytes[offset] = 255;
    CHECK(!decode(bytes));
  }
  auto bytes = good;
  bytes.push_back(0);
  CHECK(!decode(bytes));
  bytes = good;
  bytes[50] = 0xc0;
  bytes[51] = 0x7f; // NaN, bypassing local encoder validation.
  CHECK(!decode(bytes));
  bytes = good;
  bytes[51] = 0xbf; // Negative amplitude.
  CHECK(!decode(bytes));
  auto ready = encode(Ready{.epoch = 1});
  ready[31] = 1;
  CHECK(!decode(ready));
  auto config = encode(Configuration{.epoch = 1, .target = {}});
  config[30] = 1;
  CHECK(!decode(config));
  CHECK(encode(Snapshot{.epoch = 1, .features = {.rms = std::numeric_limits<float>::infinity()}}).empty());
}

UMBRIEL_TEST(heartbeatAndUnavailableHaveNoMeasurementTimestamp) {
  for (bool unavailable : {false, true}) {
    Status status{.epoch = 15, .generation = 19, .unavailable = unavailable};
    const auto bytes = encode(status);
    CHECK_EQ(bytes.size(), 32U);
    CHECK_EQ(bytes[6], unavailable ? 5 : 4);
    CHECK_EQ(std::get<Status>(*decode(bytes)), status);
  }
}

int main() { return RUN_TESTS(); }
