// A real X11 window whose popup type and override-redirect flag can change
// while mapped. The client requests its small rectangle before the first map.

#include "parse_ints.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <xcb/xcb.h>

namespace {
  struct State {
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    xcb_window_t window = XCB_NONE;
    xcb_window_t sibling = XCB_NONE;
    xcb_window_t parent = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    xcb_gcontext_t parentGc = XCB_NONE;
    xcb_atom_t typeProperty = XCB_ATOM_NONE;
    xcb_atom_t normal = XCB_ATOM_NONE;
    xcb_atom_t menu = XCB_ATOM_NONE;
    xcb_atom_t popup = XCB_ATOM_NONE;
    xcb_atom_t dropdown = XCB_ATOM_NONE;
    xcb_atom_t notification = XCB_ATOM_NONE;
    xcb_atom_t utility = XCB_ATOM_NONE;
    xcb_atom_t wmProtocols = XCB_ATOM_NONE;
    xcb_atom_t wmDeleteWindow = XCB_ATOM_NONE;
    xcb_atom_t wmTakeFocus = XCB_ATOM_NONE;
    xcb_atom_t netPing = XCB_ATOM_NONE;
    xcb_atom_t clients = XCB_ATOM_NONE;
    xcb_atom_t stacking = XCB_ATOM_NONE;
    xcb_atom_t active = XCB_ATOM_NONE;
    xcb_atom_t netState = XCB_ATOM_NONE;
    xcb_atom_t maximizedHorz = XCB_ATOM_NONE;
    xcb_atom_t maximizedVert = XCB_ATOM_NONE;
    xcb_atom_t fullscreen = XCB_ATOM_NONE;
    xcb_atom_t utf8 = XCB_ATOM_NONE;
    xcb_atom_t netName = XCB_ATOM_NONE;
    uint16_t width = 300;
    uint16_t height = 160;
    uint16_t parentWidth = 640;
    uint16_t parentHeight = 360;
    std::string commands;
    bool running = true;
    bool failed = false;
  };

  void fail(State& state, std::string_view message) {
    std::println(stderr, "xwayland-popup-client: {}", message);
    state.failed = true;
    state.running = false;
  }

  bool check(State& state, xcb_void_cookie_t cookie) {
    xcb_generic_error_t* error = xcb_request_check(state.connection, cookie);
    if (error == nullptr) {
      return true;
    }
    std::println(stderr, "xwayland-popup-client: X error {} request={}", error->error_code, error->major_code);
    std::free(error);
    state.failed = true;
    state.running = false;
    return false;
  }

  xcb_atom_t internAtom(State& state, std::string_view name) {
    auto* reply = xcb_intern_atom_reply(
        state.connection, xcb_intern_atom(state.connection, false, static_cast<uint16_t>(name.size()), name.data()),
        nullptr
    );
    if (reply == nullptr) {
      fail(state, "failed to intern an atom");
      return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
  }

  bool setTitle(State& state, xcb_window_t window, std::string_view title) {
    return check(
               state,
               xcb_change_property_checked(
                   state.connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                   static_cast<uint32_t>(title.size()), title.data()
               )
           )
        && check(
               state,
               xcb_change_property_checked(
                   state.connection, XCB_PROP_MODE_REPLACE, window, state.netName, state.utf8, 8,
                   static_cast<uint32_t>(title.size()), title.data()
               )
        );
  }

  void fill(State& state, xcb_window_t window, xcb_gcontext_t gc, uint16_t width, uint16_t height) {
    const xcb_rectangle_t rectangle{0, 0, width, height};
    xcb_poly_fill_rectangle(state.connection, window, gc, 1, &rectangle);
    xcb_flush(state.connection);
  }

  void paint(State& state) { fill(state, state.window, state.gc, state.width, state.height); }
  void paintParent(State& state) { fill(state, state.parent, state.parentGc, state.parentWidth, state.parentHeight); }

  xcb_window_t propertyWindow(const State& state, xcb_atom_t property) {
    auto* reply = xcb_get_property_reply(
        state.connection,
        xcb_get_property(state.connection, false, state.screen->root, property, XCB_ATOM_WINDOW, 0, 1), nullptr
    );
    xcb_window_t window = XCB_NONE;
    if (reply != nullptr
        && reply->format == 32
        && reply->type == XCB_ATOM_WINDOW
        && xcb_get_property_value_length(reply) == static_cast<int>(sizeof(window))) {
      std::memcpy(&window, xcb_get_property_value(reply), sizeof(window));
    }
    std::free(reply);
    return window;
  }

  bool listed(const State& state, xcb_atom_t property) {
    auto* reply = xcb_get_property_reply(
        state.connection,
        xcb_get_property(state.connection, false, state.screen->root, property, XCB_ATOM_WINDOW, 0, 1024), nullptr
    );
    if (reply == nullptr) {
      return false;
    }
    bool found = false;
    if (reply->format == 32 && reply->type == XCB_ATOM_WINDOW) {
      const auto* windows = static_cast<const xcb_window_t*>(xcb_get_property_value(reply));
      const int count = xcb_get_property_value_length(reply) / static_cast<int>(sizeof(xcb_window_t));
      for (int index = 0; index < count; ++index) {
        found |= windows[index] == state.window;
      }
    }
    std::free(reply);
    return found;
  }

  void reportState(State& state, std::string_view label) {
    auto* attributes = xcb_get_window_attributes_reply(
        state.connection, xcb_get_window_attributes(state.connection, state.window), nullptr
    );
    auto* geometry =
        xcb_get_geometry_reply(state.connection, xcb_get_geometry(state.connection, state.window), nullptr);
    auto* focus = xcb_get_input_focus_reply(state.connection, xcb_get_input_focus(state.connection), nullptr);
    auto* type = xcb_get_property_reply(
        state.connection,
        xcb_get_property(state.connection, false, state.window, state.typeProperty, XCB_ATOM_ATOM, 0, 1), nullptr
    );
    auto* netState = xcb_get_property_reply(
        state.connection, xcb_get_property(state.connection, false, state.window, state.netState, XCB_ATOM_ATOM, 0, 32),
        nullptr
    );
    if (attributes == nullptr || geometry == nullptr || focus == nullptr || type == nullptr || netState == nullptr) {
      fail(state, "failed to query X11 window state");
    } else {
      xcb_atom_t atom = XCB_ATOM_NONE;
      if (type->type == XCB_ATOM_ATOM
          && type->format == 32
          && xcb_get_property_value_length(type) == static_cast<int>(sizeof(atom))) {
        std::memcpy(&atom, xcb_get_property_value(type), sizeof(atom));
      }
      const char* typeName = atom == state.normal ? "normal"
          : atom == state.menu                    ? "menu"
          : atom == state.popup                   ? "popup"
          : atom == state.dropdown                ? "dropdown"
          : atom == state.notification            ? "notification"
          : atom == state.utility                 ? "utility"
                                                  : "none";
      bool maximizedHorz = false, maximizedVert = false, fullscreen = false;
      if (netState->type == XCB_ATOM_ATOM && netState->format == 32) {
        const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(netState));
        const int count = xcb_get_property_value_length(netState) / static_cast<int>(sizeof(xcb_atom_t));
        for (int index = 0; index < count; ++index) {
          maximizedHorz |= atoms[index] == state.maximizedHorz;
          maximizedVert |= atoms[index] == state.maximizedVert;
          fullscreen |= atoms[index] == state.fullscreen;
        }
      }
      const xcb_window_t activeWindow = propertyWindow(state, state.active);
      std::println(
          "state label={} map={} override={} type={} x={} y={} w={} h={} input={} active={} listed={} stacked={} "
          "max_horz={} max_vert={} fullscreen={}",
          label, attributes->map_state, attributes->override_redirect != 0, typeName, geometry->x, geometry->y,
          geometry->width, geometry->height, focus->focus == state.window, activeWindow == state.window,
          listed(state, state.clients), listed(state, state.stacking), maximizedHorz, maximizedVert, fullscreen
      );
      std::println("focus label={} input={} active={}", label, focus->focus, activeWindow);
    }
    std::free(attributes);
    std::free(geometry);
    std::free(focus);
    std::free(type);
    std::free(netState);
  }

  void event(State& state, const xcb_generic_event_t& generic);

  void command(State& state, std::string_view line) {
    bool valid = true;
    if (line == "map") {
      check(state, xcb_map_window_checked(state.connection, state.window));
    } else if (line == "unmap") {
      check(state, xcb_unmap_window_checked(state.connection, state.window));
    } else if (line == "parent-map") {
      xcb_map_window(state.connection, state.parent);
      xcb_flush(state.connection);
    } else if (line == "transient") {
      check(
          state,
          xcb_change_property_checked(
              state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1,
              &state.parent
          )
      );
    } else if (line == "take-focus") {
      const std::array<xcb_atom_t, 3> protocols{state.wmDeleteWindow, state.wmTakeFocus, state.netPing};
      check(
          state,
          xcb_change_property_checked(
              state.connection, XCB_PROP_MODE_REPLACE, state.window, state.wmProtocols, XCB_ATOM_ATOM, 32,
              protocols.size(), protocols.data()
          )
      );
    } else if (line.starts_with("type ")) {
      const std::string_view name = line.substr(5);
      const xcb_atom_t type = name == "normal" ? state.normal
          : name == "menu"                     ? state.menu
          : name == "popup"                    ? state.popup
          : name == "dropdown"                 ? state.dropdown
          : name == "notification"             ? state.notification
          : name == "utility"                  ? state.utility
                                               : xcb_atom_t{XCB_ATOM_NONE};
      if (type == XCB_ATOM_NONE) {
        valid = false;
      } else {
        check(
            state,
            xcb_change_property_checked(
                state.connection, XCB_PROP_MODE_REPLACE, state.window, state.typeProperty, XCB_ATOM_ATOM, 32, 1, &type
            )
        );
      }
    } else if (line == "map-move" || line == "move-map") {
      // Keep map and placement in one flush, without a reply that serializes the two X11 requests.
      constexpr std::array<uint32_t, 4> geometry{930, 550, 300, 160};
      if (line == "map-move") {
        xcb_map_window(state.connection, state.window);
      }
      xcb_configure_window(
          state.connection, state.window,
          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
          geometry.data()
      );
      if (line == "move-map") {
        xcb_map_window(state.connection, state.window);
      }
      xcb_flush(state.connection);
    } else if (line == "override-direct") {
      // A client can place its popup in the same sequence that changes OR.
      // No intermediate ConfigureNotify exposes the old maximized geometry.
      const uint32_t overrideRedirect = 1;
      check(
          state,
          xcb_change_window_attributes_checked(
              state.connection, state.window, XCB_CW_OVERRIDE_REDIRECT, &overrideRedirect
          )
      );
      const uint32_t geometry[]{240, 190, 320, 180};
      check(
          state,
          xcb_configure_window_checked(
              state.connection, state.window,
              XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geometry
          )
      );
    } else if (line == "override true" || line == "override false") {
      const uint32_t overrideRedirect = line == "override true" ? 1U : 0U;
      check(
          state,
          xcb_change_window_attributes_checked(
              state.connection, state.window, XCB_CW_OVERRIDE_REDIRECT, &overrideRedirect
          )
      );
      if (overrideRedirect != 0) {
        // Changing the attribute alone emits no event. A real stack change
        // makes Xwayland emit ConfigureNotify with its new flag, preserving
        // the geometry last imposed by the compositor. No repaint is needed.
        const uint32_t stack[]{state.sibling, XCB_STACK_MODE_BELOW};
        check(
            state,
            xcb_configure_window_checked(
                state.connection, state.window, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, stack
            )
        );
      }
    } else if (line.starts_with("geometry ")) {
      const auto geometry = harness::parseInts<int, 4>(line.substr(9), ' ');
      if (!geometry || (*geometry)[2] < 1 || (*geometry)[2] > 65535 || (*geometry)[3] < 1 || (*geometry)[3] > 65535) {
        valid = false;
      } else {
        const std::array<uint32_t, 4> values{
            static_cast<uint32_t>((*geometry)[0]), static_cast<uint32_t>((*geometry)[1]),
            static_cast<uint32_t>((*geometry)[2]), static_cast<uint32_t>((*geometry)[3])
        };
        check(
            state,
            xcb_configure_window_checked(
                state.connection, state.window,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                values.data()
            )
        );
      }
    } else if (line.starts_with("class ") && line.size() > 6) {
      const std::string name(line.substr(6));
      const std::string wmClass = name + '\0' + name + '\0';
      check(
          state,
          xcb_change_property_checked(
              state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
              static_cast<uint32_t>(wmClass.size()), wmClass.data()
          )
      );
    } else if (line == "input true" || line == "input false") {
      const std::array<uint32_t, 9> hints{1, line == "input true" ? 1U : 0U};
      check(
          state,
          xcb_change_property_checked(
              state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32,
              hints.size(), hints.data()
          )
      );
    } else if (line.starts_with("sync ")) {
      auto* reply = xcb_get_input_focus_reply(state.connection, xcb_get_input_focus(state.connection), nullptr);
      if (reply == nullptr) {
        fail(state, "failed to synchronize X11 events");
        return;
      }
      std::free(reply);
      while (xcb_generic_event_t* pending = xcb_poll_for_event(state.connection)) {
        event(state, *pending);
        std::free(pending);
      }
      std::println("synced {}", line.substr(5));
      return;
    } else if (line.starts_with("title ")) {
      setTitle(state, state.window, line.substr(6));
    } else if (line == "focus") {
      // Override-redirect clients are responsible for their own X input focus.
      check(
          state,
          xcb_set_input_focus_checked(state.connection, XCB_INPUT_FOCUS_POINTER_ROOT, state.window, XCB_CURRENT_TIME)
      );
    } else if (line.starts_with("state ")) {
      reportState(state, line.substr(6));
      return;
    } else if (line == "quit") {
      state.running = false;
    } else {
      valid = false;
    }
    if (!valid) {
      fail(state, "invalid stdin command");
    } else if (!state.failed) {
      std::println("done {}", line);
    }
  }

  void readCommands(State& state) {
    std::array<char, 1024> buffer{};
    const ssize_t size = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (size == 0) {
      state.running = false;
    } else if (size < 0) {
      if (errno != EINTR && errno != EAGAIN) {
        fail(state, "failed to read stdin");
      }
    } else {
      state.commands.append(buffer.data(), static_cast<size_t>(size));
      size_t newline;
      while ((newline = state.commands.find('\n')) != std::string::npos) {
        const std::string line = state.commands.substr(0, newline);
        state.commands.erase(0, newline + 1);
        command(state, line);
      }
    }
  }

  void event(State& state, const xcb_generic_event_t& generic) {
    const uint8_t type = generic.response_type & 0x7FU;
    if (type == XCB_EXPOSE) {
      const auto& expose = reinterpret_cast<const xcb_expose_event_t&>(generic);
      if (expose.count == 0) {
        if (expose.window == state.parent) {
          paintParent(state);
        } else {
          paint(state);
        }
      }
    } else if (type == XCB_MAP_NOTIFY) {
      // A remap gets a new Wayland surface, even when X11 retains its pixels.
      paint(state);
      std::println("mapped");
    } else if (type == XCB_UNMAP_NOTIFY) {
      std::println("unmapped");
    } else if (type == XCB_CONFIGURE_NOTIFY) {
      const auto& configure = reinterpret_cast<const xcb_configure_notify_event_t&>(generic);
      std::println(
          "configure x={} y={} w={} h={} override={}", configure.x, configure.y, configure.width, configure.height,
          configure.override_redirect != 0
      );
      if (configure.event == state.parent) {
        if (state.parentWidth != configure.width || state.parentHeight != configure.height) {
          state.parentWidth = configure.width;
          state.parentHeight = configure.height;
          paintParent(state);
        }
      } else if (state.width != configure.width || state.height != configure.height) {
        state.width = configure.width;
        state.height = configure.height;
        paint(state);
      }
    } else if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
      const auto& key = reinterpret_cast<const xcb_key_press_event_t&>(generic);
      std::println(
          "key code={} state={} window={:#x}", key.detail, type == XCB_KEY_PRESS ? "pressed" : "released", key.event
      );
    } else if (type == 0) {
      const auto& error = reinterpret_cast<const xcb_generic_error_t&>(generic);
      std::println(stderr, "xwayland-popup-client: asynchronous X error {}", error.error_code);
      state.failed = true;
      state.running = false;
    }
  }
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::println(stderr, "usage: xwayland-popup-client TITLE");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  State state;
  state.connection = xcb_connect(nullptr, nullptr);
  if (state.connection == nullptr || xcb_connection_has_error(state.connection) != 0) {
    std::println(stderr, "xwayland-popup-client: failed to connect to DISPLAY");
    return 1;
  }
  state.screen = xcb_setup_roots_iterator(xcb_get_setup(state.connection)).data;
  state.typeProperty = internAtom(state, "_NET_WM_WINDOW_TYPE");
  state.normal = internAtom(state, "_NET_WM_WINDOW_TYPE_NORMAL");
  state.menu = internAtom(state, "_NET_WM_WINDOW_TYPE_MENU");
  state.popup = internAtom(state, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
  state.dropdown = internAtom(state, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  state.notification = internAtom(state, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
  state.utility = internAtom(state, "_NET_WM_WINDOW_TYPE_UTILITY");
  state.wmProtocols = internAtom(state, "WM_PROTOCOLS");
  state.wmDeleteWindow = internAtom(state, "WM_DELETE_WINDOW");
  state.wmTakeFocus = internAtom(state, "WM_TAKE_FOCUS");
  state.netPing = internAtom(state, "_NET_WM_PING");
  state.clients = internAtom(state, "_NET_CLIENT_LIST");
  state.stacking = internAtom(state, "_NET_CLIENT_LIST_STACKING");
  state.active = internAtom(state, "_NET_ACTIVE_WINDOW");
  state.netState = internAtom(state, "_NET_WM_STATE");
  state.maximizedHorz = internAtom(state, "_NET_WM_STATE_MAXIMIZED_HORZ");
  state.maximizedVert = internAtom(state, "_NET_WM_STATE_MAXIMIZED_VERT");
  state.fullscreen = internAtom(state, "_NET_WM_STATE_FULLSCREEN");
  state.utf8 = internAtom(state, "UTF8_STRING");
  state.netName = internAtom(state, "_NET_WM_NAME");
  if (state.failed || state.screen == nullptr) {
    xcb_disconnect(state.connection);
    return 1;
  }

  // This unmapped sibling lets an override-redirect client change its real
  // X stacking order without issuing a geometry request or adding content.
  state.sibling = xcb_generate_id(state.connection);
  check(
      state,
      xcb_create_window_checked(
          state.connection, XCB_COPY_FROM_PARENT, state.sibling, state.screen->root, -10, -10, 1, 1, 0,
          XCB_WINDOW_CLASS_INPUT_OUTPUT, state.screen->root_visual, 0, nullptr
      )
  );
  state.window = xcb_generate_id(state.connection);
  constexpr uint32_t color = 0x00306090;
  constexpr uint32_t events =
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;
  const uint32_t values[]{color, events};
  check(
      state,
      xcb_create_window_checked(
          state.connection, XCB_COPY_FROM_PARENT, state.window, state.screen->root, 200, 150, state.width, state.height,
          0, XCB_WINDOW_CLASS_INPUT_OUTPUT, state.screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values
      )
  );
  state.gc = xcb_generate_id(state.connection);
  check(state, xcb_create_gc_checked(state.connection, state.gc, state.window, XCB_GC_FOREGROUND, &color));
  setTitle(state, state.window, argv[1]);
  constexpr char wmClass[] = "umbriel-xwayland-popup\0UmbrielXwaylandPopup\0";
  check(
      state,
      xcb_change_property_checked(
          state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
          sizeof(wmClass) - 1, wmClass
      )
  );
  check(
      state,
      xcb_change_property_checked(
          state.connection, XCB_PROP_MODE_REPLACE, state.window, state.typeProperty, XCB_ATOM_ATOM, 32, 1, &state.normal
      )
  );
  const auto pid = static_cast<uint32_t>(getpid());
  check(
      state,
      xcb_change_property_checked(
          state.connection, XCB_PROP_MODE_REPLACE, state.window, internAtom(state, "_NET_WM_PID"), XCB_ATOM_CARDINAL,
          32, 1, &pid
      )
  );
  // A second top-level gives a fixture a real X11 transient parent. It stays unmapped until a check asks for it.
  state.parent = xcb_generate_id(state.connection);
  constexpr uint32_t parentColor = 0x00C08040;
  const uint32_t parentValues[]{parentColor, events};
  check(
      state,
      xcb_create_window_checked(
          state.connection, XCB_COPY_FROM_PARENT, state.parent, state.screen->root, 40, 40, state.parentWidth,
          state.parentHeight, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, state.screen->root_visual,
          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, parentValues
      )
  );
  state.parentGc = xcb_generate_id(state.connection);
  check(state, xcb_create_gc_checked(state.connection, state.parentGc, state.parent, XCB_GC_FOREGROUND, &parentColor));
  setTitle(state, state.parent, "xwayland-popup-parent");
  check(
      state,
      xcb_change_property_checked(
          state.connection, XCB_PROP_MODE_REPLACE, state.parent, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
          sizeof(wmClass) - 1, wmClass
      )
  );
  check(
      state,
      xcb_change_property_checked(
          state.connection, XCB_PROP_MODE_REPLACE, state.parent, state.typeProperty, XCB_ATOM_ATOM, 32, 1, &state.normal
      )
  );
  if (!state.failed) {
    std::println("ready window={}", state.window);
    std::println("parent={:#x}", state.parent);
  }

  while (state.running && !state.failed && xcb_connection_has_error(state.connection) == 0) {
    while (xcb_generic_event_t* pending = xcb_poll_for_event(state.connection)) {
      event(state, *pending);
      std::free(pending);
    }
    if (!state.running || state.failed) {
      break;
    }
    std::array<pollfd, 2> descriptors{{
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = xcb_get_file_descriptor(state.connection), .events = POLLIN, .revents = 0},
    }};
    if (poll(descriptors.data(), descriptors.size(), -1) < 0) {
      if (errno != EINTR) {
        fail(state, "poll failed");
      }
    } else if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
      readCommands(state);
    }
  }
  if (xcb_connection_has_error(state.connection) != 0) {
    state.failed = true;
  }
  xcb_free_gc(state.connection, state.gc);
  xcb_free_gc(state.connection, state.parentGc);
  xcb_destroy_window(state.connection, state.window);
  xcb_destroy_window(state.connection, state.parent);
  xcb_destroy_window(state.connection, state.sibling);
  xcb_disconnect(state.connection);
  return state.failed ? 1 : 0;
}
