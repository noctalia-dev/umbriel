// A real X11 client that paints only its window background. It starts unmapped,
// and never repaints on its own, so a later frame cannot rescue a missed map.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <xcb/xcb.h>

namespace {

  xcb_atom_t internAtom(xcb_connection_t* connection, std::string_view name) {
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, false, static_cast<uint16_t>(name.size()), name.data()), nullptr
    );
    const xcb_atom_t atom = reply != nullptr ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    std::free(reply);
    return atom;
  }

  bool listed(xcb_connection_t* connection, xcb_window_t root, xcb_atom_t property, xcb_window_t window) {
    xcb_get_property_reply_t* reply = xcb_get_property_reply(
        connection, xcb_get_property(connection, false, root, property, XCB_ATOM_WINDOW, 0, 1024), nullptr
    );
    if (reply == nullptr) {
      return false;
    }
    const auto* windows = static_cast<const xcb_window_t*>(xcb_get_property_value(reply));
    const int count = xcb_get_property_value_length(reply) / static_cast<int>(sizeof(xcb_window_t));
    bool found = false;
    for (int index = 0; index < count; ++index) {
      found |= windows[index] == window;
    }
    std::free(reply);
    return found;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc != 5 || (std::strcmp(argv[4], "managed") != 0 && std::strcmp(argv[4], "unmanaged") != 0)) {
    std::println(stderr, "usage: xwayland-association-client TITLE WIDTH HEIGHT managed|unmanaged");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
  if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
    std::println(stderr, "xwayland-association-client: cannot connect to DISPLAY");
    return 1;
  }
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  const xcb_window_t window = xcb_generate_id(connection);
  const auto width = static_cast<uint16_t>(std::stoi(argv[2]));
  const auto height = static_cast<uint16_t>(std::stoi(argv[3]));
  const std::array<uint32_t, 2> values{0x306090, std::strcmp(argv[4], "unmanaged") == 0 ? 1U : 0U};
  xcb_create_window(
      connection, XCB_COPY_FROM_PARENT, window, screen->root, 50, 50, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
      screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values.data()
  );
  const std::string_view title = argv[1];
  xcb_change_property(
      connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
      static_cast<uint32_t>(title.size()), title.data()
  );
  const xcb_atom_t clients = internAtom(connection, "_NET_CLIENT_LIST");
  const xcb_atom_t stacking = internAtom(connection, "_NET_CLIENT_LIST_STACKING");
  xcb_flush(connection);
  std::println("ready window={}", window);

  std::array<char, 128> line{};
  while (std::fgets(line.data(), static_cast<int>(line.size()), stdin) != nullptr) {
    std::string command = line.data();
    if (!command.empty() && command.back() == '\n') {
      command.pop_back();
    }
    if (command == "quit") {
      break;
    }
    if (command == "map" || command == "unmap") {
      if (command == "map") {
        xcb_map_window(connection, window);
        // Paint once per show, including a remap whose retained X11 backing
        // pixels otherwise need not damage the new Wayland surface.
        xcb_clear_area(connection, false, window, 0, 0, 0, 0);
      } else {
        xcb_unmap_window(connection, window);
      }
      xcb_flush(connection);
      std::println("{}-requested", command);
    } else if (command.starts_with("state ")) {
      xcb_get_window_attributes_reply_t* attributes =
          xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), nullptr);
      if (attributes == nullptr) {
        std::println(stderr, "xwayland-association-client: cannot read window attributes");
        xcb_disconnect(connection);
        return 1;
      }
      std::println(
          "state label={} map_state={} listed={} stacked={}", command.substr(6), attributes->map_state,
          listed(connection, screen->root, clients, window), listed(connection, screen->root, stacking, window)
      );
      std::free(attributes);
    } else {
      std::println(stderr, "xwayland-association-client: unknown command '{}'", command);
      xcb_disconnect(connection);
      return 2;
    }
  }
  xcb_disconnect(connection);
  return 0;
}
