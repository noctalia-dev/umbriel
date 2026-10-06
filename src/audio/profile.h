#pragma once

#include "audio/protocol.h"

#include <array>
#include <optional>
#include <span>

namespace umbriel::audio {

  inline constexpr size_t kSampleRate = 48000;
  inline constexpr size_t kWindowFrames = 2048;
  inline constexpr size_t kHopFrames = 800;
  inline constexpr size_t kRingFrames = 4096;
  inline constexpr size_t kMaxChannels = 8;
  inline constexpr std::array<double, 17> kBandEdges{20,   40,   80,   120,  180,  270,   400,   600,  900,
                                                     1350, 2000, 3000, 4500, 6750, 10000, 15000, 20000};

  [[nodiscard]] float quantize(double amplitude);

  // The helper owns analysis; the compositor only consumes snapshots. Input to
  // this core is already resampled to 48 kHz. No FFT dependency or device I/O.
  class Linear16 {
  public:
    // Reject a whole window on invalid channel count, sample count, nonfinite
    // samples or invalid analysis dt. Finite samples are clamped to [-1,1].
    [[nodiscard]] std::optional<Features>
    analyze(std::span<const float> interleaved, size_t channels, double dtSeconds);
    void reset() { m_envelope = 0; }

  private:
    double m_envelope = 0;
  };

  // Bounded ingest queue. Overflow retains the newest 4096 frames, increments
  // generation and resets envelope/sequence. Timestamp belongs to ingestion of
  // the sample completing a window, never to the later drain/send call.
  class AnalysisStream {
  public:
    explicit AnalysisStream(size_t channels = 1);
    [[nodiscard]] bool ingest(std::span<const float> interleaved, uint64_t observationNs);
    [[nodiscard]] std::optional<Snapshot> next(uint64_t epoch);
    [[nodiscard]] uint64_t generation() const { return m_generation; }
    [[nodiscard]] size_t queuedFrames() const { return m_count; }

  private:
    size_t m_channels;
    std::array<float, kRingFrames * kMaxChannels> m_samples{};
    std::array<uint64_t, kRingFrames> m_times{};
    std::array<float, kWindowFrames * kMaxChannels> m_window{};
    size_t m_head = 0;
    size_t m_count = 0;
    uint64_t m_generation = 1;
    uint64_t m_sequence = 0;
    uint64_t m_lastIngestNs = 0;
    Linear16 m_analysis;
  };

} // namespace umbriel::audio
