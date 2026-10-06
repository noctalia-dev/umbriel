// Internal deterministic protocol fixture. Deliberately not installed as a
// real-device provider. Its only inherited channel is stdin, never a shell.
#include "audio/profile.h"
#include "audio/state.h"
#include "audio/transport.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <numbers>
#include <poll.h>
#include <string_view>
#include <unistd.h>

namespace {
  uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
} // namespace

int main(int argc, char** argv) {
  using namespace umbriel::audio;
  bool silence = false;
  bool modulate = false;
  bool externalTest = false;
  bool ignoreEof = false;
  bool beforeReady = false;
  bool heartbeatOnly = false;
  const char* eofMarker = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--silence") {
      silence = true;
    } else if (std::string_view(argv[i]) == "--modulate") {
      modulate = true;
    } else if (std::string_view(argv[i]) == "--external-test") {
      externalTest = true;
    } else if (std::string_view(argv[i]) == "--ignore-eof") {
      ignoreEof = true;
    } else if (std::string_view(argv[i]) == "--chatter-before-ready") {
      beforeReady = true;
    } else if (std::string_view(argv[i]) == "--heartbeat-only") {
      heartbeatOnly = true;
    } else if (std::string_view(argv[i]) == "--eof-marker" && i + 1 < argc) {
      eofMarker = argv[++i];
    } else {
      return 2;
    }
  }
  const auto stopped = [eofMarker] {
    if (eofMarker != nullptr) {
      const int fd = open(eofMarker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (fd >= 0) {
        const char value[] = "EOF\n";
        (void)write(fd, value, sizeof(value) - 1);
        close(fd);
      }
    }
    return 0;
  };
  pollfd channel{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
  if (poll(&channel, 1, 1000) <= 0) {
    return 3;
  }
  std::vector<uint8_t> bytes;
  if (receivePacket(channel.fd, bytes, true) != IoResult::Complete) {
    return 4;
  }
  const auto packet = decode(bytes);
  if (!packet || !std::holds_alternative<Configuration>(*packet)) {
    return 5;
  }
  const auto config = std::get<Configuration>(*packet);
  if (!externalTest && (config.source != SourceType::Synthetic || config.selector != Selector::Synthetic)) {
    return 6;
  }
  if (!beforeReady
      && sendPacket(channel.fd, encode(Ready{.epoch = config.epoch, .source = config.source})) != IoResult::Complete) {
    return 7;
  }
  if (beforeReady || heartbeatOnly) {
    if (heartbeatOnly) {
      const Snapshot snapshot{
          .epoch = config.epoch,
          .generation = 1,
          .sequence = 1,
          .observationNs = nowNs(),
          .features = {.rms = 1, .peak = 1, .envelope = 1}
      };
      if (sendPacket(channel.fd, encode(snapshot)) != IoResult::Complete) {
        return 7;
      }
    }
    while (true) {
      if (poll(&channel, 1, 50) != 0) {
        return 0;
      }
      if (sendPacket(channel.fd, encode(Status{.epoch = config.epoch, .generation = 1})) != IoResult::Complete) {
        return 0;
      }
    }
  }
  if (ignoreEof) {
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGTERM, &ignore, nullptr);
    close(STDIN_FILENO);
    while (true) {
      pause();
    }
  }
  AnalysisStream stream;
  CoalescedWriter writer;
  uint64_t frame = 0;
  auto nextAnalysis = nowNs();
  uint64_t lastWrite = nextAnalysis;
  constexpr uint64_t kHopNs = 1'000'000'000ULL * kHopFrames / kSampleRate;
  std::array<float, kHopFrames> samples{};
  while (true) {
    const auto now = nowNs();
    channel.events = POLLIN | (writer.pending() ? POLLOUT : 0);
    const int timeout = now >= nextAnalysis ? 0 : static_cast<int>((nextAnalysis - now + 999'999) / 1'000'000);
    if (poll(&channel, 1, timeout) < 0) {
      return 8;
    }
    if ((channel.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      // Host sends one configuration only. EOF immediately ends acquisition.
      return stopped();
    }
    const auto analysisTime = nowNs();
    if (analysisTime >= nextAnalysis) {
      for (auto& sample : samples) {
        const double amplitude = modulate ? 0.5 + 0.4 * std::sin(2 * std::numbers::pi * frame / kSampleRate) : 0.5;
        sample =
            silence ? 0 : static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * 1500 * frame / kSampleRate));
        ++frame;
      }
      if (!stream.ingest(samples, analysisTime)) {
        return 9;
      }
      // The newest completed window wins, including multiple windows completed
      // by one ingest call. Never publish equal timestamps from a batch.
      while (const auto snapshot = stream.next(config.epoch)) {
        writer.replace(*snapshot);
      }
      // Do not synthesize a backlog if the helper was descheduled.
      nextAnalysis = analysisTime + kHopNs;
    }
    if (!writer.pending() && analysisTime - lastWrite >= kHeartbeatNs) {
      writer.replace(Status{.epoch = config.epoch, .generation = stream.generation()});
    }
    const bool pending = writer.pending();
    const auto result = writer.flush(channel.fd);
    if (result == IoResult::Closed || result == IoResult::Malformed) {
      return stopped();
    }
    if (pending && result == IoResult::Complete) {
      lastWrite = analysisTime;
    }
  }
}
