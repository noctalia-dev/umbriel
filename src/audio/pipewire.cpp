// Optional helper: all PipeWire acquisition and analysis stays outside the
// compositor. The inherited SOCK_SEQPACKET stdin is the only control channel.
#include "audio/acquisition.h"
#include "audio/transport.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/audio/raw-utils.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
  using namespace umbriel::audio;
  constexpr uint64_t kHeartbeat = 100'000'000;
  constexpr uint64_t kMaxBufferAge = 500'000'000;
  constexpr uint64_t kPublishInterval = (1'000'000'000ULL + 59) / 60;

  uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  std::string property(const spa_dict* properties, const char* name) {
    const char* value = properties != nullptr ? spa_dict_lookup(properties, name) : nullptr;
    return value != nullptr ? value : "";
  }

  std::optional<uint32_t> number(std::string_view value) {
    uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      return std::nullopt;
    }
    return result;
  }

  struct Node {
    std::string name;
    std::string serial;
    std::string mediaClass;
    uint32_t channels = 0;
    std::array<uint32_t, SPA_AUDIO_MAX_CHANNELS> positions{};
    bool positioned = false;
  };
  struct Link {
    uint32_t output = SPA_ID_INVALID;
    uint32_t input = SPA_ID_INVALID;
  };

  class Provider {
  public:
    explicit Provider(Configuration configuration) : config(std::move(configuration)) {}
    ~Provider() {
      disconnectBackend();
      if (loop != nullptr) {
        pw_main_loop_destroy(loop);
      }
    }

    int run() {
      // READY negotiates the protocol, not permission to assume a device exists.
      if (sendPacket(STDIN_FILENO, encode(Ready{.epoch = config.epoch, .source = config.source}))
          != IoResult::Complete) {
        return 7;
      }
      loop = pw_main_loop_new(nullptr);
      connectBackend();
      pw_loop* dispatch = loop != nullptr ? pw_main_loop_get_loop(loop) : nullptr;
      if (dispatch != nullptr) {
        pw_loop_enter(dispatch);
      }
      int result = 0;
      uint64_t lastWrite = 0;
      uint64_t lastSnapshot = 0;
      while (true) {
        if (connectionFailed) {
          disconnectBackend();
          backendRetryAfter = nowNs() + 1'000'000'000;
        }
        if (core == nullptr && nowNs() >= backendRetryAfter) {
          connectBackend();
        }
        if (dispatch != nullptr && core != nullptr && !connectionFailed) {
          if (pw_loop_iterate(dispatch, 0) < 0) {
            connectionFailed = true;
          }
          reconcile();
        }
        const auto now = nowNs();
        if (connectionFailed) {
          disconnectBackend();
          backendRetryAfter = now + 1'000'000'000;
        }
        if (acquisition.pending() && now - lastSnapshot >= kPublishInterval) {
          const auto snapshot = acquisition.take();
          pendingGeneration = snapshot->generation;
          writer.replace(*snapshot);
          lastSnapshot = now;
        }
        if (!writer.pending() && now - lastWrite >= kHeartbeat) {
          queueStatus(!acquisition.available());
        }
        const bool pending = writer.pending();
        const auto sent = writer.flush(STDIN_FILENO);
        if (sent == IoResult::Closed || sent == IoResult::Malformed) {
          break;
        }
        if (pending && sent == IoResult::Complete) {
          acquisition.published(pendingGeneration);
          lastWrite = now;
        }
        pollfd descriptors[2]{
            {.fd = STDIN_FILENO, .events = static_cast<short>(POLLIN | (writer.pending() ? POLLOUT : 0)), .revents = 0},
            {.fd = dispatch != nullptr && core != nullptr && !connectionFailed ? pw_loop_get_fd(dispatch) : -1,
             .events = POLLIN,
             .revents = 0},
        };
        if (poll(descriptors, 2, 10) < 0) {
          if (errno == EINTR) {
            continue;
          }
          result = 8;
          break;
        }
        // One configuration per launch. EOF, STOP by closing, or unexpected
        // input ends acquisition; never reconfigure a source under its epoch.
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
          break;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
          connectionFailed = true;
        }
      }
      if (dispatch != nullptr) {
        pw_loop_leave(dispatch);
      }
      return result;
    }

  private:
    Configuration config;
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    pw_metadata* metadata = nullptr;
    pw_stream* stream = nullptr;
    spa_hook coreListener{};
    spa_hook registryListener{};
    spa_hook metadataListener{};
    spa_hook streamListener{};
    struct NodeWatch {
      Provider* owner = nullptr;
      uint32_t id = 0;
      pw_node* proxy = nullptr;
      spa_hook listener{};
    };
    std::map<uint32_t, std::unique_ptr<NodeWatch>> nodeWatches;
    std::map<uint32_t, Node> nodes;
    std::map<uint32_t, Link> links;
    std::string defaultName;
    uint32_t metadataId = SPA_ID_INVALID;
    uint32_t selectedId = SPA_ID_INVALID;
    uint32_t channels = 0;
    std::array<uint32_t, SPA_AUDIO_MAX_CHANNELS> selectedPositions{};
    bool selectedPositioned = false;
    uint64_t retryAfter = 0;
    uint64_t backendRetryAfter = 0;
    bool selectionDirty = true;
    bool connectionFailed = false;
    bool streamFailed = false;
    bool formatValid = false;
    Acquisition acquisition;
    CoalescedWriter writer;
    uint64_t pendingGeneration = 1;

    void connectBackend() {
      disconnectBackend();
      if (loop != nullptr) {
        context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
      }
      if (context != nullptr) {
        core = pw_context_connect(context, nullptr, 0);
      }
      if (core != nullptr) {
        static const pw_core_events events = [] {
          pw_core_events result{};
          result.version = PW_VERSION_CORE_EVENTS;
          result.error = [](void* data, uint32_t id, int, int, const char*) {
            if (id == PW_ID_CORE) {
              static_cast<Provider*>(data)->connectionFailed = true;
            }
          };
          return result;
        }();
        pw_core_add_listener(core, &coreListener, &events, this);
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      }
      if (registry != nullptr) {
        static const pw_registry_events events = [] {
          pw_registry_events result{};
          result.version = PW_VERSION_REGISTRY_EVENTS;
          result.global = &global;
          result.global_remove = &removed;
          return result;
        }();
        pw_registry_add_listener(registry, &registryListener, &events, this);
      }
      if (registry == nullptr) {
        disconnectBackend();
        backendRetryAfter = nowNs() + 1'000'000'000;
      }
    }

    void disconnectBackend() {
      disconnectStream();
      for (const auto& [id, watch] : nodeWatches) {
        (void)id;
        spa_hook_remove(&watch->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(watch->proxy));
      }
      nodeWatches.clear();
      if (metadata != nullptr) {
        spa_hook_remove(&metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
        metadata = nullptr;
      }
      if (registry != nullptr) {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        registry = nullptr;
      }
      if (core != nullptr) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
        core = nullptr;
      }
      if (context != nullptr) {
        pw_context_destroy(context);
        context = nullptr;
      }
      metadataId = SPA_ID_INVALID;
      defaultName.clear();
      nodes.clear();
      links.clear();
      selectionDirty = true;
      streamFailed = false;
      connectionFailed = false;
    }

    void queueStatus(bool unavailable) {
      pendingGeneration = acquisition.publishedGeneration();
      writer.replace(Status{.epoch = config.epoch, .generation = pendingGeneration, .unavailable = unavailable});
    }

    void resetAnalysis() {
      acquisition.invalidate(nowNs());
      queueStatus(true);
    }

    void disconnectStream() {
      if (stream == nullptr) {
        return;
      }
      spa_hook_remove(&streamListener);
      pw_stream_destroy(stream);
      stream = nullptr;
      selectedId = SPA_ID_INVALID;
      channels = 0;
      formatValid = false;
      resetAnalysis();
    }

    static void global(void* data, uint32_t id, uint32_t, const char* type, uint32_t version, const spa_dict* props) {
      auto& self = *static_cast<Provider*>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        self.nodes[id] = {
            .name = property(props, PW_KEY_NODE_NAME),
            .serial = property(props, PW_KEY_OBJECT_SERIAL),
            .mediaClass = property(props, PW_KEY_MEDIA_CLASS),
            .channels = number(property(props, PW_KEY_AUDIO_CHANNELS)).value_or(0)
        };
        const auto& node = self.nodes[id];
        if (node.mediaClass == "Audio/Sink" || node.mediaClass == "Audio/Source") {
          auto watch = std::make_unique<NodeWatch>();
          watch->owner = &self;
          watch->id = id;
          watch->proxy = static_cast<pw_node*>(
              pw_registry_bind(self.registry, id, type, std::min(version, uint32_t{PW_VERSION_NODE}), 0)
          );
          if (watch->proxy != nullptr) {
            static const pw_node_events events = [] {
              pw_node_events result{};
              result.version = PW_VERSION_NODE_EVENTS;
              result.info = &nodeInfo;
              result.param = &nodeParam;
              return result;
            }();
            auto* state = watch.get();
            self.nodeWatches[id] = std::move(watch);
            pw_node_add_listener(state->proxy, &state->listener, &events, state);
            uint32_t params[] = {SPA_PARAM_EnumFormat, SPA_PARAM_Format};
            pw_node_subscribe_params(state->proxy, params, 2);
          }
        }
        self.selectionDirty = true;
      } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        self.links[id] = {
            .output = number(property(props, PW_KEY_LINK_OUTPUT_NODE)).value_or(SPA_ID_INVALID),
            .input = number(property(props, PW_KEY_LINK_INPUT_NODE)).value_or(SPA_ID_INVALID)
        };
        if (self.stream != nullptr && self.links[id].input == pw_stream_get_node_id(self.stream)) {
          self.resetAnalysis();
        }
      } else if (
          std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0
          && self.metadata == nullptr
          && self.config.selector == Selector::FollowDefault
          && property(props, PW_KEY_METADATA_NAME) == "default"
      ) {
        self.metadata = static_cast<pw_metadata*>(
            pw_registry_bind(self.registry, id, type, std::min(version, uint32_t{PW_VERSION_METADATA}), 0)
        );
        if (self.metadata != nullptr) {
          self.metadataId = id;
          static const pw_metadata_events events{.version = PW_VERSION_METADATA_EVENTS, .property = &metadataProperty};
          pw_metadata_add_listener(self.metadata, &self.metadataListener, &events, &self);
        }
      }
    }

    static void removed(void* data, uint32_t id) {
      auto& self = *static_cast<Provider*>(data);
      if (auto found = self.nodeWatches.find(id); found != self.nodeWatches.end()) {
        spa_hook_remove(&found->second->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(found->second->proxy));
        self.nodeWatches.erase(found);
      }
      const auto link = self.links.find(id);
      if (id == self.selectedId
          || (self.stream != nullptr
              && link != self.links.end()
              && link->second.input == pw_stream_get_node_id(self.stream))) {
        self.resetAnalysis();
      }
      self.nodes.erase(id);
      self.links.erase(id);
      if (self.metadataId == id) {
        spa_hook_remove(&self.metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(self.metadata));
        self.metadata = nullptr;
        self.metadataId = SPA_ID_INVALID;
        self.defaultName.clear();
      }
      self.selectionDirty = true;
    }

    static void nodeInfo(void* data, const pw_node_info* info) {
      auto& watch = *static_cast<NodeWatch*>(data);
      auto& self = *watch.owner;
      auto& node = self.nodes.at(watch.id);
      if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
        for (const auto& [key, field] :
             {std::pair{PW_KEY_NODE_NAME, &node.name}, std::pair{PW_KEY_OBJECT_SERIAL, &node.serial},
              std::pair{PW_KEY_MEDIA_CLASS, &node.mediaClass}}) {
          if (const char* value = spa_dict_lookup(info->props, key)) {
            *field = value;
          }
        }
        if (const char* value = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS)) {
          node.channels = number(value).value_or(0);
        }
        if (const char* value = spa_dict_lookup(info->props, "audio.position")) {
          uint32_t count = 0;
          node.positioned = spa_audio_parse_position(value, std::strlen(value), node.positions.data(), &count) >= 0
              && count == node.channels;
        }
        self.selectionDirty = true;
      }
    }

    static void nodeParam(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
      auto& watch = *static_cast<NodeWatch*>(data);
      if (param == nullptr || (id != SPA_PARAM_EnumFormat && id != SPA_PARAM_Format)) {
        return;
      }
      spa_audio_info_raw format{};
      if (spa_format_audio_raw_parse(param, &format) < 0 || format.channels == 0 || format.channels > kMaxChannels) {
        return;
      }
      auto& node = watch.owner->nodes.at(watch.id);
      node.channels = format.channels;
      std::ranges::copy(format.position, node.positions.begin());
      node.positioned = (format.flags & SPA_AUDIO_FLAG_UNPOSITIONED) == 0;
      watch.owner->selectionDirty = true;
    }

    static int metadataProperty(void* data, uint32_t subject, const char* key, const char*, const char* value) {
      auto& self = *static_cast<Provider*>(data);
      const std::string_view expected =
          self.config.source == SourceType::Playback ? "default.audio.sink" : "default.audio.source";
      if (subject != PW_ID_CORE || (key != nullptr && key != expected)) {
        return 0;
      }
      std::string name;
      if (value != nullptr) {
        const auto json = nlohmann::json::parse(value, nullptr, false);
        if (json.is_object() && json.contains("name") && json["name"].is_string()) {
          name = json["name"].get<std::string>();
        }
      }
      if (name != self.defaultName) {
        self.defaultName = std::move(name);
        self.selectionDirty = true;
      }
      return 0;
    }

    void reconcile() {
      if ((!selectionDirty && !streamFailed) || nowNs() < retryAfter) {
        return;
      }
      selectionDirty = false;
      const auto& target = config.selector == Selector::Fixed ? config.target : defaultName;
      const std::string_view expected = config.source == SourceType::Playback ? "Audio/Sink" : "Audio/Source";
      auto chosen = nodes.end();
      for (auto iter = nodes.begin(); iter != nodes.end(); ++iter) {
        const auto& node = iter->second;
        if (!target.empty()
            && (node.name == target || (config.selector == Selector::Fixed && node.serial == target))
            && node.mediaClass == expected
            && !node.serial.empty()
            && node.channels >= 1
            && node.channels <= kMaxChannels) {
          if (chosen != nodes.end()) {
            chosen = nodes.end(); // Ambiguous names are unavailable, never arbitrary selection.
            break;
          }
          chosen = iter;
        }
      }
      if (chosen != nodes.end()
          && chosen->first == selectedId
          && chosen->second.channels == channels
          && chosen->second.positions == selectedPositions
          && chosen->second.positioned == selectedPositioned
          && !streamFailed) {
        return;
      }
      const bool failed = streamFailed;
      disconnectStream();
      streamFailed = false;
      if (failed) {
        retryAfter = nowNs() + 500'000'000;
        selectionDirty = true;
        return;
      }
      if (chosen == nodes.end()) {
        return;
      }
      selectedId = chosen->first;
      channels = chosen->second.channels;
      selectedPositions = chosen->second.positions;
      selectedPositioned = chosen->second.positioned;
      resetAnalysis();
      // Pin even follow-default streams to the resolved serial. Metadata changes
      // recreate the stream explicitly after rechecking its source type. These
      // properties are defense in depth: incoming links are checked below too.
      pw_properties* props = pw_properties_new(
          PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Analyzer", PW_KEY_APP_NAME,
          "Umbriel audio effects", PW_KEY_NODE_NAME, "umbriel-audio", PW_KEY_TARGET_OBJECT,
          chosen->second.serial.c_str(), PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback", "true",
          PW_KEY_STREAM_DONT_REMIX, "true", PW_KEY_STREAM_CAPTURE_SINK,
          config.source == SourceType::Playback ? "true" : "false", nullptr
      );
      stream = pw_stream_new(core, "Umbriel audio effects", props);
      if (stream == nullptr) {
        selectedId = SPA_ID_INVALID;
        retryAfter = nowNs() + 500'000'000;
        selectionDirty = true;
        return;
      }
      static const pw_stream_events events = [] {
        pw_stream_events result{};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char*) {
          auto& self = *static_cast<Provider*>(data);
          if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            self.streamFailed = true;
            self.resetAnalysis();
          }
        };
        result.param_changed = &formatChanged;
        result.process = &process;
        return result;
      }();
      pw_stream_add_listener(stream, &streamListener, &events, this);
      spa_audio_info_raw format{};
      format.format = SPA_AUDIO_FORMAT_F32;
      format.rate = kSampleRate;
      format.channels = channels;
      if (chosen->second.positioned) {
        std::copy(chosen->second.positions.begin(), chosen->second.positions.end(), std::begin(format.position));
      } else {
        format.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
      }
      uint8_t storage[1024];
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
      const spa_pod* parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
      const auto flags = static_cast<pw_stream_flags>(
          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT
      );
      if (pw_stream_connect(stream, PW_DIRECTION_INPUT, SPA_ID_INVALID, flags, parameters, 1) < 0) {
        disconnectStream();
        retryAfter = nowNs() + 500'000'000;
        selectionDirty = true;
      }
    }

    static void formatChanged(void* data, uint32_t id, const spa_pod* param) {
      auto& self = *static_cast<Provider*>(data);
      if (id != SPA_PARAM_Format) {
        return;
      }
      spa_audio_info_raw format{};
      self.formatValid = param != nullptr
          && spa_format_audio_raw_parse(param, &format) >= 0
          && format.format == SPA_AUDIO_FORMAT_F32
          && format.rate == kSampleRate
          && format.channels == self.channels;
      self.resetAnalysis();
    }

    bool linksVerified() const {
      const auto ownId = pw_stream_get_node_id(stream);
      size_t connectedChannels = 0;
      for (const auto& [id, link] : links) {
        (void)id;
        if (link.input == ownId) {
          if (link.output != selectedId) {
            return false;
          }
          ++connectedChannels;
        }
      }
      return connectedChannels == channels && nodes.contains(selectedId);
    }

    static void process(void* data) {
      auto& self = *static_cast<Provider*>(data);
      pw_buffer* received = pw_stream_dequeue_buffer(self.stream);
      if (received == nullptr) {
        return;
      }
      self.consume(*received);
      pw_stream_queue_buffer(self.stream, received);
    }

    void consume(const pw_buffer& received) {
      const spa_buffer* buffer = received.buffer;
      const auto now = nowNs();
      if (!formatValid
          || !linksVerified()
          || buffer == nullptr
          || buffer->n_datas != 1
          || received.time == 0
          || received.time > now
          || now - received.time > kMaxBufferAge) {
        resetAnalysis();
        return;
      }
      const spa_data& data = buffer->datas[0];
      const auto stride = channels * sizeof(float);
      if (data.data == nullptr
          || data.chunk == nullptr
          || (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0
          || data.chunk->offset > data.maxsize
          || data.chunk->size > data.maxsize - data.chunk->offset
          || data.chunk->size % stride != 0
          || data.chunk->stride != static_cast<int32_t>(stride)
          || data.chunk->offset % alignof(float) != 0
          || data.chunk->size == 0) {
        resetAnalysis();
        return;
      }
      const auto* header = static_cast<const spa_meta_header*>(
          spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header))
      );
      if (header != nullptr && (header->flags & (SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_DISCONT)) != 0) {
        resetAnalysis();
        if ((header->flags & SPA_META_HEADER_FLAG_CORRUPTED) != 0) {
          return;
        }
      }
      const auto* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data.data) + data.chunk->offset);
      if (!acquisition.ingest(
              channels, std::span(samples, data.chunk->size / sizeof(float)), received.time, now, config.epoch
          )
          && !acquisition.available()) {
        // Preserve the invalidation barrier when rejecting queued old buffers.
        // Always replace an unsent measurement with the unavailable status.
        queueStatus(true);
      }
    }
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 1) {
    return 2;
  }
  pollfd channel{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
  if (poll(&channel, 1, 1000) <= 0) {
    return 3;
  }
  std::vector<uint8_t> bytes;
  if (receivePacket(STDIN_FILENO, bytes, true) != IoResult::Complete) {
    return 4;
  }
  const auto packet = decode(bytes);
  if (!packet || !std::holds_alternative<Configuration>(*packet)) {
    return 5;
  }
  const auto configuration = std::get<Configuration>(*packet);
  if ((configuration.source != SourceType::Playback && configuration.source != SourceType::Microphone)
      || (configuration.selector != Selector::Fixed && configuration.selector != Selector::FollowDefault)) {
    return 6;
  }
  pw_init(&argc, &argv);
  int result = 0;
  {
    Provider provider(configuration);
    result = provider.run();
  }
  pw_deinit();
  return result;
}
