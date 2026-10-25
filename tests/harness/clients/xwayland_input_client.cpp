// Sends the XTEST requests used by Steam Input without mapping an X11 window. Commands on stdin keep one X11 client
// alive across input transitions; sync LABEL acknowledges that Xwayland has processed the preceding requests.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <print>
#include <sstream>
#include <string>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

namespace {
  struct State {
    xcb_connection_t* connection = nullptr;
    xcb_window_t root = XCB_NONE;
  };

  bool sendInput(const State& state, uint8_t type, uint8_t detail, int16_t x = 0, int16_t y = 0) {
    const auto cookie =
        xcb_test_fake_input_checked(state.connection, type, detail, XCB_CURRENT_TIME, state.root, x, y, 0);
    xcb_generic_error_t* error = xcb_request_check(state.connection, cookie);
    if (error != nullptr) {
      std::println(stderr, "xwayland-input-client: XTEST error {}", error->error_code);
      std::free(error);
      return false;
    }
    return xcb_connection_has_error(state.connection) == 0;
  }

  bool sync(const State& state, const std::string& label) {
    const auto cookie = xcb_get_input_focus(state.connection);
    xcb_get_input_focus_reply_t* reply = xcb_get_input_focus_reply(state.connection, cookie, nullptr);
    if (reply == nullptr) {
      return false;
    }
    std::free(reply);
    std::println("sync {}", label);
    return true;
  }

  bool handleCommand(const State& state, const std::string& line) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;
    if (command.empty()) {
      return true;
    }
    if (command == "sync") {
      std::string label;
      if (stream >> label) {
        return sync(state, label);
      }
    } else if (command == "move" || command == "relative") {
      int x = 0;
      int y = 0;
      if (stream >> x >> y
          && x >= std::numeric_limits<int16_t>::min()
          && x <= std::numeric_limits<int16_t>::max()
          && y >= std::numeric_limits<int16_t>::min()
          && y <= std::numeric_limits<int16_t>::max()) {
        return sendInput(
            state, XCB_MOTION_NOTIFY, command == "relative", static_cast<int16_t>(x), static_cast<int16_t>(y)
        );
      }
    } else if (command == "press" || command == "release" || command == "tap") {
      unsigned int key = 0;
      if (stream >> key && key <= 247) {
        const uint8_t code = static_cast<uint8_t>(key + 8);
        if (command == "release") {
          return sendInput(state, XCB_KEY_RELEASE, code);
        }
        return sendInput(state, XCB_KEY_PRESS, code) && (command != "tap" || sendInput(state, XCB_KEY_RELEASE, code));
      }
    } else if (command == "button" || command == "click") {
      unsigned int button = 0;
      unsigned int pressed = 1;
      if (stream >> button
          && button > 0
          && button <= std::numeric_limits<uint8_t>::max()
          && (command == "click" || (stream >> pressed && pressed <= 1))) {
        const uint8_t code = static_cast<uint8_t>(button);
        return sendInput(state, pressed != 0 ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, code)
            && (command != "click" || sendInput(state, XCB_BUTTON_RELEASE, code));
      }
    }
    std::println(stderr, "xwayland-input-client: invalid command '{}'", line);
    return false;
  }
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  int screenNumber = 0;
  State state;
  state.connection = xcb_connect(nullptr, &screenNumber);
  if (state.connection == nullptr || xcb_connection_has_error(state.connection) != 0) {
    std::println(stderr, "xwayland-input-client: failed to connect to DISPLAY");
    if (state.connection != nullptr) {
      xcb_disconnect(state.connection);
    }
    return 1;
  }
  auto screen = xcb_setup_roots_iterator(xcb_get_setup(state.connection));
  for (int index = 0; index < screenNumber && screen.rem != 0; ++index) {
    xcb_screen_next(&screen);
  }
  if (screen.rem == 0) {
    xcb_disconnect(state.connection);
    return 1;
  }
  state.root = screen.data->root;
  const auto cookie = xcb_test_get_version(state.connection, 2, 2);
  xcb_test_get_version_reply_t* reply = xcb_test_get_version_reply(state.connection, cookie, nullptr);
  if (reply == nullptr) {
    std::println(stderr, "xwayland-input-client: XTEST is unavailable");
    xcb_disconnect(state.connection);
    return 1;
  }
  std::free(reply);
  std::println("ready");

  bool success = true;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "quit") {
      break;
    }
    if (!handleCommand(state, line)) {
      success = false;
      break;
    }
  }
  xcb_disconnect(state.connection);
  return success ? 0 : 1;
}
