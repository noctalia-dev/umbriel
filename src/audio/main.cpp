// Playback analyser. Reports over the IPC socket on stdin, or over $UMBRIEL_SOCKET when stdin is not one.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <print>
#include <spa/param/audio/format-utils.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  using Clock = std::chrono::steady_clock;

  constexpr auto kStaleAfter = std::chrono::milliseconds(150);

  int connectToUmbriel() {
    const char* env = std::getenv("UMBRIEL_SOCKET");
    const std::string_view path = env != nullptr ? env : "";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return -1;
    }
    path.copy(addr.sun_path, path.size());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  struct Analyser {
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    int ipcFd = STDIN_FILENO;
    bool failed = false;
    bool formatValid = false;
    double power = 0;
    size_t samples = 0;
    float level = 0;
    float held = 0;
    Clock::time_point lastData = Clock::now();
    Clock::time_point lastTick = Clock::now();

    // One bounded request/acknowledgement at a time. A blocked compositor never
    // leaves recording running indefinitely; EOF also ends capture on shutdown.
    bool publish() const {
      const float encoded = std::round(level * 65535.0F) / 65535.0F;
      const std::string message =
          nlohmann::json{{"cmd", "effect-audio"}, {"version", 1}, {"level", encoded}}.dump() + '\n';
      const auto deadline = Clock::now() + std::chrono::milliseconds(150);
      size_t offset = 0;
      std::string reply;
      while (Clock::now() < deadline) {
        pollfd fd{.fd = ipcFd, .events = static_cast<short>(offset < message.size() ? POLLOUT : POLLIN), .revents = 0};
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (poll(&fd, 1, static_cast<int>(std::max<int64_t>(remaining, 0))) <= 0
            || (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          return false;
        }
        if (offset < message.size()) {
          const auto count = send(ipcFd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
          if (count <= 0) {
            return false;
          }
          offset += static_cast<size_t>(count);
        } else {
          std::array<char, 256> buffer{};
          const auto count = recv(ipcFd, buffer.data(), buffer.size(), MSG_DONTWAIT);
          if (count <= 0) {
            return false;
          }
          reply.append(buffer.data(), static_cast<size_t>(count));
          if (reply.size() > buffer.size()) {
            return false;
          }
          if (reply.ends_with('\n')) {
            const auto response = nlohmann::json::parse(reply, nullptr, false);
            return response.is_object() && response.contains("ok") && response["ok"] == true;
          }
        }
      }
      return false;
    }

    static void process(void* data) {
      auto& self = *static_cast<Analyser*>(data);
      pw_buffer* buffer = pw_stream_dequeue_buffer(self.stream);
      if (buffer == nullptr) {
        return;
      }
      if (self.formatValid && buffer->buffer->n_datas > 0) {
        const auto& block = buffer->buffer->datas[0];
        if (block.data != nullptr
            && block.chunk != nullptr
            && block.chunk->offset <= block.maxsize
            && block.chunk->size <= block.maxsize - block.chunk->offset) {
          const auto* bytes = static_cast<const char*>(block.data) + block.chunk->offset;
          for (size_t i = 0; i + sizeof(float) <= block.chunk->size; i += sizeof(float)) {
            float value = 0;
            std::memcpy(&value, bytes + i, sizeof(value));
            if (std::isfinite(value)) {
              // Average channel power, not samples: anti-phase stereo stays audible.
              const double bounded = std::clamp(value, -1.0F, 1.0F);
              self.power += bounded * bounded;
              ++self.samples;
            }
          }
        }
      }
      pw_stream_queue_buffer(self.stream, buffer);
    }

    static void format(void* data, uint32_t id, const spa_pod* param) {
      auto& self = *static_cast<Analyser*>(data);
      if (id != SPA_PARAM_Format) {
        return;
      }
      spa_audio_info_raw info{};
      self.formatValid = param != nullptr
          && spa_format_audio_raw_parse(param, &info) >= 0
          && info.format == SPA_AUDIO_FORMAT_F32
          && info.channels > 0;
      self.power = 0;
      self.samples = 0;
      self.level = 0;
      self.held = 0;
    }

    static void state(void* data, pw_stream_state /*old*/, pw_stream_state current, const char* error) {
      auto& self = *static_cast<Analyser*>(data);
      if (current == PW_STREAM_STATE_ERROR || current == PW_STREAM_STATE_UNCONNECTED) {
        if (error != nullptr) {
          std::println(stderr, "umbriel-audio: {}", error);
        }
        self.failed = true;
        pw_main_loop_quit(self.loop);
      }
    }

    static void tick(void* data, uint64_t /*expirations*/) {
      auto& self = *static_cast<Analyser*>(data);
      const auto now = Clock::now();
      const float elapsed = std::chrono::duration<float>(now - self.lastTick).count();
      self.lastTick = now;
      // A fixed gain with a square-root curve keeps quiet playback visible without automatic gain
      // amplifying noise during silence. 10 ms attack, 150 ms release.
      if (self.samples > 0) {
        self.held = std::sqrt(std::min(1.0, 6.0 * std::sqrt(self.power / self.samples)));
        self.lastData = now;
      } else if (now - self.lastData > kStaleAfter) {
        // PipeWire can deliver less often than a tick, so one empty tick is not silence.
        self.held = 0;
      }
      const float target = self.held;
      self.power = 0;
      self.samples = 0;
      self.level += (target - self.level) * (1.0F - std::exp(-elapsed / (target > self.level ? 0.01F : 0.15F)));
      if (!self.publish()) {
        pw_main_loop_quit(self.loop);
      }
    }
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 1) {
    std::println(stderr, "usage: umbriel-audio (takes no arguments)");
    return 1;
  }
  int type = 0;
  socklen_t size = sizeof(type);
  int ipcFd = STDIN_FILENO;
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM) {
    ipcFd = connectToUmbriel();
    if (ipcFd < 0) {
      std::println(stderr, "umbriel-audio: cannot connect to $UMBRIEL_SOCKET");
      return 1;
    }
  }
  pw_init(&argc, &argv);
  Analyser analyser;
  analyser.ipcFd = ipcFd;
  analyser.loop = pw_main_loop_new(nullptr);
  if (analyser.loop == nullptr) {
    return 1;
  }
  const pw_stream_events events = [] {
    pw_stream_events result{};
    result.version = PW_VERSION_STREAM_EVENTS;
    result.state_changed = Analyser::state;
    result.param_changed = Analyser::format;
    result.process = Analyser::process;
    return result;
  }();
  auto* loop = pw_main_loop_get_loop(analyser.loop);
  analyser.stream = pw_stream_new_simple(
      loop, "umbriel-audio",
      pw_properties_new(
          PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_APP_NAME, "Umbriel Audio",
          PW_KEY_STREAM_CAPTURE_SINK, "true", PW_KEY_NODE_PASSIVE, "true", nullptr
      ),
      &events, &analyser
  );
  auto* timer = pw_loop_add_timer(loop, Analyser::tick, &analyser);
  int result = 1;
  if (analyser.stream != nullptr && timer != nullptr) {
    std::array<uint8_t, 512> storage{};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = 48000;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    // No RT_PROCESS: analysis and IPC run on this helper's loop, never an RT callback.
    if (pw_stream_connect(
            analyser.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1
        )
        >= 0) {
      timespec interval{.tv_sec = 0, .tv_nsec = 20'000'000};
      pw_loop_update_timer(loop, timer, &interval, &interval, false);
      pw_main_loop_run(analyser.loop);
      result = analyser.failed ? 1 : 0;
    }
  }
  if (timer != nullptr) {
    pw_loop_destroy_source(loop, timer);
  }
  if (analyser.stream != nullptr) {
    pw_stream_destroy(analyser.stream);
  }
  pw_main_loop_destroy(analyser.loop);
  pw_deinit();
  return result;
}
