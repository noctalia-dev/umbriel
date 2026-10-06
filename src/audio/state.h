#pragma once

#include "audio/protocol.h"

#include <array>
#include <optional>

namespace umbriel::audio {

  inline constexpr uint64_t kReadyDeadlineNs = 1'000'000'000;
  inline constexpr uint64_t kHeartbeatNs = 100'000'000;
  inline constexpr uint64_t kStaleNs = 250'000'000;
  inline constexpr uint64_t kFadeNs = 150'000'000;
  inline constexpr uint64_t kFutureToleranceNs = 10'000'000;
  inline constexpr uint64_t kEofGraceNs = 100'000'000;
  inline constexpr uint64_t kTermGraceNs = 400'000'000;
  inline constexpr size_t kSourceReceiveBudget = 16;
  inline constexpr size_t kDispatchReceiveBudget = 64;
  inline constexpr size_t kMaxDemandedSources = 4;

  struct Input {
    bool available = false;
    Features features;
    bool operator==(const Input&) const = default;
    [[nodiscard]] std::array<float, 4> levels() const {
      return {available ? 1.0F : 0.0F, features.rms, features.peak, features.envelope};
    }
  };

  enum class ReceiveResult { Accepted, Rejected, Flood };

  // Deterministic receiver. Inspection is const and never creates demand.
  // Timers/event-loop ownership belong to the service, not this pure model.
  class Receiver {
  public:
    void begin(uint64_t epoch, SourceType source, uint64_t nowNs);
    void clear();
    [[nodiscard]] ReceiveResult receive(std::span<const uint8_t> bytes, uint64_t nowNs);
    void disconnected(uint64_t nowNs);
    void advance(uint64_t nowNs);
    [[nodiscard]] bool ready() const { return m_ready; }
    [[nodiscard]] bool readyExpired(uint64_t nowNs) const;
    [[nodiscard]] bool transportExpired(uint64_t nowNs) const;
    [[nodiscard]] uint64_t deadlineNs(uint64_t nowNs) const;
    [[nodiscard]] bool decaying() const { return m_decaying; }
    [[nodiscard]] const Input& input() const { return m_input; }
    [[nodiscard]] uint64_t revision() const { return m_revision; }
    [[nodiscard]] uint64_t epoch() const { return m_epoch; }
    [[nodiscard]] uint64_t generation() const { return m_generation; }
    [[nodiscard]] uint64_t observationNs() const { return m_observationNs; }
    [[nodiscard]] uint64_t sequence() const { return m_sequence; }

  private:
    void publish(Input input);
    void generationChanged(uint64_t generation);
    uint64_t m_epoch = 0;
    SourceType m_source = SourceType::Synthetic;
    bool m_ready = false;
    bool m_decaying = false;
    uint64_t m_startNs = 0;
    uint64_t m_transportNs = 0;
    uint64_t m_generation = 0;
    uint64_t m_sequence = 0;
    uint64_t m_observationNs = 0;
    uint64_t m_fadeStartNs = 0;
    uint64_t m_revision = 0;
    uint64_t m_rateNs = 0;
    double m_tokens = 16;
    size_t m_floods = 0;
    Input m_input;
    Features m_fadeFrom;
  };

  // Success-only revision consumption. A failed display/capture submission
  // retries the same latched input; updates never mutate an in-flight frame.
  class InputLatch {
  public:
    [[nodiscard]] const Input& latch(const Receiver& receiver, bool frozen = false, bool injected = false);
    void submitted(bool success);
    // Session/lease teardown abandons a composition, unlike an ordinary failed
    // submit. Its old input must not leak into the next frame after unlock.
    void cancel();
    // Drop a fixture transaction while retaining what the output actually
    // presented, so an equal real input does not demand a no-op commit.
    void discard();
    [[nodiscard]] const Input& input() const { return m_input; }
    [[nodiscard]] const Input& presented() const { return m_presented; }
    [[nodiscard]] bool pending() const { return m_pending; }
    [[nodiscard]] bool dirty(const Receiver& receiver) const { return m_pending || receiver.input() != m_presented; }
    [[nodiscard]] uint64_t revision() const { return m_revision; }
    [[nodiscard]] uint64_t consumedRevision() const { return m_consumed; }

  private:
    bool m_initialized = false;
    bool m_pending = false;
    Input m_input;
    Input m_presented;
    uint64_t m_revision = 0;
    uint64_t m_consumed = 0;
  };

  // Crash budget is per source and persists across failed launch attempts.
  // Removing demand cancels retry immediately; explicit reset clears failures.
  class RetryPolicy {
  public:
    void setDemand(bool demand);
    void reset();
    void failed(uint64_t nowNs);
    [[nodiscard]] std::optional<uint64_t> deadlineNs() const { return m_deadline; }
    [[nodiscard]] bool exhausted() const { return m_exhausted; }

  private:
    bool m_demand = false;
    bool m_exhausted = false;
    std::array<uint64_t, 5> m_failures{};
    size_t m_count = 0;
    std::optional<uint64_t> m_deadline;
  };

} // namespace umbriel::audio
