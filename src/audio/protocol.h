#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace umbriel::audio {

  // Internal wire contract, independent of native struct packing. All integers
  // and IEEE-754 binary32 values are little endian. Every reserved byte is zero.
  inline constexpr uint16_t kProtocolVersion = 1;
  inline constexpr uint16_t kLinear16Profile = 1;
  inline constexpr size_t kHeaderBytes = 16;
  inline constexpr size_t kSnapshotBytes = 124;
  inline constexpr size_t kMaxPacketBytes = 256;
  inline constexpr size_t kMaxTargetBytes = 1024;
  inline constexpr size_t kMaxConfigurationBytes = 1056;
  enum class MessageType : uint16_t { Configure = 1, Ready = 2, Snapshot = 3, Heartbeat = 4, Unavailable = 5 };
  enum class SourceType : uint16_t { Playback = 1, Microphone = 2, Synthetic = 3 };
  enum class Selector : uint16_t { Fixed = 1, FollowDefault = 2, Synthetic = 3 };

  struct Features {
    float rms = 0;
    float peak = 0;
    float envelope = 0;
    std::array<float, 16> bands{};
    bool operator==(const Features&) const = default;
  };

  // CONFIGURE: epoch@16, source type@24, selector@26, target length@28,
  // reserved uint16@30, UTF-8 target@32. No terminating NUL.
  struct Configuration {
    uint64_t epoch = 0;
    SourceType source = SourceType::Synthetic;
    Selector selector = Selector::Synthetic;
    std::string target;
    bool operator==(const Configuration&) const = default;
  };
  // READY: epoch@16, source type@24, six reserved bytes@26. READY confirms
  // negotiation, never device availability.
  struct Ready {
    uint64_t epoch = 0;
    SourceType source = SourceType::Synthetic;
    bool operator==(const Ready&) const = default;
  };
  struct Snapshot {
    uint64_t epoch = 0;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    uint64_t observationNs = 0;
    Features features{};
    bool operator==(const Snapshot&) const = default;
  };
  // HEARTBEAT and UNAVAILABLE: epoch@16, generation@24. No observation time:
  // transport activity cannot make an old measurement fresh.
  struct Status {
    uint64_t epoch = 0;
    uint64_t generation = 0;
    bool unavailable = false;
    bool operator==(const Status&) const = default;
  };
  using Packet = std::variant<Configuration, Ready, Snapshot, Status>;

  [[nodiscard]] bool validFeatures(const Features& features);
  // Empty result denotes malformed local data or an unsupported contract.
  [[nodiscard]] std::vector<uint8_t> encode(const Packet& packet);
  [[nodiscard]] std::optional<Packet> decode(std::span<const uint8_t> bytes);

} // namespace umbriel::audio
