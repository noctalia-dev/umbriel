#include "audio/protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace umbriel::audio {

  namespace {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

    void put(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
      }
    }

    uint64_t get(std::span<const uint8_t> bytes, size_t offset, size_t count) {
      uint64_t value = 0;
      for (size_t i = 0; i < count; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
      }
      return value;
    }

    bool sourceValid(SourceType source) {
      return source == SourceType::Playback || source == SourceType::Microphone || source == SourceType::Synthetic;
    }

    // Reject NUL, overlong encodings, surrogates and values beyond Unicode.
    bool utf8Valid(std::string_view value) {
      for (size_t i = 0; i < value.size();) {
        auto c = static_cast<uint8_t>(value[i++]);
        if (c == 0) {
          return false;
        }
        if (c < 0x80) {
          continue;
        }
        int extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
        if (extra < 0 || i + extra > value.size()) {
          return false;
        }
        uint32_t code = c & ((1U << (6 - extra)) - 1);
        for (int n = 0; n < extra; ++n) {
          const auto next = static_cast<uint8_t>(value[i++]);
          if ((next & 0xc0) != 0x80) {
            return false;
          }
          code = (code << 6) | (next & 0x3f);
        }
        if ((extra == 1 && code < 0x80)
            || (extra == 2 && code < 0x800)
            || (extra == 3 && code < 0x10000)
            || (code >= 0xd800 && code <= 0xdfff)
            || code > 0x10ffff) {
          return false;
        }
      }
      return true;
    }

    bool configValid(const Configuration& config) {
      if (config.epoch == 0
          || !sourceValid(config.source)
          || config.target.size() > kMaxTargetBytes
          || !utf8Valid(config.target)) {
        return false;
      }
      if (config.source == SourceType::Synthetic) {
        return config.selector == Selector::Synthetic && config.target.empty();
      }
      return (config.selector == Selector::Fixed && !config.target.empty())
          || (config.selector == Selector::FollowDefault && config.target.empty());
    }

    std::vector<uint8_t> header(MessageType type, size_t size) {
      std::vector<uint8_t> bytes(size);
      bytes[0] = 'U';
      bytes[1] = 'A';
      bytes[2] = 'F';
      bytes[3] = '1';
      put(bytes, 4, kProtocolVersion, 2);
      put(bytes, 6, static_cast<uint16_t>(type), 2);
      put(bytes, 8, size, 2);
      put(bytes, 10, kLinear16Profile, 2);
      return bytes;
    }
  } // namespace

  bool validFeatures(const Features& features) {
    const auto valid = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
    return valid(features.rms)
        && valid(features.peak)
        && valid(features.envelope)
        && std::ranges::all_of(features.bands, valid);
  }

  std::vector<uint8_t> encode(const Packet& packet) {
    return std::visit(
        [](const auto& value) -> std::vector<uint8_t> {
          using T = std::decay_t<decltype(value)>;
          if (value.epoch == 0) {
            return {};
          }
          if constexpr (std::is_same_v<T, Configuration>) {
            if (!configValid(value)) {
              return {};
            }
            auto bytes = header(MessageType::Configure, 32 + value.target.size());
            put(bytes, 16, value.epoch, 8);
            put(bytes, 24, static_cast<uint16_t>(value.source), 2);
            put(bytes, 26, static_cast<uint16_t>(value.selector), 2);
            put(bytes, 28, value.target.size(), 2);
            std::copy(value.target.begin(), value.target.end(), bytes.begin() + 32);
            return bytes;
          } else if constexpr (std::is_same_v<T, Ready>) {
            if (!sourceValid(value.source)) {
              return {};
            }
            auto bytes = header(MessageType::Ready, 32);
            put(bytes, 16, value.epoch, 8);
            put(bytes, 24, static_cast<uint16_t>(value.source), 2);
            return bytes;
          } else if constexpr (std::is_same_v<T, Status>) {
            auto bytes = header(value.unavailable ? MessageType::Unavailable : MessageType::Heartbeat, 32);
            put(bytes, 16, value.epoch, 8);
            put(bytes, 24, value.generation, 8);
            return bytes;
          } else {
            if (!validFeatures(value.features)) {
              return {};
            }
            auto bytes = header(MessageType::Snapshot, kSnapshotBytes);
            put(bytes, 16, value.epoch, 8);
            put(bytes, 24, value.generation, 8);
            put(bytes, 32, value.sequence, 8);
            put(bytes, 40, value.observationNs, 8);
            put(bytes, 48, std::bit_cast<uint32_t>(value.features.rms), 4);
            put(bytes, 52, std::bit_cast<uint32_t>(value.features.peak), 4);
            put(bytes, 56, std::bit_cast<uint32_t>(value.features.envelope), 4);
            for (size_t i = 0; i < value.features.bands.size(); ++i) {
              put(bytes, 60 + i * 4, std::bit_cast<uint32_t>(value.features.bands[i]), 4);
            }
            return bytes;
          }
        },
        packet
    );
  }

  std::optional<Packet> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes
        || bytes.size() > kMaxConfigurationBytes
        || bytes[0] != 'U'
        || bytes[1] != 'A'
        || bytes[2] != 'F'
        || bytes[3] != '1'
        || get(bytes, 4, 2) != kProtocolVersion
        || get(bytes, 8, 2) != bytes.size()
        || get(bytes, 10, 2) != kLinear16Profile
        || get(bytes, 12, 4) != 0) {
      return std::nullopt;
    }
    const auto type = static_cast<MessageType>(get(bytes, 6, 2));
    if (type == MessageType::Configure) {
      if (bytes.size() < 32 || get(bytes, 30, 2) != 0 || get(bytes, 28, 2) != bytes.size() - 32) {
        return std::nullopt;
      }
      Configuration config{
          .epoch = get(bytes, 16, 8),
          .source = static_cast<SourceType>(get(bytes, 24, 2)),
          .selector = static_cast<Selector>(get(bytes, 26, 2)),
          .target = std::string(bytes.begin() + 32, bytes.end())
      };
      return configValid(config) ? std::optional<Packet>(config) : std::nullopt;
    }
    if (bytes.size() > kMaxPacketBytes || bytes.size() < 32 || get(bytes, 16, 8) == 0) {
      return std::nullopt;
    }
    if (type == MessageType::Ready && bytes.size() == 32 && get(bytes, 26, 6) == 0) {
      Ready ready{.epoch = get(bytes, 16, 8), .source = static_cast<SourceType>(get(bytes, 24, 2))};
      return sourceValid(ready.source) ? std::optional<Packet>(ready) : std::nullopt;
    }
    if ((type == MessageType::Heartbeat || type == MessageType::Unavailable) && bytes.size() == 32) {
      return Status{
          .epoch = get(bytes, 16, 8), .generation = get(bytes, 24, 8), .unavailable = type == MessageType::Unavailable
      };
    }
    if (type != MessageType::Snapshot || bytes.size() != kSnapshotBytes) {
      return std::nullopt;
    }
    Snapshot snapshot{
        .epoch = get(bytes, 16, 8),
        .generation = get(bytes, 24, 8),
        .sequence = get(bytes, 32, 8),
        .observationNs = get(bytes, 40, 8),
        .features = {}
    };
    const auto number = [&](size_t offset) {
      return std::bit_cast<float>(static_cast<uint32_t>(get(bytes, offset, 4)));
    };
    snapshot.features.rms = number(48);
    snapshot.features.peak = number(52);
    snapshot.features.envelope = number(56);
    for (size_t i = 0; i < snapshot.features.bands.size(); ++i) {
      snapshot.features.bands[i] = number(60 + i * 4);
    }
    return validFeatures(snapshot.features) ? std::optional<Packet>(snapshot) : std::nullopt;
  }

} // namespace umbriel::audio
