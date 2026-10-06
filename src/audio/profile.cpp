#include "audio/profile.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace umbriel::audio {

  namespace {
    // In-place radix-2 unnormalized forward FFT. A fixed 2048-point transform
    // avoids a runtime dependency and has bounded O(N log N) work/storage.
    void fft(std::array<std::complex<double>, kWindowFrames>& values) {
      for (size_t i = 1, j = 0; i < values.size(); ++i) {
        size_t bit = values.size() >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
          j ^= bit;
        }
        j ^= bit;
        if (i < j) {
          std::swap(values[i], values[j]);
        }
      }
      for (size_t length = 2; length <= values.size(); length <<= 1) {
        const auto step = std::polar(1.0, -2 * std::numbers::pi / static_cast<double>(length));
        for (size_t start = 0; start < values.size(); start += length) {
          std::complex<double> factor(1, 0);
          for (size_t i = 0; i < length / 2; ++i) {
            const auto a = values[start + i];
            const auto b = values[start + i + length / 2] * factor;
            values[start + i] = a + b;
            values[start + i + length / 2] = a - b;
            factor *= step;
          }
        }
      }
    }
  } // namespace

  float quantize(double amplitude) {
    if (!std::isfinite(amplitude)) {
      return 0;
    }
    return static_cast<float>(std::round(std::clamp(amplitude, 0.0, 1.0) * 65535) / 65535);
  }

  std::optional<Features> Linear16::analyze(std::span<const float> interleaved, size_t channels, double dtSeconds) {
    if (channels == 0
        || channels > kMaxChannels
        || interleaved.size() != channels * kWindowFrames
        || !std::isfinite(dtSeconds)
        || dtSeconds < 0
        || !std::ranges::all_of(interleaved, [](float sample) { return std::isfinite(sample); })) {
      return std::nullopt;
    }
    std::array<double, kWindowFrames / 2 + 1> powers{};
    std::array<std::complex<double>, kWindowFrames> values{};
    double total = 0;
    double peak = 0;
    double windowPower = 0;
    for (size_t channel = 0; channel < channels; ++channel) {
      for (size_t n = 0; n < kWindowFrames; ++n) {
        const double sample = std::clamp(static_cast<double>(interleaved[n * channels + channel]), -1.0, 1.0);
        const double window = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * n / kWindowFrames);
        total += sample * sample;
        peak = std::max(peak, std::abs(sample));
        if (channel == 0) {
          windowPower += window * window;
        }
        values[n] = sample * window;
      }
      fft(values);
      for (size_t k = 0; k < powers.size(); ++k) {
        const double factor = k == 0 || k == kWindowFrames / 2 ? 1 : 2;
        powers[k] += factor * std::norm(values[k]) / (kWindowFrames * windowPower * channels);
      }
    }
    const double rms = std::sqrt(total / interleaved.size());
    const double tau = rms > m_envelope ? 0.010 : 0.150;
    m_envelope = rms + (m_envelope - rms) * std::exp(-dtSeconds / tau);
    if (m_envelope < 1.0 / 65535) {
      m_envelope = 0;
    }
    Features features{.rms = quantize(rms), .peak = quantize(peak), .envelope = quantize(m_envelope), .bands = {}};
    constexpr double binWidth = static_cast<double>(kSampleRate) / kWindowFrames;
    for (size_t band = 0; band < features.bands.size(); ++band) {
      double power = 0;
      for (size_t k = 0; k < powers.size(); ++k) {
        const double left = std::max(0.0, (static_cast<double>(k) - 0.5) * binWidth);
        const double right = std::min(kSampleRate / 2.0, (static_cast<double>(k) + 0.5) * binWidth);
        const double overlap = std::max(0.0, std::min(right, kBandEdges[band + 1]) - std::max(left, kBandEdges[band]));
        power += powers[k] * overlap / (right - left);
      }
      features.bands[band] = quantize(std::sqrt(power));
    }
    return features;
  }

  AnalysisStream::AnalysisStream(size_t channels) : m_channels(channels) {}

  bool AnalysisStream::ingest(std::span<const float> interleaved, uint64_t observationNs) {
    if (m_channels == 0
        || m_channels > kMaxChannels
        || interleaved.empty()
        || interleaved.size() % m_channels != 0
        || observationNs == 0
        || observationNs < m_lastIngestNs
        || !std::ranges::all_of(interleaved, [](float sample) { return std::isfinite(sample); })) {
      return false;
    }
    size_t frames = interleaved.size() / m_channels;
    size_t skip = 0;
    if (frames > kRingFrames - m_count) {
      ++m_generation;
      m_sequence = 0;
      m_analysis.reset();
      const size_t excess = frames - (kRingFrames - m_count);
      const size_t drop = std::min(excess, m_count);
      m_head = (m_head + drop) % kRingFrames;
      m_count -= drop;
      skip = excess - drop;
    }
    for (size_t i = skip; i < frames; ++i) {
      const size_t frame = (m_head + m_count) % kRingFrames;
      for (size_t c = 0; c < m_channels; ++c) {
        m_samples[frame * m_channels + c] = std::clamp(interleaved[i * m_channels + c], -1.0F, 1.0F);
      }
      m_times[frame] = observationNs;
      ++m_count;
    }
    m_lastIngestNs = observationNs;
    return true;
  }

  std::optional<Snapshot> AnalysisStream::next(uint64_t epoch) {
    if (m_count < kWindowFrames || epoch == 0) {
      return std::nullopt;
    }
    for (size_t i = 0; i < kWindowFrames; ++i) {
      const size_t frame = (m_head + i) % kRingFrames;
      std::copy_n(m_samples.begin() + frame * m_channels, m_channels, m_window.begin() + i * m_channels);
    }
    const uint64_t observation = m_times[(m_head + kWindowFrames - 1) % kRingFrames];
    const auto features = m_analysis.analyze(
        std::span(m_window.data(), kWindowFrames * m_channels), m_channels,
        static_cast<double>(m_sequence == 0 ? kWindowFrames : kHopFrames) / kSampleRate
    );
    m_head = (m_head + kHopFrames) % kRingFrames;
    m_count -= kHopFrames;
    return Snapshot{
        .epoch = epoch,
        .generation = m_generation,
        .sequence = ++m_sequence,
        .observationNs = observation,
        .features = *features
    };
  }

} // namespace umbriel::audio
