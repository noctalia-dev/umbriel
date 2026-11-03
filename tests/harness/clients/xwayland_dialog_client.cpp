// A globally active X11 application with a parent and an independently mapped
// transient dialog. The dialog intentionally has no EWMH MODAL state, like AWT.

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <xcb/xcb.h>

namespace {
  struct Window {
    xcb_window_t id = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    uint16_t width = 0;
    uint16_t height = 0;
    std::string_view name;
  };

  struct State {
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    xcb_atom_t utf8String = XCB_ATOM_NONE;
    xcb_atom_t netWmName = XCB_ATOM_NONE;
    xcb_atom_t netActiveWindow = XCB_ATOM_NONE;
    xcb_atom_t wmProtocols = XCB_ATOM_NONE;
    xcb_atom_t wmTakeFocus = XCB_ATOM_NONE;
    xcb_atom_t wmDeleteWindow = XCB_ATOM_NONE;
    std::array<Window, 2> windows{{
        {.width = 800, .height = 600, .name = "xwayland-dialog-parent"},
        {.width = 480, .height = 320, .name = "xwayland-dialog-child"},
    }};
    std::string commands;
    bool running = true;
  };

  xcb_atom_t internAtom(const State& state, std::string_view name) {
    const auto cookie = xcb_intern_atom(state.connection, false, static_cast<uint16_t>(name.size()), name.data());
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(state.connection, cookie, nullptr);
    if (reply == nullptr) {
      return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
  }

  Window* findWindow(State& state, xcb_window_t id) {
    for (Window& window : state.windows) {
      if (window.id == id) {
        return &window;
      }
    }
    return nullptr;
  }

  std::string windowName(const State& state, xcb_window_t id) {
    for (const Window& window : state.windows) {
      if (window.id == id) {
        return std::string(window.name);
      }
    }
    return id == XCB_NONE ? "none" : std::format("other:0x{:x}", id);
  }

  void paint(const State& state, const Window& window) {
    const xcb_rectangle_t rectangle{0, 0, window.width, window.height};
    xcb_poly_fill_rectangle(state.connection, window.id, window.gc, 1, &rectangle);
    xcb_flush(state.connection);
  }

  bool createWindow(State& state, size_t index) {
    Window& window = state.windows[index];
    window.id = xcb_generate_id(state.connection);
    const uint32_t color = index == 0 ? 0x00306090U : 0x0060C030U;
    constexpr uint32_t events = XCB_EVENT_MASK_EXPOSURE
        | XCB_EVENT_MASK_STRUCTURE_NOTIFY
        | XCB_EVENT_MASK_FOCUS_CHANGE
        | XCB_EVENT_MASK_KEY_PRESS
        | XCB_EVENT_MASK_KEY_RELEASE
        | XCB_EVENT_MASK_BUTTON_PRESS
        | XCB_EVENT_MASK_BUTTON_RELEASE;
    const uint32_t values[]{color, events};
    xcb_create_window(
        state.connection, XCB_COPY_FROM_PARENT, window.id, state.screen->root, 80, 80, window.width, window.height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, state.screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values
    );
    window.gc = xcb_generate_id(state.connection);
    xcb_create_gc(state.connection, window.gc, window.id, XCB_GC_FOREGROUND, &color);
    for (const auto& [property, type] :
         {std::pair{xcb_atom_t{XCB_ATOM_WM_NAME}, xcb_atom_t{XCB_ATOM_STRING}},
          std::pair{state.netWmName, state.utf8String}}) {
      xcb_change_property(
          state.connection, XCB_PROP_MODE_REPLACE, window.id, property, type, 8,
          static_cast<uint32_t>(window.name.size()), window.name.data()
      );
    }
    constexpr char wmClass[] = "umbriel-xwayland-dialog\0UmbrielXwaylandDialog\0";
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, window.id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wmClass) - 1,
        wmClass
    );
    const std::array<uint32_t, 9> hints{1, 0, 0, 0, 0, 0, 0, 0, 0};
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, window.id, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, hints.size(),
        hints.data()
    );
    const std::array protocols{state.wmTakeFocus, state.wmDeleteWindow};
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, window.id, state.wmProtocols, XCB_ATOM_ATOM, 32, protocols.size(),
        protocols.data()
    );
    const auto pid = static_cast<uint32_t>(getpid());
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, window.id, internAtom(state, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1,
        &pid
    );
    const xcb_atom_t type = internAtom(state, index == 0 ? "_NET_WM_WINDOW_TYPE_NORMAL" : "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, window.id, internAtom(state, "_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, 32,
        1, &type
    );
    if (index != 0) {
      xcb_change_property(
          state.connection, XCB_PROP_MODE_REPLACE, window.id, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
          &state.windows[0].id
      );
    }
    return xcb_connection_has_error(state.connection) == 0;
  }

  void reportState(const State& state, std::string_view label) {
    const auto focusCookie = xcb_get_input_focus(state.connection);
    xcb_get_input_focus_reply_t* focus = xcb_get_input_focus_reply(state.connection, focusCookie, nullptr);
    const xcb_window_t input = focus != nullptr ? focus->focus : XCB_NONE;
    std::free(focus);
    const auto activeCookie =
        xcb_get_property(state.connection, false, state.screen->root, state.netActiveWindow, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t* active = xcb_get_property_reply(state.connection, activeCookie, nullptr);
    xcb_window_t activeWindow = XCB_NONE;
    if (active != nullptr
        && active->type == XCB_ATOM_WINDOW
        && active->format == 32
        && xcb_get_property_value_length(active) == static_cast<int>(sizeof(activeWindow))) {
      std::memcpy(&activeWindow, xcb_get_property_value(active), sizeof(activeWindow));
    }
    std::free(active);
    std::println("state label={} input={} active={}", label, windowName(state, input), windowName(state, activeWindow));
  }

  void readCommands(State& state) {
    std::array<char, 1024> buffer{};
    const ssize_t size = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (size == 0) {
      state.running = false;
      return;
    }
    if (size < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        state.running = false;
      }
      return;
    }
    state.commands.append(buffer.data(), static_cast<size_t>(size));
    size_t newline;
    while ((newline = state.commands.find('\n')) != std::string::npos) {
      const std::string command = state.commands.substr(0, newline);
      state.commands.erase(0, newline + 1);
      if (command == "map-dialog") {
        xcb_map_window(state.connection, state.windows[1].id);
        xcb_flush(state.connection);
        std::println("map-dialog-requested");
      } else if (command == "unmap-dialog") {
        xcb_unmap_window(state.connection, state.windows[1].id);
        xcb_flush(state.connection);
        std::println("unmap-dialog-requested");
      } else if (command.starts_with("state ")) {
        reportState(state, std::string_view(command).substr(6));
      } else if (command == "quit") {
        state.running = false;
      } else if (!command.empty()) {
        std::println(stderr, "xwayland-dialog-client: unknown command '{}'", command);
        state.running = false;
      }
    }
  }

  void handleEvent(State& state, const xcb_generic_event_t& event) {
    const uint8_t type = event.response_type & 0x7FU;
    if (type == XCB_EXPOSE) {
      const auto& expose = reinterpret_cast<const xcb_expose_event_t&>(event);
      if (Window* window = findWindow(state, expose.window); window != nullptr && expose.count == 0) {
        paint(state, *window);
      }
    } else if (type == XCB_CONFIGURE_NOTIFY) {
      const auto& configure = reinterpret_cast<const xcb_configure_notify_event_t&>(event);
      if (Window* window = findWindow(state, configure.window)) {
        window->width = configure.width;
        window->height = configure.height;
        paint(state, *window);
      }
    } else if (type == XCB_CLIENT_MESSAGE) {
      const auto& message = reinterpret_cast<const xcb_client_message_event_t&>(event);
      if (message.type != state.wmProtocols) {
        return;
      }
      if (message.data.data32[0] == state.wmTakeFocus) {
        // Input=False requires the client to accept WM_TAKE_FOCUS itself.
        xcb_set_input_focus(state.connection, XCB_INPUT_FOCUS_POINTER_ROOT, message.window, message.data.data32[1]);
        xcb_flush(state.connection);
        std::println("take-focus window={}", windowName(state, message.window));
      } else if (message.data.data32[0] == state.wmDeleteWindow) {
        if (message.window == state.windows[1].id) {
          xcb_unmap_window(state.connection, message.window);
          xcb_flush(state.connection);
        } else {
          state.running = false;
        }
      }
    } else if (type == XCB_FOCUS_IN || type == XCB_FOCUS_OUT) {
      const auto& focus = reinterpret_cast<const xcb_focus_in_event_t&>(event);
      std::println("focus {} window={}", type == XCB_FOCUS_IN ? "in" : "out", windowName(state, focus.event));
    } else if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
      const auto& key = reinterpret_cast<const xcb_key_press_event_t&>(event);
      std::println(
          "key window={} code={} state={}", windowName(state, key.event), key.detail,
          type == XCB_KEY_PRESS ? "pressed" : "released"
      );
    } else if (type == XCB_BUTTON_PRESS || type == XCB_BUTTON_RELEASE) {
      const auto& button = reinterpret_cast<const xcb_button_press_event_t&>(event);
      std::println(
          "button window={} code={} state={}", windowName(state, button.event), button.detail,
          type == XCB_BUTTON_PRESS ? "pressed" : "released"
      );
    } else if (type == 0) {
      const auto& error = reinterpret_cast<const xcb_generic_error_t&>(event);
      std::println(stderr, "xwayland-dialog-client: X error {} request={}", error.error_code, error.major_code);
      state.running = false;
    }
  }
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  State state;
  int screenNumber = 0;
  state.connection = xcb_connect(nullptr, &screenNumber);
  if (state.connection == nullptr || xcb_connection_has_error(state.connection) != 0) {
    std::println(stderr, "xwayland-dialog-client: failed to connect to DISPLAY");
    return 1;
  }
  auto screens = xcb_setup_roots_iterator(xcb_get_setup(state.connection));
  for (int index = 0; index < screenNumber && screens.rem != 0; ++index) {
    xcb_screen_next(&screens);
  }
  state.screen = screens.data;
  state.utf8String = internAtom(state, "UTF8_STRING");
  state.netWmName = internAtom(state, "_NET_WM_NAME");
  state.netActiveWindow = internAtom(state, "_NET_ACTIVE_WINDOW");
  state.wmProtocols = internAtom(state, "WM_PROTOCOLS");
  state.wmTakeFocus = internAtom(state, "WM_TAKE_FOCUS");
  state.wmDeleteWindow = internAtom(state, "WM_DELETE_WINDOW");
  if (state.screen == nullptr
      || state.utf8String == XCB_ATOM_NONE
      || state.netWmName == XCB_ATOM_NONE
      || state.netActiveWindow == XCB_ATOM_NONE
      || state.wmProtocols == XCB_ATOM_NONE
      || state.wmTakeFocus == XCB_ATOM_NONE
      || state.wmDeleteWindow == XCB_ATOM_NONE
      || !createWindow(state, 0)
      || !createWindow(state, 1)) {
    std::println(stderr, "xwayland-dialog-client: failed to create windows");
    xcb_disconnect(state.connection);
    return 1;
  }
  xcb_map_window(state.connection, state.windows[0].id);
  xcb_flush(state.connection);
  std::println("ready parent=0x{:x} dialog=0x{:x}", state.windows[0].id, state.windows[1].id);

  while (state.running && xcb_connection_has_error(state.connection) == 0) {
    // Round-trip state queries may already have read and queued X events.
    // Drain those before poll so WM_TAKE_FOCUS never waits for unrelated I/O.
    while (xcb_generic_event_t* event = xcb_poll_for_event(state.connection)) {
      handleEvent(state, *event);
      std::free(event);
    }
    if (!state.running) {
      break;
    }
    std::array<pollfd, 2> descriptors{{
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = xcb_get_file_descriptor(state.connection), .events = POLLIN, .revents = 0},
    }};
    const int result = poll(descriptors.data(), descriptors.size(), -1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::println(stderr, "xwayland-dialog-client: poll failed: {}", std::strerror(errno));
      break;
    }
    if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
      readCommands(state);
    }
  }
  for (const Window& window : state.windows) {
    xcb_free_gc(state.connection, window.gc);
    xcb_destroy_window(state.connection, window.id);
  }
  xcb_disconnect(state.connection);
  return state.running ? 1 : 0;
}
