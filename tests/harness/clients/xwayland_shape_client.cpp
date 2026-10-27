// Maps an ARGB X11 window with two red widgets, setting its bounding shape
// before mapping so Xwayland's exported pixmap can contain opaque black outside
// the shape. Shape commands do not repaint: shrinking a region must clip pixels
// already present in the buffer, even when no new surface commit follows.
// Commands: `shape both|left|right|empty|clear`, `input empty|clear`, `resize`, and `quit`.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

namespace {
  constexpr uint16_t kWidth = 600;
  constexpr uint16_t kHeight = 400;
  constexpr std::array<xcb_rectangle_t, 2> kWidgets{{{50, 50, 200, 300}, {350, 50, 200, 300}}};
  constexpr xcb_rectangle_t kExpandedWidget{650, 50, 100, 300};
  constexpr size_t kMaxCommandBytes = 256;

  struct State {
    xcb_connection_t* connection = nullptr;
    xcb_window_t window = XCB_NONE;
    xcb_gcontext_t graphics = XCB_NONE;
    std::string commands;
    bool mapped = false;
    bool expanded = false;
    bool resized = false;
    bool running = true;
    bool failed = false;
  };

  void fail(State& state, std::string_view message) {
    std::println(stderr, "xwayland-shape-client: {}", message);
    state.failed = true;
    state.running = false;
  }

  bool checkRequest(State& state, xcb_void_cookie_t cookie, std::string_view operation) {
    xcb_generic_error_t* error = xcb_request_check(state.connection, cookie);
    if (error != nullptr) {
      std::println(stderr, "xwayland-shape-client: {} failed with X11 error {}", operation, error->error_code);
      std::free(error);
      state.failed = true;
      state.running = false;
      return false;
    }
    return true;
  }

  bool setRectangles(State& state, xcb_shape_kind_t kind, std::span<const xcb_rectangle_t> rectangles) {
    return checkRequest(
        state,
        xcb_shape_rectangles_checked(
            state.connection, XCB_SHAPE_SO_SET, kind, XCB_CLIP_ORDERING_UNSORTED, state.window, 0, 0,
            static_cast<uint32_t>(rectangles.size()), rectangles.data()
        ),
        "setting shape rectangles"
    );
  }

  bool clearShape(State& state, xcb_shape_kind_t kind) {
    return checkRequest(
        state, xcb_shape_mask_checked(state.connection, XCB_SHAPE_SO_SET, kind, state.window, 0, 0, XCB_NONE),
        "clearing a shape"
    );
  }

  void reportState(State& state, std::string_view label) {
    // Querying the region acknowledges that Xwayland has processed the command.
    auto* extents = xcb_shape_query_extents_reply(
        state.connection, xcb_shape_query_extents(state.connection, state.window), nullptr
    );
    auto* rectangles = xcb_shape_get_rectangles_reply(
        state.connection, xcb_shape_get_rectangles(state.connection, state.window, XCB_SHAPE_SK_BOUNDING), nullptr
    );
    if (extents == nullptr || rectangles == nullptr) {
      std::free(extents);
      std::free(rectangles);
      fail(state, "failed to query the bounding shape");
      return;
    }
    std::println(
        "done {} bounding={} rectangles={}", label, extents->bounding_shaped != 0,
        xcb_shape_get_rectangles_rectangles_length(rectangles)
    );
    std::free(extents);
    std::free(rectangles);
  }

  void paint(State& state) {
    checkRequest(
        state,
        xcb_poly_fill_rectangle_checked(
            state.connection, state.window, state.graphics, static_cast<uint32_t>(kWidgets.size()), kWidgets.data()
        ),
        "drawing widgets"
    );
    if (state.expanded) {
      checkRequest(
          state, xcb_poly_fill_rectangle_checked(state.connection, state.window, state.graphics, 1, &kExpandedWidget),
          "drawing the expanded widget"
      );
    }
  }

  xcb_visualid_t argbVisual(xcb_screen_t* screen) {
    for (auto depth = xcb_screen_allowed_depths_iterator(screen); depth.rem != 0; xcb_depth_next(&depth)) {
      if (depth.data->depth != 32) {
        continue;
      }
      for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem != 0; xcb_visualtype_next(&visual)) {
        if (visual.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR
            && visual.data->red_mask == 0xFF0000
            && visual.data->green_mask == 0x00FF00
            && visual.data->blue_mask == 0x0000FF) {
          return visual.data->visual_id;
        }
      }
    }
    return XCB_NONE;
  }

  bool createWindow(State& state, xcb_screen_t* screen, std::string_view title, bool unmanaged, bool shaped) {
    const xcb_visualid_t visual = argbVisual(screen);
    if (visual == XCB_NONE) {
      fail(state, "no 32-bit ARGB visual");
      return false;
    }
    const xcb_colormap_t colormap = xcb_generate_id(state.connection);
    if (!checkRequest(
            state,
            xcb_create_colormap_checked(state.connection, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root, visual),
            "creating a colormap"
        )) {
      return false;
    }
    state.window = xcb_generate_id(state.connection);
    const uint32_t values[]{
        0, 0, unmanaged ? 1U : 0U, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY, colormap
    };
    if (!checkRequest(
            state,
            xcb_create_window_checked(
                state.connection, 32, state.window, screen->root, 100, 100, kWidth, kHeight, 0,
                XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
                XCB_CW_BACK_PIXEL
                    | XCB_CW_BORDER_PIXEL
                    | XCB_CW_OVERRIDE_REDIRECT
                    | XCB_CW_EVENT_MASK
                    | XCB_CW_COLORMAP,
                values
            ),
            "creating an ARGB window"
        )
        || !checkRequest(
            state,
            xcb_change_property_checked(
                state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                static_cast<uint32_t>(title.size()), title.data()
            ),
            "setting the window title"
        )) {
      return false;
    }
    state.graphics = xcb_generate_id(state.connection);
    const uint32_t red = 0xFFFF0000;
    if (!checkRequest(
            state, xcb_create_gc_checked(state.connection, state.graphics, state.window, XCB_GC_FOREGROUND, &red),
            "creating a graphics context"
        )
        || (shaped && !setRectangles(state, XCB_SHAPE_SK_BOUNDING, kWidgets))) {
      return false;
    }
    return checkRequest(state, xcb_map_window_checked(state.connection, state.window), "mapping the window");
  }

  void handleCommand(State& state, std::string_view command) {
    bool valid = true;
    if (command == "shape both") {
      setRectangles(state, XCB_SHAPE_SK_BOUNDING, kWidgets);
    } else if (command == "shape left") {
      setRectangles(state, XCB_SHAPE_SK_BOUNDING, std::span(kWidgets).first(1));
    } else if (command == "shape right") {
      setRectangles(state, XCB_SHAPE_SK_BOUNDING, std::span(kWidgets).last(1));
    } else if (command == "shape empty") {
      setRectangles(state, XCB_SHAPE_SK_BOUNDING, {});
    } else if (command == "shape clear") {
      clearShape(state, XCB_SHAPE_SK_BOUNDING);
    } else if (command == "input empty") {
      setRectangles(state, XCB_SHAPE_SK_INPUT, {});
    } else if (command == "input clear") {
      clearShape(state, XCB_SHAPE_SK_INPUT);
    } else if (command == "resize") {
      state.expanded = true;
      const std::array<uint32_t, 2> size{800, 500};
      checkRequest(
          state,
          xcb_configure_window_checked(
              state.connection, state.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size.data()
          ),
          "resizing the window"
      );
    } else if (command == "quit") {
      state.running = false;
      return;
    } else if (command.empty()) {
      return;
    } else {
      valid = false;
    }
    if (!valid) {
      std::println(stderr, "xwayland-shape-client: unknown command '{}'", command);
      state.failed = true;
      state.running = false;
    } else if (state.running) {
      reportState(state, command);
    }
  }

  void readCommands(State& state) {
    std::array<char, kMaxCommandBytes> buffer{};
    const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        fail(state, "failed to read stdin");
      }
      return;
    }
    if (count == 0) {
      state.running = false;
      return;
    }
    state.commands.append(buffer.data(), static_cast<size_t>(count));
    size_t newline = std::string::npos;
    while (state.running && (newline = state.commands.find('\n')) != std::string::npos) {
      handleCommand(state, std::string_view(state.commands).substr(0, newline));
      state.commands.erase(0, newline + 1);
    }
    if (state.commands.size() > kMaxCommandBytes) {
      fail(state, "command exceeds size limit");
    }
  }

  void drainEvents(State& state) {
    while (state.running) {
      xcb_generic_event_t* event = xcb_poll_for_event(state.connection);
      if (event == nullptr) {
        break;
      }
      const uint8_t type = event->response_type & 0x7F;
      if (type == XCB_MAP_NOTIFY && !state.mapped) {
        state.mapped = true;
        paint(state);
        reportState(state, "mapped");
      } else if (type == XCB_EXPOSE && reinterpret_cast<xcb_expose_event_t*>(event)->count == 0) {
        paint(state);
      } else if (type == XCB_CONFIGURE_NOTIFY && state.expanded && !state.resized) {
        const auto* configure = reinterpret_cast<xcb_configure_notify_event_t*>(event);
        if (configure->width == 800 && configure->height == 500) {
          state.resized = true;
          paint(state);
          std::println("resized 800x500");
        }
      } else if (type == 0) {
        std::println(
            stderr, "xwayland-shape-client: asynchronous X11 error {}",
            reinterpret_cast<xcb_generic_error_t*>(event)->error_code
        );
        state.failed = true;
        state.running = false;
      }
      std::free(event);
    }
  }
} // namespace

int main(int argc, char** argv) {
  if (argc < 3
      || argc > 4
      || (std::string_view(argv[2]) != "managed" && std::string_view(argv[2]) != "unmanaged")
      || (argc == 4 && std::string_view(argv[3]) != "unshaped")) {
    std::println(stderr, "usage: xwayland-shape-client TITLE managed|unmanaged [unshaped]");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  State state;
  int screenNumber = 0;
  state.connection = xcb_connect(nullptr, &screenNumber);
  if (state.connection == nullptr || xcb_connection_has_error(state.connection) != 0) {
    std::println(stderr, "xwayland-shape-client: failed to connect to DISPLAY");
    if (state.connection != nullptr) {
      xcb_disconnect(state.connection);
    }
    return 1;
  }
  const auto* extension = xcb_get_extension_data(state.connection, &xcb_shape_id);
  auto screens = xcb_setup_roots_iterator(xcb_get_setup(state.connection));
  for (int index = 0; index < screenNumber && screens.rem != 0; ++index) {
    xcb_screen_next(&screens);
  }
  if (extension == nullptr
      || extension->present == 0
      || screens.rem == 0
      || !createWindow(state, screens.data, argv[1], std::string_view(argv[2]) == "unmanaged", argc != 4)) {
    std::println(stderr, "xwayland-shape-client: failed to initialize the shaped window");
    xcb_disconnect(state.connection);
    return 1;
  }
  while (state.running && xcb_connection_has_error(state.connection) == 0) {
    // Synchronous shape queries can buffer expose events without leaving the
    // X11 descriptor readable, so drain before every poll.
    drainEvents(state);
    xcb_flush(state.connection);
    if (!state.running) {
      break;
    }
    std::array<pollfd, 2> ready{{
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = xcb_get_file_descriptor(state.connection), .events = POLLIN, .revents = 0},
    }};
    if (poll(ready.data(), ready.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail(state, "poll failed");
      break;
    }
    if ((ready[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      fail(state, "X11 connection closed");
      break;
    }
    drainEvents(state);
    if ((ready[0].revents & (POLLIN | POLLHUP)) != 0) {
      readCommands(state);
    }
  }
  if (xcb_connection_has_error(state.connection) != 0) {
    state.failed = true;
  }
  xcb_destroy_window(state.connection, state.window);
  xcb_flush(state.connection);
  xcb_disconnect(state.connection);
  return state.failed ? 1 : 0;
}
