// Sets a regular clipboard selection through version 1 of the legacy wlr
// data-control protocol, then receives the exact payload through ext-data-control.

#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>

namespace {
  constexpr std::string_view kMimeType = "text/plain;charset=utf-8";
  constexpr std::string_view kPayload = "umbriel legacy data-control clipboard payload";

  struct State;

  struct ExtOfferState {
    ext_data_control_offer_v1* offer = nullptr;
    bool expectedMime = false;
  };

  struct State {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    zwlr_data_control_manager_v1* legacyManager = nullptr;
    ext_data_control_manager_v1* extManager = nullptr;
    zwlr_data_control_device_v1* legacyDevice = nullptr;
    ext_data_control_device_v1* extDevice = nullptr;
    zwlr_data_control_source_v1* legacySource = nullptr;
    std::vector<zwlr_data_control_offer_v1*> legacyOffers;
    std::vector<std::unique_ptr<ExtOfferState>> extOffers;
    ExtOfferState* extSelection = nullptr;
    unsigned extSelectionEvents = 0;
    bool legacyFinished = false;
    bool extFinished = false;
    bool sourceSent = false;
    bool sourceMimeMatched = false;
    bool sourceCancelled = false;
    std::string failure;
  };

  void fail(State& state, std::string message) {
    if (state.failure.empty()) {
      state.failure = std::move(message);
    }
  }

  void legacyOfferMime(void*, zwlr_data_control_offer_v1*, const char*) {}

  constexpr zwlr_data_control_offer_v1_listener kLegacyOfferListener{
      .offer = legacyOfferMime,
  };

  void legacyDataOffer(void* data, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1* offer) {
    auto& state = *static_cast<State*>(data);
    zwlr_data_control_offer_v1_add_listener(offer, &kLegacyOfferListener, &state);
    state.legacyOffers.push_back(offer);
  }

  void legacySelection(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {}

  void legacyFinished(void* data, zwlr_data_control_device_v1*) { static_cast<State*>(data)->legacyFinished = true; }

  void legacyPrimarySelection(void*, zwlr_data_control_device_v1*, zwlr_data_control_offer_v1*) {}

  constexpr zwlr_data_control_device_v1_listener kLegacyDeviceListener{
      .data_offer = legacyDataOffer,
      .selection = legacySelection,
      .finished = legacyFinished,
      .primary_selection = legacyPrimarySelection,
  };

  void extOfferMime(void* data, ext_data_control_offer_v1*, const char* mimeType) {
    auto& offerState = *static_cast<ExtOfferState*>(data);
    if (mimeType != nullptr && kMimeType == mimeType) {
      offerState.expectedMime = true;
    }
  }

  constexpr ext_data_control_offer_v1_listener kExtOfferListener{
      .offer = extOfferMime,
  };

  ExtOfferState* findExtOffer(State& state, ext_data_control_offer_v1* offer) {
    const auto found =
        std::ranges::find_if(state.extOffers, [offer](const auto& candidate) { return candidate->offer == offer; });
    return found == state.extOffers.end() ? nullptr : found->get();
  }

  void extDataOffer(void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
    auto& state = *static_cast<State*>(data);
    auto offerState = std::make_unique<ExtOfferState>(offer, false);
    ext_data_control_offer_v1_add_listener(offer, &kExtOfferListener, offerState.get());
    state.extOffers.push_back(std::move(offerState));
  }

  void extSelection(void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
    auto& state = *static_cast<State*>(data);
    ++state.extSelectionEvents;
    if (offer == nullptr) {
      state.extSelection = nullptr;
      return;
    }
    state.extSelection = findExtOffer(state, offer);
    if (state.extSelection == nullptr) {
      fail(state, "ext-data-control selected an offer it did not announce");
    }
  }

  void extFinished(void* data, ext_data_control_device_v1*) { static_cast<State*>(data)->extFinished = true; }

  void extPrimarySelection(void*, ext_data_control_device_v1*, ext_data_control_offer_v1*) {}

  constexpr ext_data_control_device_v1_listener kExtDeviceListener{
      .data_offer = extDataOffer,
      .selection = extSelection,
      .finished = extFinished,
      .primary_selection = extPrimarySelection,
  };

  bool writeAll(State& state, int fd, std::string_view payload) {
    size_t written = 0;
    while (written < payload.size()) {
      const ssize_t result = write(fd, payload.data() + written, payload.size() - written);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result <= 0) {
        fail(state, std::string("failed to write clipboard payload: ") + std::strerror(errno));
        return false;
      }
      written += static_cast<size_t>(result);
    }
    return true;
  }

  void legacySourceSend(void* data, zwlr_data_control_source_v1*, const char* mimeType, int fd) {
    auto& state = *static_cast<State*>(data);
    state.sourceSent = true;
    state.sourceMimeMatched = mimeType != nullptr && kMimeType == mimeType;
    if (!state.sourceMimeMatched) {
      fail(state, "legacy source received an unexpected MIME request");
    } else {
      writeAll(state, fd, kPayload);
    }
    close(fd);
  }

  void legacySourceCancelled(void* data, zwlr_data_control_source_v1*) {
    static_cast<State*>(data)->sourceCancelled = true;
  }

  constexpr zwlr_data_control_source_v1_listener kLegacySourceListener{
      .send = legacySourceSend,
      .cancelled = legacySourceCancelled,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (state.seat == nullptr && std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (
        state.legacyManager == nullptr && std::strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0
    ) {
      // Bind exactly the version required by legacy applications, even though
      // wlroots advertises version 2.
      state.legacyManager = static_cast<zwlr_data_control_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 1)
      );
    } else if (state.extManager == nullptr && std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
      state.extManager = static_cast<ext_data_control_manager_v1*>(
          wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, std::min(version, 1U))
      );
    }
  }

  void registryRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = registryGlobal,
      .global_remove = registryRemove,
  };

  bool roundtrip(State& state, std::string_view phase) {
    if (wl_display_roundtrip(state.display) >= 0) {
      return state.failure.empty();
    }
    fail(state, std::string("Wayland roundtrip failed while ") + std::string(phase));
    return false;
  }

  bool readPayload(State& state, int fd, std::string& payload) {
    std::array<char, 256> buffer{};
    while (true) {
      pollfd descriptor{.fd = fd, .events = POLLIN | POLLHUP, .revents = 0};
      const int ready = poll(&descriptor, 1, 2000);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0) {
        fail(state, std::string("failed to wait for clipboard payload: ") + std::strerror(errno));
        return false;
      }
      if (ready == 0) {
        fail(state, "timed out waiting for clipboard payload");
        return false;
      }
      const ssize_t count = read(fd, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        fail(state, std::string("failed to read clipboard payload: ") + std::strerror(errno));
        return false;
      }
      if (count == 0) {
        return true;
      }
      payload.append(buffer.data(), static_cast<size_t>(count));
    }
  }

  void cleanup(State& state) {
    for (auto* offer : state.legacyOffers) {
      zwlr_data_control_offer_v1_destroy(offer);
    }
    for (const auto& offer : state.extOffers) {
      ext_data_control_offer_v1_destroy(offer->offer);
    }
    if (state.extDevice != nullptr) {
      ext_data_control_device_v1_destroy(state.extDevice);
    }
    if (state.legacyDevice != nullptr) {
      zwlr_data_control_device_v1_destroy(state.legacyDevice);
    }
    if (state.legacySource != nullptr) {
      zwlr_data_control_source_v1_destroy(state.legacySource);
    }
    if (state.extManager != nullptr) {
      ext_data_control_manager_v1_destroy(state.extManager);
    }
    if (state.legacyManager != nullptr) {
      zwlr_data_control_manager_v1_destroy(state.legacyManager);
    }
    if (state.seat != nullptr) {
      wl_seat_destroy(state.seat);
    }
    if (state.registry != nullptr) {
      wl_registry_destroy(state.registry);
    }
    if (state.display != nullptr) {
      wl_display_disconnect(state.display);
    }
  }

  bool run(State& state) {
    state.registry = wl_display_get_registry(state.display);
    if (state.registry == nullptr) {
      fail(state, "failed to get Wayland registry");
      return false;
    }
    wl_registry_add_listener(state.registry, &kRegistryListener, &state);
    if (!roundtrip(state, "discovering globals")) {
      return false;
    }
    if (state.seat == nullptr) {
      fail(state, "wl_seat is unavailable");
      return false;
    }
    if (state.legacyManager == nullptr) {
      fail(state, "zwlr_data_control_manager_v1 is unavailable");
      return false;
    }
    if (state.extManager == nullptr) {
      fail(state, "ext_data_control_manager_v1 is unavailable");
      return false;
    }

    state.extDevice = ext_data_control_manager_v1_get_data_device(state.extManager, state.seat);
    state.legacyDevice = zwlr_data_control_manager_v1_get_data_device(state.legacyManager, state.seat);
    if (state.extDevice == nullptr || state.legacyDevice == nullptr) {
      fail(state, "failed to create data-control devices");
      return false;
    }
    ext_data_control_device_v1_add_listener(state.extDevice, &kExtDeviceListener, &state);
    zwlr_data_control_device_v1_add_listener(state.legacyDevice, &kLegacyDeviceListener, &state);
    if (!roundtrip(state, "receiving initial selections")) {
      return false;
    }
    if (state.extSelection != nullptr) {
      fail(state, "headless test started with an unexpected clipboard selection");
      return false;
    }
    const unsigned initialSelectionEvents = state.extSelectionEvents;

    state.legacySource = zwlr_data_control_manager_v1_create_data_source(state.legacyManager);
    if (state.legacySource == nullptr) {
      fail(state, "failed to create legacy data-control source");
      return false;
    }
    zwlr_data_control_source_v1_add_listener(state.legacySource, &kLegacySourceListener, &state);
    zwlr_data_control_source_v1_offer(state.legacySource, kMimeType.data());
    zwlr_data_control_device_v1_set_selection(state.legacyDevice, state.legacySource);
    if (!roundtrip(state, "setting the legacy clipboard selection")) {
      return false;
    }
    if (state.legacyFinished || state.extFinished) {
      fail(state, "a data-control device unexpectedly finished");
      return false;
    }
    if (state.sourceCancelled) {
      fail(state, "legacy clipboard source was unexpectedly cancelled");
      return false;
    }
    if (state.extSelectionEvents <= initialSelectionEvents || state.extSelection == nullptr) {
      fail(state, "legacy clipboard selection did not reach ext-data-control");
      return false;
    }
    if (!state.extSelection->expectedMime) {
      fail(state, "ext-data-control selection did not advertise the expected MIME type");
      return false;
    }

    int pipeFds[2];
    if (pipe(pipeFds) < 0) {
      fail(state, std::string("failed to create clipboard pipe: ") + std::strerror(errno));
      return false;
    }
    ext_data_control_offer_v1_receive(state.extSelection->offer, kMimeType.data(), pipeFds[1]);
    close(pipeFds[1]);
    if (!roundtrip(state, "requesting the clipboard payload")) {
      close(pipeFds[0]);
      return false;
    }

    std::string received;
    const bool read = readPayload(state, pipeFds[0], received);
    close(pipeFds[0]);
    if (!read) {
      return false;
    }
    if (!state.sourceSent || !state.sourceMimeMatched) {
      fail(state, "legacy source did not serve the ext-data-control request");
      return false;
    }
    if (received != kPayload) {
      fail(state, "clipboard payload changed during the cross-protocol transfer");
      return false;
    }
    return true;
  }
} // namespace

int main() {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "data-control-client: cannot connect to WAYLAND_DISPLAY");
    return 1;
  }

  const bool success = run(state);
  cleanup(state);
  if (!success) {
    std::println(stderr, "data-control-client: {}", state.failure);
    return 1;
  }
  std::println("legacy data-control v1 clipboard reached ext-data-control with exact content");
  return 0;
}
