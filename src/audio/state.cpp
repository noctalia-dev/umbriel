#include "audio/state.h"

#include "audio/profile.h"

#include <algorithm>

namespace umbriel::audio {

  void Receiver::publish(Input input) {
    if (m_input != input) {
      m_input = input;
      ++m_revision;
    }
  }

  void Receiver::clear() {
    const auto nextRevision = m_revision + (m_input != Input{} ? 1 : 0);
    *this = Receiver{};
    m_revision = nextRevision;
  }

  void Receiver::begin(uint64_t epoch, SourceType source, uint64_t nowNs) {
    clear();
    m_epoch = epoch;
    m_source = source;
    m_startNs = nowNs;
    m_transportNs = nowNs;
    m_rateNs = nowNs;
  }

  void Receiver::generationChanged(uint64_t generation) {
    m_generation = generation;
    m_sequence = 0;
    m_observationNs = 0;
    m_decaying = false;
    publish({});
  }

  ReceiveResult Receiver::receive(std::span<const uint8_t> bytes, uint64_t nowNs) {
    if (m_epoch == 0 || nowNs < m_rateNs) {
      return ReceiveResult::Rejected;
    }
    m_tokens = std::min(16.0, m_tokens + static_cast<double>(nowNs - m_rateNs) * 120 / 1e9);
    m_rateNs = nowNs;
    if (m_tokens < 1) {
      return ++m_floods >= 3 ? ReceiveResult::Flood : ReceiveResult::Rejected;
    }
    m_tokens -= 1;
    const auto packet = decode(bytes);
    if (!packet) {
      return ReceiveResult::Rejected;
    }
    if (const auto* ready = std::get_if<Ready>(&*packet)) {
      if (m_ready || ready->epoch != m_epoch || ready->source != m_source || readyExpired(nowNs)) {
        return ReceiveResult::Rejected;
      }
      m_ready = true;
      m_transportNs = nowNs;
      return ReceiveResult::Accepted;
    }
    if (!m_ready) {
      return ReceiveResult::Rejected;
    }
    if (const auto* status = std::get_if<Status>(&*packet)) {
      if (status->epoch != m_epoch || status->generation < m_generation) {
        return ReceiveResult::Rejected;
      }
      if (status->generation > m_generation) {
        generationChanged(status->generation);
      }
      m_transportNs = nowNs;
      if (status->unavailable) {
        disconnected(nowNs);
      }
      return ReceiveResult::Accepted;
    }
    const auto* snapshot = std::get_if<Snapshot>(&*packet);
    if (snapshot == nullptr
        || snapshot->epoch != m_epoch
        || snapshot->generation == 0
        || snapshot->generation < m_generation
        || snapshot->sequence == 0
        || snapshot->observationNs == 0
        || (snapshot->observationNs > nowNs && snapshot->observationNs - nowNs > kFutureToleranceNs)
        || (nowNs >= snapshot->observationNs && nowNs - snapshot->observationNs >= kStaleNs)
        || (snapshot->generation == m_generation
            && (snapshot->sequence <= m_sequence || snapshot->observationNs <= m_observationNs))) {
      return ReceiveResult::Rejected;
    }
    if (snapshot->generation > m_generation) {
      generationChanged(snapshot->generation);
    }
    m_generation = snapshot->generation;
    m_sequence = snapshot->sequence;
    m_observationNs = snapshot->observationNs;
    m_transportNs = nowNs;
    m_decaying = false;
    publish({.available = true, .features = snapshot->features});
    return ReceiveResult::Accepted;
  }

  void Receiver::disconnected(uint64_t nowNs) {
    if (!m_input.available) {
      return;
    }
    m_fadeFrom = m_input.features;
    m_fadeStartNs = nowNs;
    m_decaying = m_fadeFrom != Features{};
    publish({.available = false, .features = m_fadeFrom});
  }

  void Receiver::advance(uint64_t nowNs) {
    if (m_input.available && nowNs >= m_observationNs && nowNs - m_observationNs >= kStaleNs) {
      disconnected(m_observationNs + kStaleNs);
    }
    if (!m_decaying || nowNs < m_fadeStartNs) {
      return;
    }
    if (nowNs - m_fadeStartNs >= kFadeNs) {
      m_decaying = false;
      publish({});
      return;
    }
    const double factor = 1 - static_cast<double>(nowNs - m_fadeStartNs) / kFadeNs;
    Features features{
        .rms = quantize(m_fadeFrom.rms * factor),
        .peak = quantize(m_fadeFrom.peak * factor),
        .envelope = quantize(m_fadeFrom.envelope * factor),
        .bands = {}
    };
    for (size_t i = 0; i < features.bands.size(); ++i) {
      features.bands[i] = quantize(m_fadeFrom.bands[i] * factor);
    }
    publish({.available = false, .features = features});
  }

  uint64_t Receiver::deadlineNs(uint64_t nowNs) const {
    if (m_epoch == 0) {
      return 0;
    }
    uint64_t deadline = m_ready ? m_transportNs + kReadyDeadlineNs : m_startNs + kReadyDeadlineNs;
    if (m_input.available) {
      deadline = std::min(deadline, m_observationNs + kStaleNs);
    }
    if (m_decaying) {
      deadline = std::min({deadline, m_fadeStartNs + kFadeNs, nowNs + 16'000'000});
    }
    return deadline;
  }

  bool Receiver::readyExpired(uint64_t nowNs) const {
    return m_epoch != 0 && !m_ready && nowNs >= m_startNs && nowNs - m_startNs >= kReadyDeadlineNs;
  }

  bool Receiver::transportExpired(uint64_t nowNs) const {
    return m_epoch != 0 && nowNs >= m_transportNs && nowNs - m_transportNs >= kReadyDeadlineNs;
  }

  const Input& InputLatch::latch(const Receiver& receiver, bool frozen, bool injected) {
    if (m_pending || (m_initialized && frozen && !injected)) {
      return m_input;
    }
    m_input = receiver.input();
    m_revision = receiver.revision();
    m_initialized = true;
    m_pending = m_input != m_presented;
    // A revision whose packed values equal the already submitted values needs
    // no GPU transaction. An empty/no-damage frame cannot pin a stale latch.
    if (!m_pending) {
      m_consumed = m_revision;
    }
    return m_input;
  }

  void InputLatch::submitted(bool success) {
    if (m_pending && success) {
      m_consumed = m_revision;
      m_presented = m_input;
      m_pending = false;
    }
  }

  void InputLatch::cancel() {
    m_pending = false;
    m_initialized = false;
    m_input = {};
    m_presented = {};
    m_revision = 0;
    m_consumed = 0;
  }

  void InputLatch::discard() {
    m_pending = false;
    m_initialized = false;
    m_input = m_presented;
    m_revision = m_consumed;
  }

  void RetryPolicy::setDemand(bool demand) {
    m_demand = demand;
    if (!demand) {
      m_deadline.reset();
    }
  }

  void RetryPolicy::reset() {
    m_count = 0;
    m_exhausted = false;
    m_deadline.reset();
  }

  void RetryPolicy::failed(uint64_t nowNs) {
    m_deadline.reset();
    if (!m_demand || m_exhausted) {
      return;
    }
    size_t kept = 0;
    for (size_t i = 0; i < m_count; ++i) {
      if (nowNs >= m_failures[i] && nowNs - m_failures[i] < 60'000'000'000) {
        m_failures[kept++] = m_failures[i];
      }
    }
    m_count = kept;
    m_failures[m_count++] = nowNs;
    if (m_count == m_failures.size()) {
      m_exhausted = true;
      return;
    }
    const uint64_t delay = std::min<uint64_t>(30, 1ULL << (m_count - 1));
    m_deadline = nowNs + delay * 1'000'000'000;
  }

} // namespace umbriel::audio
