#include "input/eis.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/eis_socket.h"
#include "input/event_time.h"
#include "input/keyboard.h"
#include "input/keyboard_layout.h"
#include "input/seat.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "wlr.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <libeis.h>
#include <limits>
#include <linux/input-event-codes.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
}

namespace umbriel {

  namespace {
    constexpr Logger kLog("eis");
    constexpr wlr_pointer_impl kPointerImpl{.name = "umbriel-eis"};
    constexpr wlr_keyboard_impl kKeyboardImpl{.name = "umbriel-eis", .led_update = nullptr};
    constexpr std::array kCapabilities{
        EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_KEYBOARD,
        EIS_DEVICE_CAP_SCROLL,  EIS_DEVICE_CAP_BUTTON,
    };
  } // namespace

  struct EisInput::Impl {
    struct Device {
      Impl& owner;
      eis_device* handle = nullptr;
      wlr_pointer pointer{};
      wlr_keyboard keyboard{};
      bool pointerInitialized = false;
      bool keyboardInitialized = false;
      bool absolute = false;
      bool emulating = false;
      bool advertised = false;
      std::unordered_set<uint32_t> keys;
      std::unordered_set<uint32_t> buttons;
      double scrollX = 0;
      double scrollY = 0;
      int32_t discreteX = 0;
      int32_t discreteY = 0;
      bool stopX = false;
      bool stopY = false;

      explicit Device(Impl& owner) : owner(owner) {}
      ~Device() {
        release();
        if (handle != nullptr) {
          eis_device_set_user_data(handle, nullptr);
          if (advertised) {
            eis_device_remove(handle);
          }
          eis_device_unref(handle);
        }
        if (keyboardInitialized) {
          wlr_keyboard_finish(&keyboard);
        }
        if (pointerInitialized) {
          wlr_pointer_finish(&pointer);
        }
      }

      void key(uint32_t code, bool pressed) {
        if (!keyboardInitialized
            || code > KEY_MAX
            || (pressed && keys.size() >= WLR_KEYBOARD_KEYS_CAP)
            || (pressed ? !keys.insert(code).second : keys.erase(code) == 0)) {
          return;
        }
        wlr_keyboard_key_event event{
            .time_msec = monotonicMsec(),
            .keycode = code,
            .update_state = true,
            .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
        };
        wlr_keyboard_notify_key(&keyboard, &event);
      }

      void button(uint32_t code, bool pressed) {
        if (!pointerInitialized
            || code < BTN_MISC
            || code > KEY_MAX
            || (pressed && buttons.size() >= WLR_POINTER_BUTTONS_CAP)
            || (pressed ? !buttons.insert(code).second : buttons.erase(code) == 0)) {
          return;
        }
        wlr_pointer_button_event event{
            .pointer = &pointer,
            .time_msec = monotonicMsec(),
            .button = code,
            .state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
        };
        wlr_pointer_notify_button(&pointer, &event);
      }

      void release() {
        if (owner.shuttingDown) {
          keys.clear();
          buttons.clear();
          return;
        }
        while (!keys.empty()) {
          key(*keys.begin(), false);
        }
        while (!buttons.empty()) {
          button(*buttons.begin(), false);
        }
        if (pointerInitialized) {
          wl_signal_emit_mutable(&pointer.events.frame, nullptr);
        }
        scrollX = scrollY = 0;
        discreteX = discreteY = 0;
        stopX = stopY = false;
        emulating = false;
      }

      void frame() {
        if (pointerInitialized) {
          const auto axis = [this](wl_pointer_axis orientation, double delta, int32_t discrete, bool stopped) {
            if (delta == 0 && discrete == 0 && !stopped) {
              return;
            }
            wlr_pointer_axis_event event{
                .pointer = &pointer,
                .time_msec = monotonicMsec(),
                .source = discrete != 0 ? WL_POINTER_AXIS_SOURCE_WHEEL : WL_POINTER_AXIS_SOURCE_CONTINUOUS,
                .orientation = orientation,
                .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
                .delta = delta != 0 ? delta : discrete * (15.0 / WLR_POINTER_AXIS_DISCRETE_STEP),
                .delta_discrete = discrete,
            };
            wl_signal_emit_mutable(&pointer.events.axis, &event);
          };
          axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, scrollX, discreteX, stopX);
          axis(WL_POINTER_AXIS_VERTICAL_SCROLL, scrollY, discreteY, stopY);
          wl_signal_emit_mutable(&pointer.events.frame, nullptr);
        }
        scrollX = scrollY = 0;
        discreteX = discreteY = 0;
        stopX = stopY = false;
        if (keyboardInitialized) {
          const auto& mods = keyboard.modifiers;
          eis_device_keyboard_send_xkb_modifiers(handle, mods.depressed, mods.latched, mods.locked, mods.group);
        }
      }
    };

    struct Client {
      uint64_t id = 0;
      eis_client* handle = nullptr;
      eis_seat* seat = nullptr;
      bool approved = false;
      uint32_t capabilities = 0;
      std::vector<std::unique_ptr<Device>> devices;

      ~Client() {
        devices.clear();
        if (seat != nullptr) {
          eis_seat_set_user_data(seat, nullptr);
          eis_seat_remove(seat);
          eis_seat_unref(seat);
        }
        eis_client_set_user_data(handle, nullptr);
        eis_client_disconnect(handle);
        eis_client_unref(handle);
      }
    };

    EisInput& owner;
    Server& server;
    eis* context = nullptr;
    wl_event_source* eventSource = nullptr;
    wl_event_source* updateSource = nullptr;
    std::unique_ptr<EisSocket> socket;
    int socketFd = -1;
    uint64_t nextId = 0;
    bool locked = false;
    bool shuttingDown = false;
    bool updateKeyboards = false;
    bool updateAbsolute = false;
    PeerAuthentication authenticate;
    AuthorizationRequest authorize;
    AuthorizationCancelled cancelled;
    std::vector<std::unique_ptr<Client>> clients;

    Impl(EisInput& owner, Server& server) : owner(owner), server(server) {}
    ~Impl() {
      shuttingDown = true;
      if (eventSource != nullptr) {
        wl_event_source_remove(eventSource);
      }
      if (updateSource != nullptr) {
        wl_event_source_remove(updateSource);
      }
      clients.clear();
      socket.reset();
      if (context != nullptr) {
        eis_unref(context);
      }
    }

    void initialize() {
      context = eis_new(this);
      const int result = context == nullptr ? -ENOMEM : eis_setup_backend_fd(context);
      if (result != 0) {
        kLog.error("failed to create input emulation backend: {}", std::strerror(-result));
        return;
      }
      socket = std::make_unique<EisSocket>(
          wl_display_get_event_loop(server.display()),
          [this](pid_t pid) { return !locked && !server.sessionLocked() && authenticate && authenticate(pid); },
          [this] { return eis_backend_fd_add_client(context); }
      );
      socketFd = socket->fd();
      if (socketFd < 0) {
        return;
      }
      eventSource = wl_event_loop_add_fd(
          wl_display_get_event_loop(server.display()), eis_get_fd(context), WL_EVENT_READABLE,
          [](int, uint32_t, void* data) -> int {
            static_cast<Impl*>(data)->dispatch();
            return 0;
          },
          this
      );
      if (eventSource == nullptr) {
        socket.reset();
        socketFd = -1;
        kLog.error("failed to watch input emulation events");
      }
    }

    void requestClient(eis_client* handle) {
      const char* name = eis_client_get_name(handle);
      if (locked
          || server.sessionLocked()
          || !eis_client_is_sender(handle)
          || name == nullptr
          || *name == '\0'
          || !authorize) {
        eis_client_disconnect(handle);
        return;
      }
      auto client = std::make_unique<Client>();
      client->id = ++nextId;
      client->handle = eis_client_ref(handle);
      eis_client_set_user_data(handle, client.get());
      const uint64_t id = client->id;
      clients.push_back(std::move(client));
      authorize(id, name);
    }

    void approve(uint64_t id, bool allowed) {
      auto found = std::ranges::find_if(clients, [id](const auto& client) { return client->id == id; });
      if (found == clients.end() || (*found)->approved) {
        return;
      }
      Client& client = **found;
      if (!allowed || locked || server.sessionLocked()) {
        clients.erase(found);
        return;
      }
      client.approved = true;
      eis_client_connect(client.handle);
      client.seat = eis_client_new_seat(client.handle, "Umbriel desktop");
      eis_seat_set_user_data(client.seat, &client);
      for (const auto capability : kCapabilities) {
        eis_seat_configure_capability(client.seat, capability);
      }
      eis_seat_add(client.seat);
    }

    bool configureKeyboard(Device& device) {
      wlr_keyboard_init(&device.keyboard, &kKeyboardImpl, "Umbriel emulated keyboard");
      device.keyboardInitialized = true;
      wlr_keyboard* source = owner.sourceKeyboard();
      if (source != nullptr && source->keymap != nullptr) {
        wlr_keyboard_set_keymap(&device.keyboard, source->keymap);
        const auto& mods = source->modifiers;
        wlr_keyboard_notify_modifiers(&device.keyboard, 0, 0, mods.locked, mods.group);
      } else {
        const XkbNames names = resolveXkbNames(config().input.keyboard, nullptr, server.systemKeymap());
        const xkb_rule_names rules{
            .rules = nullptr,
            .model = names.model.empty() ? nullptr : names.model.c_str(),
            .layout = names.layout.empty() ? nullptr : names.layout.c_str(),
            .variant = names.variant.empty() ? nullptr : names.variant.c_str(),
            .options = names.options.empty() ? nullptr : names.options.c_str(),
        };
        xkb_context* xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap* keymap = xkbContext == nullptr
            ? nullptr
            : xkb_keymap_new_from_names(xkbContext, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap != nullptr) {
          wlr_keyboard_set_keymap(&device.keyboard, keymap);
          xkb_keymap_unref(keymap);
        }
        if (xkbContext != nullptr) {
          xkb_context_unref(xkbContext);
        }
      }
      if (device.keyboard.keymap == nullptr) {
        return false;
      }
      eis_keymap* keymap = eis_device_new_keymap(
          device.handle, EIS_KEYMAP_TYPE_XKB, device.keyboard.keymap_fd, device.keyboard.keymap_size
      );
      if (keymap == nullptr) {
        return false;
      }
      eis_keymap_add(keymap);
      eis_keymap_unref(keymap);
      owner.addKeyboard(&device.keyboard.base);
      return true;
    }

    void createDevice(Client& client, eis_device_capability capability) {
      auto device = std::make_unique<Device>(*this);
      device->handle = eis_seat_new_device(client.seat);
      eis_device_set_user_data(device->handle, device.get());
      eis_device_configure_type(device->handle, EIS_DEVICE_TYPE_VIRTUAL);
      eis_device_configure_capability(device->handle, capability);
      if (capability == EIS_DEVICE_CAP_KEYBOARD) {
        eis_device_configure_name(device->handle, "Umbriel keyboard");
        if (!configureKeyboard(*device)) {
          kLog.error("failed to configure input emulation keyboard");
          return;
        }
      } else {
        device->absolute = capability == EIS_DEVICE_CAP_POINTER_ABSOLUTE;
        eis_device_configure_name(device->handle, device->absolute ? "Umbriel absolute pointer" : "Umbriel pointer");
        for (const auto extra : {EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL}) {
          if ((client.capabilities & extra) != 0) {
            eis_device_configure_capability(device->handle, extra);
          }
        }
        if (device->absolute) {
          wlr_box layout{};
          wlr_output_layout_get_box(server.outputLayout(), nullptr, &layout);
          if (layout.width <= 0 || layout.height <= 0) {
            return;
          }
          wlr_output_layout_output* output;
          wl_list_for_each(output, &server.outputLayout()->outputs, link) {
            if (!output->output->enabled) {
              continue;
            }
            wlr_box box{};
            wlr_output_layout_get_box(server.outputLayout(), output->output, &box);
            if (box.width <= 0 || box.height <= 0) {
              continue;
            }
            Xwayland* xwayland = server.xwayland();
            if (xwayland == nullptr) {
              continue;
            }
            const auto mapped = xwayland->outputs().region(output->output, box.x, box.y);
            eis_region* region = eis_device_new_region(device->handle);
            eis_region_set_offset(region, mapped.x.x, mapped.x.y);
            eis_region_set_size(region, mapped.x.width, mapped.x.height);
            eis_region_set_physical_scale(region, 1.0);
            eis_region_set_mapping_id(region, output->output->name);
            eis_region_add(region);
            eis_region_unref(region);
          }
        }
        wlr_pointer_init(&device->pointer, &kPointerImpl, "Umbriel emulated pointer");
        device->pointerInitialized = true;
        owner.addPointer(&device->pointer.base);
      }
      eis_device_add(device->handle);
      device->advertised = true;
      eis_device_resume(device->handle);
      client.devices.push_back(std::move(device));
    }

    void rebuild(Client& client, bool keyboard, bool absolute, bool pointer) {
      std::erase_if(client.devices, [keyboard, absolute, pointer](const auto& device) {
        return device->keyboardInitialized ? keyboard : (device->absolute ? absolute : pointer);
      });
      for (const auto capability : {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_KEYBOARD}) {
        const bool replace = capability == EIS_DEVICE_CAP_KEYBOARD
            ? keyboard
            : (capability == EIS_DEVICE_CAP_POINTER_ABSOLUTE ? absolute : pointer);
        if (replace && (client.capabilities & capability) != 0) {
          createDevice(client, capability);
        }
      }
    }

    void scheduleUpdate(bool keyboard, bool absolute) {
      updateKeyboards = updateKeyboards || keyboard;
      updateAbsolute = updateAbsolute || absolute;
      if (updateSource != nullptr || shuttingDown) {
        return;
      }
      // A config or output action can run inside a native input notification. Rebuild after that signal returns.
      updateSource = wl_event_loop_add_idle(
          wl_display_get_event_loop(server.display()),
          [](void* data) {
            auto* self = static_cast<Impl*>(data);
            self->updateSource = nullptr;
            const bool keyboard = std::exchange(self->updateKeyboards, false);
            const bool absolute = std::exchange(self->updateAbsolute, false);
            for (const auto& client : self->clients) {
              if (client->approved) {
                self->rebuild(*client, keyboard, absolute, false);
              }
            }
          },
          this
      );
    }

    void dispatch() {
      eis_dispatch(context);
      while (eis_event* event = eis_get_event(context)) {
        handleEvent(event);
        eis_event_unref(event);
      }
    }

    void handleEvent(eis_event* event) {
      const auto type = eis_event_get_type(event);
      eis_client* handle = eis_event_get_client(event);
      if (handle == nullptr) {
        return;
      }
      if (type == EIS_EVENT_CLIENT_CONNECT) {
        requestClient(handle);
        return;
      }
      auto* client = static_cast<Client*>(eis_client_get_user_data(handle));
      if (client == nullptr) {
        return;
      }
      if (type == EIS_EVENT_CLIENT_DISCONNECT) {
        const uint64_t id = client->id;
        const bool pending = !client->approved;
        std::erase_if(clients, [client](const auto& entry) { return entry.get() == client; });
        if (pending && cancelled) {
          cancelled(id);
        }
        return;
      }
      if (type == EIS_EVENT_SEAT_BIND) {
        client->capabilities = 0;
        for (const auto capability : kCapabilities) {
          if (eis_event_seat_has_capability(event, capability)) {
            client->capabilities |= capability;
          }
        }
        rebuild(*client, true, true, true);
        return;
      }
      eis_device* eisDevice = eis_event_get_device(event);
      auto* device = eisDevice == nullptr ? nullptr : static_cast<Device*>(eis_device_get_user_data(eisDevice));
      if (device == nullptr) {
        return;
      }
      if (type == EIS_EVENT_DEVICE_CLOSED) {
        std::erase_if(client->devices, [device](const auto& entry) { return entry.get() == device; });
        return;
      }
      if (type == EIS_EVENT_DEVICE_STOP_EMULATING) {
        device->release();
        return;
      }
      if (type == EIS_EVENT_DEVICE_START_EMULATING) {
        device->release();
        device->emulating = true;
        return;
      }
      if (locked || server.sessionLocked()) {
        return;
      }
      if (server.quitConfirm() != nullptr && server.quitConfirm()->visible()) {
        const bool emulating = device->emulating;
        device->release();
        device->emulating = emulating;
        return;
      }
      if (!device->emulating) {
        return;
      }
      switch (type) {
      case EIS_EVENT_FRAME:
        device->frame();
        break;
      case EIS_EVENT_POINTER_MOTION: {
        double dx = eis_event_pointer_get_dx(event);
        double dy = eis_event_pointer_get_dy(event);
        if (!std::isfinite(dx) || !std::isfinite(dy)) {
          break;
        }
        if (Xwayland* xwayland = server.xwayland()) {
          const wlr_cursor* cursor = server.cursor()->wlr();
          wlr_output* output = wlr_output_layout_output_at(server.outputLayout(), cursor->x, cursor->y);
          const auto mapped = xwayland->outputs().region(output, cursor->x, cursor->y);
          dx /= mapped.scale;
          dy /= mapped.scale;
        }
        wlr_pointer_motion_event motion{
            .pointer = &device->pointer,
            .time_msec = monotonicMsec(),
            .delta_x = dx,
            .delta_y = dy,
            .unaccel_dx = dx,
            .unaccel_dy = dy,
        };
        wl_signal_emit_mutable(&device->pointer.events.motion, &motion);
        break;
      }
      case EIS_EVENT_POINTER_MOTION_ABSOLUTE: {
        const double x = eis_event_pointer_get_absolute_x(event);
        const double y = eis_event_pointer_get_absolute_y(event);
        wlr_box layout{};
        wlr_output_layout_get_box(server.outputLayout(), nullptr, &layout);
        if (!std::isfinite(x) || !std::isfinite(y) || layout.width <= 0 || layout.height <= 0) {
          break;
        }
        Xwayland* xwayland = server.xwayland();
        if (xwayland == nullptr
            || x < std::numeric_limits<int>::min()
            || x > std::numeric_limits<int>::max()
            || y < std::numeric_limits<int>::min()
            || y > std::numeric_limits<int>::max()) {
          break;
        }
        const auto mapped = xwayland->outputs().regionAtX(x, y);
        const double logicalX = mapped.layout.x + (x - mapped.x.x) / mapped.scale;
        const double logicalY = mapped.layout.y + (y - mapped.x.y) / mapped.scale;
        wlr_pointer_motion_absolute_event motion{
            .pointer = &device->pointer,
            .time_msec = monotonicMsec(),
            .x = (logicalX - layout.x) / layout.width,
            .y = (logicalY - layout.y) / layout.height,
        };
        wl_signal_emit_mutable(&device->pointer.events.motion_absolute, &motion);
        break;
      }
      case EIS_EVENT_BUTTON_BUTTON:
        device->button(eis_event_button_get_button(event), eis_event_button_get_is_press(event));
        break;
      case EIS_EVENT_KEYBOARD_KEY:
        device->key(eis_event_keyboard_get_key(event), eis_event_keyboard_get_key_is_press(event));
        break;
      case EIS_EVENT_SCROLL_DELTA: {
        const double x = device->scrollX + eis_event_scroll_get_dx(event);
        const double y = device->scrollY + eis_event_scroll_get_dy(event);
        if (std::isfinite(x)) {
          device->scrollX = x;
        }
        if (std::isfinite(y)) {
          device->scrollY = y;
        }
        break;
      }
      case EIS_EVENT_SCROLL_DISCRETE: {
        const auto sum = [](int32_t value, int32_t delta) {
          return static_cast<int32_t>(std::clamp<int64_t>(
              static_cast<int64_t>(value) + delta, std::numeric_limits<int32_t>::min(),
              std::numeric_limits<int32_t>::max()
          ));
        };
        device->discreteX = sum(device->discreteX, eis_event_scroll_get_discrete_dx(event));
        device->discreteY = sum(device->discreteY, eis_event_scroll_get_discrete_dy(event));
        break;
      }
      case EIS_EVENT_SCROLL_STOP:
      case EIS_EVENT_SCROLL_CANCEL:
        device->stopX = device->stopX || eis_event_scroll_get_stop_x(event);
        device->stopY = device->stopY || eis_event_scroll_get_stop_y(event);
        break;
      default:
        break;
      }
    }
  };

  EisInput::EisInput(Server& server) : m_impl(std::make_unique<Impl>(*this, server)) { m_impl->initialize(); }
  EisInput::~EisInput() = default;
  int EisInput::socketFd() const { return m_impl->socketFd; }
  bool EisInput::ownsDevice(const wlr_input_device* device) const {
    if (device == nullptr) {
      return false;
    }
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
      return wlr_keyboard_from_input_device(const_cast<wlr_input_device*>(device))->impl == &kKeyboardImpl;
    }
    if (device->type == WLR_INPUT_DEVICE_POINTER) {
      return wlr_pointer_from_input_device(const_cast<wlr_input_device*>(device))->impl == &kPointerImpl;
    }
    return false;
  }
  void EisInput::setAuthenticate(PeerAuthentication authenticate) { m_impl->authenticate = std::move(authenticate); }
  void EisInput::setAuthorize(AuthorizationRequest authorize) { m_impl->authorize = std::move(authorize); }
  void EisInput::setAuthorizationCancelled(AuthorizationCancelled cancelled) {
    m_impl->cancelled = std::move(cancelled);
  }
  void EisInput::authorizeClient(uint64_t id, bool allowed) { m_impl->approve(id, allowed); }
  void EisInput::applyConfig() { m_impl->scheduleUpdate(true, false); }
  void EisInput::updateOutputs() { m_impl->scheduleUpdate(false, true); }
  void EisInput::updateModifiers(const wlr_keyboard_modifiers& modifiers) {
    if (m_impl->locked || m_impl->server.sessionLocked()) {
      return;
    }
    for (const auto& client : m_impl->clients) {
      for (const auto& device : client->devices) {
        if (device->keyboardInitialized) {
          const auto current = device->keyboard.modifiers;
          if (current.locked != modifiers.locked || current.group != modifiers.group) {
            wlr_keyboard_notify_modifiers(
                &device->keyboard, current.depressed, current.latched, modifiers.locked, modifiers.group
            );
          }
          eis_device_keyboard_send_xkb_modifiers(
              device->handle, modifiers.depressed, modifiers.latched, modifiers.locked, modifiers.group
          );
        }
      }
    }
  }
  void EisInput::setSessionLocked(bool locked) {
    if (m_impl->locked == locked) {
      return;
    }
    m_impl->locked = locked;
    if (locked) {
      std::vector<uint64_t> pending;
      for (const auto& client : m_impl->clients) {
        if (!client->approved) {
          pending.push_back(client->id);
        }
      }
      std::erase_if(m_impl->clients, [](const auto& client) { return !client->approved; });
      if (m_impl->cancelled) {
        for (const uint64_t id : pending) {
          m_impl->cancelled(id);
        }
      }
    }
    for (const auto& client : m_impl->clients) {
      for (const auto& device : client->devices) {
        if (locked) {
          const bool emulating = device->emulating;
          device->release();
          device->emulating = emulating;
        }
      }
    }
  }
  void EisInput::addKeyboard(wlr_input_device* device) {
    m_impl->server.addKeyboard(device);
    m_impl->server.updateSeatCapabilities();
  }
  void EisInput::addPointer(wlr_input_device* device) { m_impl->server.addPointer(device); }
  wlr_keyboard* EisInput::sourceKeyboard() const {
    Keyboard* source = m_impl->server.m_keyboardLayoutSource;
    return source != nullptr ? source->wlr() : nullptr;
  }

} // namespace umbriel
