#pragma once

#include "audio/profile.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace umbriel::audio {

  // Analysis generations change on a complete replacement measurement. Wire
  // status keeps the last successfully sent generation even if a newer queued
  // measurement was coalesced away, so the host can fade its held input.
  class Acquisition {
  public:
    void invalidate(uint64_t nowNs) {
      m_analysis.reset();
      m_pending.reset();
      m_available = false;
      m_afterNs = std::max(m_afterNs, nowNs);
      m_observationNs = 0;
    }

    [[nodiscard]] bool
    ingest(size_t channels, std::span<const float> samples, uint64_t cycleNs, uint64_t nowNs, uint64_t epoch) {
      // Old buffers queued before source/link replacement cannot establish a
      // replacement generation. Dropping them must not move the barrier again.
      if (cycleNs <= m_afterNs) {
        return false;
      }
      if (cycleNs < m_observationNs
          || channels == 0
          || channels > kMaxChannels
          || (m_analysis && channels != m_channels)) {
        invalidate(nowNs);
        return false;
      }
      if (!m_analysis) {
        m_channels = channels;
        m_analysis = std::make_unique<AnalysisStream>(channels);
        m_base = m_generation + 1;
      }
      if (!m_analysis->ingest(samples, nowNs)) {
        invalidate(nowNs);
        return false;
      }
      m_observationNs = cycleNs;
      while (auto snapshot = m_analysis->next(epoch)) {
        snapshot->generation += m_base - 1;
        m_generation = snapshot->generation;
        m_pending = *snapshot;
        m_available = true;
      }
      return true;
    }

    void published(uint64_t generation) { m_publishedGeneration = std::max(m_publishedGeneration, generation); }
    [[nodiscard]] uint64_t publishedGeneration() const { return m_publishedGeneration; }
    [[nodiscard]] bool available() const { return m_available; }
    [[nodiscard]] uint64_t generation() const { return m_generation; }
    [[nodiscard]] bool pending() const { return m_pending.has_value(); }
    [[nodiscard]] std::optional<Snapshot> take() { return std::exchange(m_pending, std::nullopt); }

  private:
    uint64_t m_generation = 1;
    uint64_t m_publishedGeneration = 1;
    uint64_t m_base = 1;
    uint64_t m_afterNs = 0;
    uint64_t m_observationNs = 0;
    bool m_available = false;
    size_t m_channels = 0;
    std::unique_ptr<AnalysisStream> m_analysis;
    std::optional<Snapshot> m_pending;
  };

} // namespace umbriel::audio
