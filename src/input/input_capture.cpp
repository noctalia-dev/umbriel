#include "input/input_capture.h"

#include "core/log.h"
#include "input/cursor.h"
#include "input/input_capture_geometry.h"
#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <libeis.h>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <umbriel-input-capture-v1-server-protocol.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {
    constexpr Logger kLog("input-capture");
    constexpr size_t kMaxSessions = 16;
    constexpr size_t kMaxBarriers = 256;
    constexpr uint32_t kKeyboard = 1;
    constexpr uint32_t kPointer = 2;
    constexpr uint32_t kSuccess = 0;
    constexpr uint32_t kCancelled = 1;
    constexpr uint32_t kFailure = 2;

    wl_fixed_t protocolPosition(double value) {
      constexpr double minimum = static_cast<double>(std::numeric_limits<int32_t>::min()) / 256.0;
      constexpr double maximum = static_cast<double>(std::numeric_limits<int32_t>::max()) / 256.0;
      return wl_fixed_from_double(std::clamp(value, minimum, maximum));
    }
  } // namespace

  struct InputCapture::Impl {
    struct Session;

    struct KeyboardDevice {
      wlr_keyboard* source = nullptr;
      eis_device* handle = nullptr;
      std::string keymap;
      std::unordered_set<uint32_t> keys;

      ~KeyboardDevice() {
        if (handle != nullptr) {
          eis_device_remove(handle);
          eis_device_unref(handle);
        }
      }
    };

    struct Session {
      Impl& owner;
      wl_resource* resource = nullptr;
      uint64_t id = 0;
      uint32_t startRequest = 0;
      uint32_t capabilities = 0;
      uint32_t lastZoneSet = 0;
      uint32_t activation = 0;
      uint32_t persistMode = 0;
      uint64_t permissionPeer = 0;
      std::string application;
      std::string applicationInstance;
      std::string restoreToken;
      std::string grantId;
      bool startAttempted = false;
      bool pending = false;
      bool approved = false;
      bool closed = false;
      bool enabled = false;
      bool overflow = false;
      bool fdIssued = false;
      bool keyboardBound = false;
      bool pointerBound = false;
      bool buttonsBound = false;
      bool scrollBound = false;
      eis* context = nullptr;
      eis_client* client = nullptr;
      eis_seat* seat = nullptr;
      eis_device* pointer = nullptr;
      wl_event_source* eventSource = nullptr;
      std::vector<std::unique_ptr<KeyboardDevice>> keyboards;
      std::unordered_set<uint32_t> buttons;
      std::vector<InputCaptureBarrier> barriers;
      std::vector<InputCaptureBarrier> pendingBarriers;
      std::unordered_map<const void*, std::pair<double, double>> absoluteTargets;

      explicit Session(Impl& owner) : owner(owner) {}

      ~Session() {
        deactivate(false);
        if (pending && owner.cancelled) {
          owner.cancelled(id);
        }
        if (eventSource != nullptr) {
          wl_event_source_remove(eventSource);
        }
        destroyClient();
        if (context != nullptr) {
          eis_unref(context);
        }
      }

      void result(uint32_t request, bool success) const {
        umbriel_input_capture_session_v1_send_result(resource, request, success ? kSuccess : kFailure);
      }

      [[nodiscard]] bool usable() const { return approved && !closed && !owner.blocked(); }

      void clearPointer() {
        if (pointer != nullptr) {
          eis_device_remove(pointer);
          eis_device_unref(pointer);
          pointer = nullptr;
        }
        buttons.clear();
      }

      void destroyClient() {
        keyboards.clear();
        clearPointer();
        if (seat != nullptr) {
          eis_seat_set_user_data(seat, nullptr);
          eis_seat_remove(seat);
          eis_seat_unref(seat);
          seat = nullptr;
        }
        if (client != nullptr) {
          eis_client_set_user_data(client, nullptr);
          eis_client_disconnect(client);
          eis_client_unref(client);
          client = nullptr;
        }
        keyboardBound = pointerBound = buttonsBound = scrollBound = false;
      }

      void close() {
        if (closed) {
          return;
        }
        disable(true);
        closed = true;
        if (pending) {
          pending = false;
          if (owner.cancelled) {
            owner.cancelled(id);
          }
          umbriel_input_capture_session_v1_send_started(resource, startRequest, kFailure, 0);
        }
        destroyClient();
        umbriel_input_capture_session_v1_send_closed(resource);
      }

      void sendModifiers(KeyboardDevice& keyboard) const {
        const auto& mods = keyboard.source->modifiers;
        eis_device_keyboard_send_xkb_modifiers(keyboard.handle, mods.depressed, mods.latched, mods.locked, mods.group);
      }

      void beginKeyboard(KeyboardDevice& keyboard) {
        eis_device_start_emulating(keyboard.handle, activation);
        for (size_t index = 0; index < keyboard.source->num_keycodes; ++index) {
          const uint32_t code = keyboard.source->keycodes[index];
          if (!owner.server.keyboardKeyConsumed(keyboard.source, code) && keyboard.keys.insert(code).second) {
            eis_device_keyboard_key(keyboard.handle, code, true);
          }
        }
        eis_device_frame(keyboard.handle, eis_now(context));
        sendModifiers(keyboard);
      }

      void addKeyboard(wlr_keyboard* keyboard) {
        if (seat == nullptr
            || !keyboardBound
            || keyboard == nullptr
            || keyboard->keymap == nullptr
            || keyboard->keymap_fd < 0
            || std::ranges::any_of(keyboards, [keyboard](const auto& item) { return item->source == keyboard; })) {
          return;
        }
        auto device = std::make_unique<KeyboardDevice>();
        device->source = keyboard;
        device->keymap = keyboard->keymap_string != nullptr ? keyboard->keymap_string : "";
        device->handle = eis_seat_new_device(seat);
        if (device->handle == nullptr) {
          return;
        }
        eis_device_configure_type(device->handle, EIS_DEVICE_TYPE_VIRTUAL);
        eis_device_configure_name(device->handle, "Umbriel captured keyboard");
        eis_device_configure_capability(device->handle, EIS_DEVICE_CAP_KEYBOARD);
        eis_keymap* keymap =
            eis_device_new_keymap(device->handle, EIS_KEYMAP_TYPE_XKB, keyboard->keymap_fd, keyboard->keymap_size);
        if (keymap == nullptr) {
          eis_device_unref(device->handle);
          device->handle = nullptr;
          return;
        }
        eis_keymap_add(keymap);
        eis_keymap_unref(keymap);
        eis_device_add(device->handle);
        eis_device_resume(device->handle);
        if (owner.active == this) {
          beginKeyboard(*device);
        }
        keyboards.push_back(std::move(device));
      }

      void createPointer() {
        if (seat == nullptr || !pointerBound || pointer != nullptr || owner.zones.empty()) {
          return;
        }
        eis_device* device = eis_seat_new_device(seat);
        if (device == nullptr) {
          return;
        }
        eis_device_configure_type(device, EIS_DEVICE_TYPE_VIRTUAL);
        eis_device_configure_name(device, "Umbriel captured pointer");
        eis_device_configure_capability(device, EIS_DEVICE_CAP_POINTER);
        if (buttonsBound) {
          eis_device_configure_capability(device, EIS_DEVICE_CAP_BUTTON);
        }
        if (scrollBound) {
          eis_device_configure_capability(device, EIS_DEVICE_CAP_SCROLL);
        }
        // EI regions describe relative pointer geometry too. Their offsets are
        // unsigned, so preserve the logical layout in a separate desktop origin.
        int32_t minimumX = owner.zones.front().x;
        int32_t minimumY = owner.zones.front().y;
        for (const auto& zone : owner.zones) {
          minimumX = std::min(minimumX, zone.x);
          minimumY = std::min(minimumY, zone.y);
        }
        for (const auto& zone : owner.zones) {
          const uint64_t x = static_cast<int64_t>(zone.x) - minimumX;
          const uint64_t y = static_cast<int64_t>(zone.y) - minimumY;
          constexpr uint32_t maximum = std::numeric_limits<uint32_t>::max();
          if (x > maximum - zone.width || y > maximum - zone.height) {
            kLog.error("input capture layout exceeds EI region coordinates");
            eis_device_remove(device);
            eis_device_unref(device);
            return;
          }
          eis_region* region = eis_device_new_region(device);
          if (region == nullptr) {
            kLog.error("failed to allocate an input capture EI region");
            eis_device_remove(device);
            eis_device_unref(device);
            return;
          }
          eis_region_set_offset(region, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
          eis_region_set_size(region, zone.width, zone.height);
          eis_region_set_physical_scale(region, zone.physicalScale);
          eis_region_add(region);
          eis_region_unref(region);
        }
        eis_device_add(device);
        eis_device_resume(device);
        pointer = device;
      }

      [[nodiscard]] bool ready() const {
        return client != nullptr
            && seat != nullptr
            && ((capabilities & kKeyboard) == 0 || keyboardBound)
            && ((capabilities & kPointer) == 0 || pointer != nullptr);
      }

      void activate(uint32_t barrier, double x, double y) {
        owner.active = this;
        activation = ++owner.nextActivation;
        if (activation == 0) {
          activation = ++owner.nextActivation;
        }
        // The compositor callback ends local delivery before any held key is
        // sent to EI. The activation sequence is identical on both transports.
        if (owner.changed) {
          owner.changed(true);
        }
        umbriel_input_capture_session_v1_send_activated(
            resource, activation, protocolPosition(x), protocolPosition(y), barrier
        );
        if (pointer != nullptr) {
          eis_device_start_emulating(pointer, activation);
        }
        for (const auto& keyboard : keyboards) {
          beginKeyboard(*keyboard);
        }
      }

      void deactivate(bool signal) {
        if (owner.active != this) {
          return;
        }
        for (const auto& keyboard : keyboards) {
          for (const uint32_t code : keyboard->keys) {
            eis_device_keyboard_key(keyboard->handle, code, false);
          }
          keyboard->keys.clear();
          eis_device_frame(keyboard->handle, eis_now(context));
          eis_device_keyboard_send_xkb_modifiers(keyboard->handle, 0, 0, 0, 0);
          eis_device_stop_emulating(keyboard->handle);
        }
        if (pointer != nullptr) {
          if (buttonsBound) {
            for (const uint32_t button : buttons) {
              eis_device_button_button(pointer, button, false);
            }
          }
          buttons.clear();
          if (scrollBound) {
            eis_device_scroll_stop(pointer, true, true);
          }
          eis_device_frame(pointer, eis_now(context));
          eis_device_stop_emulating(pointer);
        }
        owner.active = nullptr;
        absoluteTargets.clear();
        if (owner.changed) {
          owner.changed(false);
        }
        if (signal) {
          umbriel_input_capture_session_v1_send_deactivated(resource, activation);
        }
      }

      void disable(bool signal) {
        deactivate(signal);
        const bool wasEnabled = std::exchange(enabled, false);
        if (signal && wasEnabled) {
          umbriel_input_capture_session_v1_send_disabled(resource);
        }
      }

      void handleEiEvent(eis_event* event) {
        const auto type = eis_event_get_type(event);
        eis_client* candidate = eis_event_get_client(event);
        if (type == EIS_EVENT_CLIENT_CONNECT) {
          if (closed || client != nullptr || candidate == nullptr || eis_client_is_sender(candidate)) {
            if (candidate != nullptr) {
              eis_client_disconnect(candidate);
            }
            if (!closed && client == nullptr) {
              close();
            }
            return;
          }
          client = eis_client_ref(candidate);
          eis_client_set_user_data(client, this);
          eis_client_connect(client);
          seat = eis_client_new_seat(client, "Umbriel capture");
          if (seat == nullptr) {
            close();
            return;
          }
          if ((capabilities & kKeyboard) != 0) {
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
          }
          if ((capabilities & kPointer) != 0) {
            for (const auto cap : {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL}) {
              eis_seat_configure_capability(seat, cap);
            }
          }
          eis_seat_add(seat);
          return;
        }
        if (candidate == nullptr || candidate != client || closed) {
          return;
        }
        if (type == EIS_EVENT_CLIENT_DISCONNECT) {
          close();
        } else if (type == EIS_EVENT_SEAT_BIND) {
          if (owner.active == this) {
            disable(true);
          }
          keyboards.clear();
          clearPointer();
          keyboardBound =
              (capabilities & kKeyboard) != 0 && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD);
          pointerBound = (capabilities & kPointer) != 0 && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER);
          buttonsBound = pointerBound && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON);
          scrollBound = pointerBound && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL);
          createPointer();
          for (wlr_keyboard* keyboard : owner.keyboards) {
            addKeyboard(keyboard);
          }
        } else if (type == EIS_EVENT_DEVICE_CLOSED) {
          disable(true);
          eis_device* device = eis_event_get_device(event);
          if (device == pointer) {
            clearPointer();
          } else {
            std::erase_if(keyboards, [device](const auto& keyboard) { return keyboard->handle == device; });
          }
        }
      }

      bool connect(uint32_t request) {
        if (!usable() || fdIssued) {
          result(request, false);
          return false;
        }
        context = eis_new(this);
        if (context == nullptr || eis_setup_backend_fd(context) != 0) {
          if (context != nullptr) {
            eis_unref(context);
            context = nullptr;
          }
          result(request, false);
          return false;
        }
        const int fd = eis_backend_fd_add_client(context);
        if (fd < 0) {
          eis_unref(context);
          context = nullptr;
          result(request, false);
          return false;
        }
        eventSource = wl_event_loop_add_fd(
            wl_display_get_event_loop(owner.server.display()), eis_get_fd(context), WL_EVENT_READABLE,
            [](int, uint32_t, void* data) -> int {
              auto* session = static_cast<Session*>(data);
              eis_dispatch(session->context);
              while (eis_event* event = eis_get_event(session->context)) {
                session->handleEiEvent(event);
                eis_event_unref(event);
              }
              return 0;
            },
            this
        );
        if (eventSource == nullptr) {
          ::close(fd);
          eis_unref(context);
          context = nullptr;
          result(request, false);
          return false;
        }
        fdIssued = true;
        umbriel_input_capture_session_v1_send_eis_fd(resource, request, fd);
        ::close(fd);
        result(request, true);
        return true;
      }

      void start(uint32_t request, const char* label, uint32_t requested) {
        if (startAttempted
            || closed
            || owner.blocked()
            || !owner.authorize
            || label == nullptr
            || std::char_traits<char>::length(label) > 256
            || (requested & 3U) == 0) {
          umbriel_input_capture_session_v1_send_started(resource, request, kFailure, 0);
          return;
        }
        startAttempted = true;
        startRequest = request;
        capabilities = requested & 3U;
        application = label;
        wl_client* client = wl_resource_get_client(resource);
        permissionPeer = owner.permissionPeer(client, "");
        if (!restoreToken.empty()) {
          const uint64_t scoped = owner.permissionPeer(client, applicationInstance, false);
          const bool newScope = persistMode == 1 && scoped == 0;
          const uint64_t restorePeer = persistMode == 1 ? owner.permissionPeer(client, applicationInstance)
              : persistMode == 0 && scoped != 0         ? scoped
                                                        : permissionPeer;
          const auto grant =
              owner.permissionStore.restore(restoreToken, application, capabilities, persistMode, restorePeer);
          restoreToken.clear();
          if (grant) {
            permissionPeer = restorePeer;
            approve(*grant);
            return;
          }
          if (newScope) {
            owner.forgetPermissionScope(client, applicationInstance);
          }
        }
        pending = true;
        owner.authorize(id, *label != '\0' ? label : "unidentified application", persistMode);
      }

      void approve(const InputCaptureGrant& grant) {
        approved = true;
        grantId = grant.id;
        if (grant.persistMode != 0 && wl_resource_get_version(resource) >= 2) {
          umbriel_input_capture_session_v1_send_grant(resource, startRequest, grant.persistMode, grant.token.c_str());
        }
        umbriel_input_capture_session_v1_send_started(resource, startRequest, kSuccess, capabilities);
      }

      void commitBarriers(uint32_t request, uint32_t generation) {
        disable(true);
        barriers.clear();
        const bool current = usable() && !overflow && generation == owner.zoneSet && generation == lastZoneSet;
        for (const InputCaptureBarrier& barrier : pendingBarriers) {
          const bool duplicate = std::ranges::count(pendingBarriers, barrier.id, &InputCaptureBarrier::id) != 1;
          if (!current || duplicate || !validInputCaptureBarrier(owner.zones, barrier)) {
            umbriel_input_capture_session_v1_send_barrier_failed(resource, request, barrier.id);
          } else {
            barriers.push_back(barrier);
          }
        }
        pendingBarriers.clear();
        overflow = false;
        umbriel_input_capture_session_v1_send_barriers_done(resource, request, current ? kSuccess : kFailure);
      }
    };

    Server& server;
    wl_global* global = nullptr;
    AuthorizationRequest authorize;
    AuthorizationCancelled cancelled;
    ActiveChanged changed;
    Session* active = nullptr;
    uint64_t nextSession = 0;
    uint32_t nextActivation = 0;
    uint32_t zoneSet = 1;
    std::vector<InputCaptureZone> zones;
    std::vector<wlr_keyboard*> keyboards;
    std::vector<std::unique_ptr<Session>> sessions;
    std::vector<wl_resource*> managers;
    InputCapturePermissions permissionStore{InputCapturePermissions::defaultPath()};
    struct Peer {
      struct DestroyListener {
        wl_listener listener{};
        Peer* peer = nullptr;
      } destroy;
      Impl* owner = nullptr;
      wl_client* client = nullptr;
      std::unordered_map<std::string, uint64_t> scopes;
      ~Peer() { wl_list_remove(&destroy.listener.link); }
    };
    uint64_t nextPermissionPeer = 0;
    std::unordered_map<wl_client*, std::unique_ptr<Peer>> permissionPeers;

    uint64_t permissionPeer(wl_client* client, const std::string& scope, bool create = true) {
      auto found = permissionPeers.find(client);
      if (found == permissionPeers.end()) {
        if (!create) {
          return 0;
        }
        auto peer = std::make_unique<Peer>();
        peer->owner = this;
        peer->client = client;
        peer->destroy.peer = peer.get();
        peer->destroy.listener.notify = [](wl_listener* listener, void*) {
          Peer::DestroyListener* hook = nullptr;
          hook = wl_container_of(listener, hook, listener);
          Peer* peer = hook->peer;
          for (const auto& [name, id] : peer->scopes) {
            (void)name;
            peer->owner->permissionStore.expirePeer(id);
          }
          peer->owner->permissionPeers.erase(peer->client);
        };
        wl_client_add_destroy_listener(client, &peer->destroy.listener);
        found = permissionPeers.emplace(client, std::move(peer)).first;
      }
      if (!create) {
        const auto entry = found->second->scopes.find(scope);
        return entry != found->second->scopes.end() ? entry->second : 0;
      }
      auto& id = found->second->scopes[scope];
      if (id == 0) {
        id = ++nextPermissionPeer;
      }
      return id;
    }

    void forgetPermissionScope(wl_client* client, const std::string& scope) {
      const auto found = permissionPeers.find(client);
      if (found != permissionPeers.end()) {
        found->second->scopes.erase(scope);
      }
    }

    void expireScope(wl_client* client, std::string_view scope) {
      const auto peer = permissionPeers.find(client);
      if (peer == permissionPeers.end()) {
        return;
      }
      const auto found = peer->second->scopes.find(std::string(scope));
      if (found == peer->second->scopes.end()) {
        return;
      }
      const uint64_t id = found->second;
      permissionStore.expirePeer(id);
      peer->second->scopes.erase(found);
      for (const auto& state : sessions) {
        if (state->permissionPeer == id) {
          state->close();
        }
      }
    }

    explicit Impl(Server& server) : server(server) {}
    ~Impl() {
      if (global != nullptr) {
        wl_global_destroy(global);
      }
      while (!sessions.empty()) {
        wl_resource_destroy(sessions.back()->resource);
      }
      while (!managers.empty()) {
        wl_resource_destroy(managers.back());
      }
      permissionPeers.clear();
    }

    [[nodiscard]] bool blocked() const {
      return server.stopping()
          || server.sessionLocked()
          || server.sessionLock() != nullptr
          || (server.session() != nullptr && !server.session()->active);
    }

    void initialize() {
      global = wl_global_create(
          server.display(), &umbriel_input_capture_manager_v1_interface, 2, this,
          [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            auto* self = static_cast<Impl*>(data);
            wl_resource* resource =
                wl_resource_create(client, &umbriel_input_capture_manager_v1_interface, version, id);
            if (resource == nullptr) {
              wl_client_post_no_memory(client);
              return;
            }
            wl_resource_set_implementation(resource, &kManager, self, [](wl_resource* manager) {
              auto* owner = static_cast<Impl*>(wl_resource_get_user_data(manager));
              std::erase(owner->managers, manager);
            });
            self->managers.push_back(resource);
          }
      );
      if (global == nullptr) {
        kLog.error("failed to create the input capture global");
      }
    }

    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void createSession(wl_client* client, wl_resource* manager, uint32_t id) {
      auto* self = static_cast<Impl*>(wl_resource_get_user_data(manager));
      if (self->sessions.size() >= kMaxSessions) {
        wl_resource_post_error(manager, 0, "too many input capture sessions");
        return;
      }
      auto session = std::make_unique<Session>(*self);
      session->resource =
          wl_resource_create(client, &umbriel_input_capture_session_v1_interface, wl_resource_get_version(manager), id);
      if (session->resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      session->id = ++self->nextSession;
      wl_resource_set_implementation(session->resource, &kSession, session.get(), [](wl_resource* resource) {
        auto* session = static_cast<Session*>(wl_resource_get_user_data(resource));
        std::erase_if(session->owner.sessions, [session](const auto& item) { return item.get() == session; });
      });
      self->sessions.push_back(std::move(session));
    }

    static Session& session(wl_resource* resource) {
      return *static_cast<Session*>(wl_resource_get_user_data(resource));
    }

    static const struct umbriel_input_capture_manager_v1_interface kManager;
    static const struct umbriel_input_capture_session_v1_interface kSession;
  };

  const struct umbriel_input_capture_manager_v1_interface InputCapture::Impl::kManager{
      .destroy = destroy,
      .create_session = createSession,
      .expire_scope = [](wl_client* client, wl_resource* resource, const char* scope) {
        auto* owner = static_cast<Impl*>(wl_resource_get_user_data(resource));
        if (scope != nullptr && std::char_traits<char>::length(scope) <= 256) {
          owner->expireScope(client, scope);
        }
      },
  };

  const struct umbriel_input_capture_session_v1_interface InputCapture::Impl::kSession{
      .destroy = destroy,
      .start = [](wl_client*, wl_resource* resource, uint32_t request, const char* label,
                  uint32_t capabilities) { session(resource).start(request, label, capabilities); },
      .get_zones =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            auto& state = session(resource);
            if (state.usable()) {
              for (const auto& zone : state.owner.zones) {
                umbriel_input_capture_session_v1_send_zone(resource, request, zone.width, zone.height, zone.x, zone.y);
              }
              state.lastZoneSet = state.owner.zoneSet;
            }
            umbriel_input_capture_session_v1_send_zones_done(
                resource, request, state.usable() ? kSuccess : kFailure, state.owner.zoneSet
            );
          },
      .clear_barriers =
          [](wl_client*, wl_resource* resource) {
            auto& state = session(resource);
            state.disable(true);
            state.barriers.clear();
            state.pendingBarriers.clear();
            state.overflow = false;
          },
      .add_barrier =
          [](wl_client*, wl_resource* resource, uint32_t id, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
            auto& state = session(resource);
            if (state.pendingBarriers.size() >= kMaxBarriers) {
              state.overflow = true;
              return;
            }
            state.pendingBarriers.push_back({.id = id, .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2});
          },
      .commit_barriers = [](wl_client*, wl_resource* resource, uint32_t request,
                            uint32_t generation) { session(resource).commitBarriers(request, generation); },
      .connect_eis = [](wl_client*, wl_resource* resource, uint32_t request) { session(resource).connect(request); },
      .enable =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            auto& state = session(resource);
            const bool allowed = state.usable() && state.fdIssued;
            if (allowed) {
              state.enabled = true;
            }
            state.result(request, allowed);
          },
      .disable =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            auto& state = session(resource);
            state.disable(false);
            state.result(request, !state.closed && state.approved);
          },
      .release =
          [](wl_client*, wl_resource* resource, uint32_t request, uint32_t activation, uint32_t hasPosition,
             wl_fixed_t x, wl_fixed_t y) {
            auto& state = session(resource);
            if (state.owner.active == &state && state.activation == activation) {
              if (hasPosition != 0) {
                const double targetX = wl_fixed_to_double(x);
                const double targetY = wl_fixed_to_double(y);
                const bool contained = std::ranges::any_of(state.owner.zones, [targetX, targetY](const auto& zone) {
                  return targetX >= zone.x
                      && targetY >= zone.y
                      && targetX < static_cast<double>(zone.x) + zone.width
                      && targetY < static_cast<double>(zone.y) + zone.height;
                });
                if (contained) {
                  wlr_cursor_warp_closest(state.owner.server.cursor()->wlr(), nullptr, targetX, targetY);
                }
              }
              state.deactivate(false);
            }
            state.result(request, !state.closed && state.approved);
          },
      .set_persistence =
          [](wl_client*, wl_resource* resource, uint32_t mode, const char* token, const char* instance) {
            auto& state = session(resource);
            if (state.startAttempted
                || mode > 2
                || token == nullptr
                || instance == nullptr
                || std::char_traits<char>::length(token) > 4096
                || std::char_traits<char>::length(instance) > 256) {
              state.close();
              return;
            }
            state.persistMode = mode;
            state.restoreToken = std::char_traits<char>::length(token) == 64 ? token : "";
            state.applicationInstance = instance;
          },
  };

  InputCapture::InputCapture(Server& server) : m_impl(std::make_unique<Impl>(server)) {
    m_impl->initialize();
    updateOutputs();
  }
  InputCapture::~InputCapture() = default;
  void InputCapture::setAuthorize(AuthorizationRequest request) { m_impl->authorize = std::move(request); }
  void InputCapture::setAuthorizationCancelled(AuthorizationCancelled cancelled) {
    m_impl->cancelled = std::move(cancelled);
  }
  void InputCapture::setActiveChanged(ActiveChanged changed) { m_impl->changed = std::move(changed); }
  bool InputCapture::active() const { return m_impl->active != nullptr; }
  uint32_t InputCapture::capabilities() const { return m_impl->active != nullptr ? m_impl->active->capabilities : 0; }

  void InputCapture::authorizeSession(uint64_t id, bool allowed, bool remember) {
    const auto found = std::ranges::find_if(m_impl->sessions, [id](const auto& state) { return state->id == id; });
    if (found == m_impl->sessions.end() || !(*found)->pending) {
      return;
    }
    auto& state = **found;
    state.pending = false;
    if (allowed && !m_impl->blocked()) {
      if (!remember) {
        state.approve({});
        return;
      }
      if (state.persistMode != 0) {
        wl_client* client = wl_resource_get_client(state.resource);
        const uint64_t previous = m_impl->permissionPeer(client, state.applicationInstance, false);
        const bool newScope = state.persistMode == 1 && previous == 0;
        if (state.persistMode == 1) {
          state.permissionPeer = m_impl->permissionPeer(client, state.applicationInstance);
        }
        const auto grant = m_impl->permissionStore.issue(
            state.application, state.capabilities, state.persistMode, state.permissionPeer
        );
        if (grant) {
          state.approve(*grant);
          return;
        }
        if (newScope) {
          m_impl->forgetPermissionScope(client, state.applicationInstance);
        }
        kLog.error("unable to save input capture permission; capture was not started");
      }
      umbriel_input_capture_session_v1_send_started(state.resource, state.startRequest, kFailure, 0);
      return;
    }
    state.approved = false;
    umbriel_input_capture_session_v1_send_started(
        state.resource, state.startRequest, state.approved ? kSuccess : kCancelled,
        state.approved ? state.capabilities : 0
    );
  }

  std::vector<InputCapturePermission> InputCapture::permissions() { return m_impl->permissionStore.list(); }

  bool InputCapture::revokePermission(std::string_view id) {
    const bool forgotten = m_impl->permissionStore.revoke(id);
    for (const auto& state : m_impl->sessions) {
      if (!state->grantId.empty() && (id == "all" || state->grantId == id)) {
        state->close();
      }
    }
    return forgotten;
  }

  void InputCapture::addKeyboard(wlr_keyboard* keyboard) {
    if (keyboard == nullptr || std::ranges::find(m_impl->keyboards, keyboard) != m_impl->keyboards.end()) {
      return;
    }
    m_impl->keyboards.push_back(keyboard);
    for (const auto& state : m_impl->sessions) {
      state->addKeyboard(keyboard);
    }
  }

  void InputCapture::removeKeyboard(wlr_keyboard* keyboard) {
    for (const auto& state : m_impl->sessions) {
      const auto found =
          std::ranges::find_if(state->keyboards, [keyboard](const auto& device) { return device->source == keyboard; });
      if (found != state->keyboards.end()) {
        state->disable(true);
        state->keyboards.erase(found);
      }
    }
    std::erase(m_impl->keyboards, keyboard);
  }

  void InputCapture::forgetAbsoluteDevice(const void* device) {
    for (const auto& state : m_impl->sessions) {
      state->absoluteTargets.erase(device);
    }
  }

  void InputCapture::updateKeymap(wlr_keyboard* keyboard) {
    if (std::ranges::find(m_impl->keyboards, keyboard) == m_impl->keyboards.end()) {
      return;
    }
    for (const auto& state : m_impl->sessions) {
      const auto found =
          std::ranges::find_if(state->keyboards, [keyboard](const auto& device) { return device->source == keyboard; });
      if (found != state->keyboards.end()
          && keyboard->keymap_string != nullptr
          && (*found)->keymap == keyboard->keymap_string) {
        continue;
      }
      if (found != state->keyboards.end()) {
        state->disable(true);
        state->keyboards.erase(found);
      }
      state->addKeyboard(keyboard);
    }
  }

  void InputCapture::updateOutputs() {
    std::vector<InputCaptureZone> zones;
    for (const auto& output : m_impl->server.outputs()) {
      if (!output->desktopEnabled() || !output->wlr()->enabled) {
        continue;
      }
      const wlr_box box = output->layoutBox();
      if (box.width > 0 && box.height > 0) {
        zones.push_back(
            {.x = box.x,
             .y = box.y,
             .width = static_cast<uint32_t>(box.width),
             .height = static_cast<uint32_t>(box.height),
             .physicalScale = output->wlr()->scale}
        );
      }
    }
    std::ranges::sort(zones, [](const auto& first, const auto& second) {
      return std::tie(first.x, first.y, first.width, first.height, first.physicalScale)
          < std::tie(second.x, second.y, second.width, second.height, second.physicalScale);
    });
    if (zones == m_impl->zones) {
      return;
    }
    const uint32_t invalidated = m_impl->zoneSet;
    if (++m_impl->zoneSet == 0) {
      ++m_impl->zoneSet;
    }
    m_impl->zones = std::move(zones);
    for (const auto& state : m_impl->sessions) {
      state->disable(true);
      state->barriers.clear();
      // Regions are immutable after device publication. Reannounce the pointer
      // so EI clients replace their desktop bounds when the outputs change.
      state->clearPointer();
      state->createPointer();
      if (state->approved && !state->closed) {
        umbriel_input_capture_session_v1_send_zones_changed(state->resource, invalidated);
      }
    }
  }

  void InputCapture::suspend() {
    for (const auto& state : m_impl->sessions) {
      state->disable(true);
      if (state->pending) {
        state->pending = false;
        if (m_impl->cancelled) {
          m_impl->cancelled(state->id);
        }
        umbriel_input_capture_session_v1_send_started(state->resource, state->startRequest, kCancelled, 0);
      }
    }
  }

  bool InputCapture::captureMotion(uint32_t, double x, double y, double dx, double dy, bool canActivate) {
    if (!std::isfinite(x)
        || !std::isfinite(y)
        || !std::isfinite(dx)
        || !std::isfinite(dy)
        || !std::isfinite(x + dx)
        || !std::isfinite(y + dy)) {
      return false;
    }
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active != nullptr) {
      auto& state = *m_impl->active;
      if ((state.capabilities & kPointer) == 0) {
        return false;
      }
      state.absoluteTargets.clear();
      if (state.pointer != nullptr) {
        eis_device_pointer_motion(state.pointer, dx, dy);
      }
      return true;
    }
    if (!canActivate) {
      return false;
    }
    Impl::Session* chosen = nullptr;
    std::optional<InputCaptureCrossing> crossing;
    for (const auto& state : m_impl->sessions) {
      if (!state->enabled || !state->usable() || !state->ready()) {
        continue;
      }
      const auto candidate = inputCaptureCrossing(m_impl->zones, state->barriers, x, y, dx, dy);
      if (candidate && (!crossing || candidate->fraction < crossing->fraction)) {
        chosen = state.get();
        crossing = candidate;
      }
    }
    if (chosen == nullptr) {
      return false;
    }
    wlr_cursor_warp_closest(
        m_impl->server.cursor()->wlr(), nullptr, x + crossing->fraction * dx, y + crossing->fraction * dy
    );
    chosen->activate(crossing->id, x + dx, y + dy);
    // The activation position includes overshoot. Sending this triggering
    // delta as well would apply that movement twice at the remote edge.
    return (chosen->capabilities & kPointer) != 0;
  }

  bool InputCapture::captureAbsoluteMotion(
      uint32_t time, double x, double y, double targetX, double targetY, bool canActivate, const void* device
  ) {
    if (!std::isfinite(targetX) || !std::isfinite(targetY)) {
      return false;
    }
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active != nullptr && (m_impl->active->capabilities & kPointer) != 0) {
      auto& state = *m_impl->active;
      const auto previous = state.absoluteTargets.find(device);
      if (previous != state.absoluteTargets.end() && state.pointer != nullptr) {
        eis_device_pointer_motion(state.pointer, targetX - previous->second.first, targetY - previous->second.second);
      }
      // The first sample after switching devices establishes its baseline.
      // Subtracting the frozen local cursor would repeat all prior movement.
      if (previous == state.absoluteTargets.end() && state.absoluteTargets.size() >= 64) {
        state.absoluteTargets.erase(state.absoluteTargets.begin());
      }
      state.absoluteTargets[device] = {targetX, targetY};
      return true;
    }
    const bool captured = captureMotion(time, x, y, targetX - x, targetY - y, canActivate);
    if (captured && m_impl->active != nullptr) {
      m_impl->active->absoluteTargets[device] = {targetX, targetY};
    }
    return captured;
  }

  bool InputCapture::captureKey(uint32_t, wlr_keyboard* keyboard, uint32_t keycode, bool pressed) {
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active == nullptr || (m_impl->active->capabilities & kKeyboard) == 0) {
      return false;
    }
    auto& state = *m_impl->active;
    const auto found =
        std::ranges::find_if(state.keyboards, [keyboard](const auto& device) { return device->source == keyboard; });
    if (found != state.keyboards.end()) {
      auto& device = **found;
      if (pressed ? device.keys.insert(keycode).second : device.keys.erase(keycode) != 0) {
        eis_device_keyboard_key(device.handle, keycode, pressed);
        eis_device_frame(device.handle, eis_now(state.context));
      }
    }
    return true;
  }

  bool InputCapture::captureModifiers(wlr_keyboard* keyboard) {
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active == nullptr || (m_impl->active->capabilities & kKeyboard) == 0) {
      return false;
    }
    auto& state = *m_impl->active;
    for (const auto& device : state.keyboards) {
      if (device->source == keyboard) {
        state.sendModifiers(*device);
      }
    }
    return true;
  }

  bool InputCapture::captureButton(uint32_t, uint32_t button, bool pressed) {
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active == nullptr || (m_impl->active->capabilities & kPointer) == 0) {
      return false;
    }
    auto& state = *m_impl->active;
    if (state.pointer != nullptr
        && state.buttonsBound
        && (pressed ? state.buttons.insert(button).second : state.buttons.erase(button) != 0)) {
      eis_device_button_button(state.pointer, button, pressed);
    }
    return true;
  }

  bool InputCapture::captureAxis(uint32_t, uint32_t orientation, double delta, int32_t discrete) {
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active == nullptr || (m_impl->active->capabilities & kPointer) == 0) {
      return false;
    }
    auto& state = *m_impl->active;
    if (state.pointer != nullptr && state.scrollBound && std::isfinite(delta)) {
      const bool horizontal = orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
      if (delta != 0) {
        eis_device_scroll_delta(state.pointer, horizontal ? delta : 0, horizontal ? 0 : delta);
      }
      if (discrete != 0) {
        eis_device_scroll_discrete(state.pointer, horizontal ? discrete : 0, horizontal ? 0 : discrete);
      }
      if (delta == 0 && discrete == 0) {
        eis_device_scroll_stop(state.pointer, horizontal, !horizontal);
      }
    }
    return true;
  }

  bool InputCapture::captureFrame(uint32_t) {
    if (m_impl->blocked()) {
      suspend();
      return false;
    }
    if (m_impl->active == nullptr || (m_impl->active->capabilities & kPointer) == 0) {
      return false;
    }
    auto& state = *m_impl->active;
    if (state.pointer != nullptr) {
      eis_device_frame(state.pointer, eis_now(state.context));
    }
    return true;
  }

} // namespace umbriel
