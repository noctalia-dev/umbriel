#include "audio/protocol.h"
#include "audio/transport.h"
#include "check.h"

#include <chrono>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace umbriel::audio;

namespace {
  struct Helper {
    pid_t pid = -1;
    int fd = -1;
    Helper() {
      int channel[2];
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) != 0) {
        return;
      }
      pid = fork();
      if (pid == 0) {
        close(channel[0]);
        if (dup2(channel[1], STDIN_FILENO) < 0) {
          _exit(126);
        }
        close(channel[1]);
        // This test must never connect to the developer's PipeWire server.
        const auto remote = "/tmp/umbriel-audio-absent-" + std::to_string(getpid());
        setenv("PIPEWIRE_REMOTE", remote.c_str(), 1);
        execl(UMBRIEL_AUDIO_PIPEWIRE, UMBRIEL_AUDIO_PIPEWIRE, nullptr);
        _exit(127);
      }
      close(channel[1]);
      fd = channel[0];
    }
    ~Helper() {
      if (fd >= 0) {
        close(fd);
      }
      if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
      }
    }
    std::optional<Packet> next() const {
      pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
      if (poll(&descriptor, 1, 1000) <= 0) {
        return std::nullopt;
      }
      std::vector<uint8_t> bytes;
      return receivePacket(fd, bytes) == IoResult::Complete ? decode(bytes) : std::nullopt;
    }
    bool stop() {
      close(fd);
      fd = -1;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
          pid = -1;
          return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        poll(nullptr, 0, 5);
      }
      return false;
    }
  };
} // namespace

UMBRIEL_TEST(absentPipewireNeverFallsBackOrFabricatesSnapshots) {
  for (const auto source : {SourceType::Playback, SourceType::Microphone}) {
    for (const auto selector : {Selector::Fixed, Selector::FollowDefault}) {
      Helper helper;
      CHECK(helper.pid > 0 && helper.fd >= 0);
      if (helper.pid <= 0 || helper.fd < 0) {
        return;
      }
      const Configuration config{
          .epoch = 19,
          .source = source,
          .selector = selector,
          .target = selector == Selector::Fixed ? "exact-absent-device" : ""
      };
      CHECK(sendPacket(helper.fd, encode(config)) == IoResult::Complete);
      const auto ready = helper.next();
      CHECK(ready && std::holds_alternative<Ready>(*ready));
      if (!ready || !std::holds_alternative<Ready>(*ready)) {
        continue;
      }
      CHECK(std::get<Ready>(*ready) == (Ready{.epoch = 19, .source = source}));
      for (int i = 0; i < 3; ++i) {
        const auto status = helper.next();
        CHECK(status && std::holds_alternative<Status>(*status));
        if (status && std::holds_alternative<Status>(*status)) {
          CHECK(std::get<Status>(*status).unavailable);
          CHECK_EQ(std::get<Status>(*status).epoch, uint64_t{19});
          CHECK(std::get<Status>(*status).generation > 0);
        }
      }
      CHECK(helper.stop());
    }
  }
}

int main() { return RUN_TESTS(); }
