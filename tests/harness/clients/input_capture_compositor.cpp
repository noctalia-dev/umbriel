// A real Server with backend-boundary keyboard and pointer devices. Commands
// exercise physical event handling without changing the user's input devices.
#include "config/config.h"
#include "input/cursor.h"
#include "input/input_capture.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "wlr.h"

extern "C" {
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
  constexpr wlr_keyboard_impl kKeyboardImpl{.name = "capture test backend", .led_update = nullptr};
  constexpr wlr_pointer_impl kPointerImpl{.name = "capture test backend"};

  class PhysicalInput {
  public:
    explicit PhysicalInput(umbriel::Server& server) : m_server(server) {
      wlr_keyboard_init(&m_keyboard, &kKeyboardImpl, "capture test physical keyboard");
      wl_signal_emit_mutable(&server.backend()->events.new_input, &m_keyboard.base);
      wlr_pointer_init(&m_pointer, &kPointerImpl, "capture test physical pointer");
      wl_signal_emit_mutable(&server.backend()->events.new_input, &m_pointer.base);
    }
    ~PhysicalInput() {
      wlr_pointer_finish(&m_pointer);
      wlr_keyboard_finish(&m_keyboard);
    }
    PhysicalInput(const PhysicalInput&) = delete;
    PhysicalInput& operator=(const PhysicalInput&) = delete;

    void command(const std::string& line) {
      std::istringstream words(line);
      std::string action;
      words >> action;
      if (action == "tap" || action == "press" || action == "release") {
        uint32_t code = 0;
        if (!(words >> code))
          throw std::runtime_error("key command requires a code");
        if (action != "release")
          key(code, WL_KEYBOARD_KEY_STATE_PRESSED);
        if (action != "press")
          key(code, WL_KEYBOARD_KEY_STATE_RELEASED);
      } else if (action == "move" || action == "at") {
        double x = 0;
        double y = 0;
        if (!(words >> x >> y))
          throw std::runtime_error("motion command requires x and y");
        if (action == "at") {
          x -= m_server.cursor()->wlr()->x;
          y -= m_server.cursor()->wlr()->y;
        }
        wlr_pointer_motion_event event{
            .pointer = &m_pointer,
            .time_msec = ++m_time,
            .delta_x = x,
            .delta_y = y,
            .unaccel_dx = x,
            .unaccel_dy = y,
        };
        wl_signal_emit_mutable(&m_pointer.events.motion, &event);
        frame();
      } else if (action == "absolute") {
        double x = 0;
        double y = 0;
        if (!(words >> x >> y))
          throw std::runtime_error("absolute motion requires normalized x and y");
        wlr_pointer_motion_absolute_event event{
            .pointer = &m_pointer,
            .time_msec = ++m_time,
            .x = x,
            .y = y,
        };
        wl_signal_emit_mutable(&m_pointer.events.motion_absolute, &event);
        frame();
      } else if (action == "button") {
        uint32_t code = 0;
        int pressed = 0;
        if (!(words >> code >> pressed))
          throw std::runtime_error("button command requires code and state");
        wlr_pointer_button_event event{
            .pointer = &m_pointer,
            .time_msec = ++m_time,
            .button = code,
            .state = pressed != 0 ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
        };
        wlr_pointer_notify_button(&m_pointer, &event);
        frame();
      } else if (action == "scroll") {
        double delta = 0;
        int32_t discrete = 0;
        if (!(words >> delta >> discrete))
          throw std::runtime_error("scroll requires delta and discrete value");
        wlr_pointer_axis_event event{
            .pointer = &m_pointer,
            .time_msec = ++m_time,
            .source = WL_POINTER_AXIS_SOURCE_WHEEL,
            .orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
            .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
            .delta = delta,
            .delta_discrete = discrete,
        };
        wl_signal_emit_mutable(&m_pointer.events.axis, &event);
        frame();
      } else if (action == "mark") {
        std::string marker;
        words >> marker;
        std::println("mark {}", marker);
      } else if (action == "status") {
        const auto* confirmation = m_server.quitConfirm();
        std::println(
            "status confirmation={:d} active={:d} x={:.3f} y={:.3f}",
            confirmation != nullptr && confirmation->visible(),
            m_server.inputCapture() != nullptr && m_server.inputCapture()->active(), m_server.cursor()->wlr()->x,
            m_server.cursor()->wlr()->y
        );
      } else if (action == "quit") {
        m_server.stop();
      } else {
        throw std::runtime_error("unknown physical input command: " + action);
      }
      wl_display_flush_clients(m_server.display());
      std::fflush(stdout);
    }

    static int readable(int fd, uint32_t, void* data) {
      auto& self = *static_cast<PhysicalInput*>(data);
      char buffer[4096];
      while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
          self.m_pending.append(buffer, static_cast<size_t>(count));
          size_t newline = 0;
          while ((newline = self.m_pending.find('\n')) != std::string::npos) {
            const std::string line = self.m_pending.substr(0, newline);
            self.m_pending.erase(0, newline + 1);
            try {
              self.command(line);
            } catch (const std::exception& error) {
              std::println(stderr, "physical input: {}", error.what());
              self.m_server.stop();
              self.m_failed = true;
            }
          }
        } else if (count < 0 && errno == EINTR) {
          continue;
        } else {
          if (count == 0)
            self.m_server.stop();
          return 0;
        }
      }
    }
    [[nodiscard]] bool failed() const { return m_failed; }

  private:
    void key(uint32_t code, wl_keyboard_key_state state) {
      wlr_keyboard_key_event event{
          .time_msec = ++m_time,
          .keycode = code,
          .update_state = true,
          .state = state,
      };
      wlr_keyboard_notify_key(&m_keyboard, &event);
    }
    void frame() { wl_signal_emit_mutable(&m_pointer.events.frame, &m_pointer); }

    umbriel::Server& m_server;
    wlr_keyboard m_keyboard{};
    wlr_pointer m_pointer{};
    std::string m_pending;
    uint32_t m_time = 0;
    bool m_failed = false;
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::println(stderr, "usage: input-capture-compositor CONFIG");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    umbriel::loadConfig(argv[1]);
    umbriel::Server server;
    if (!server.start())
      return 1;
    PhysicalInput input(server);
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0)
      return 1;
    wl_event_source* source = wl_event_loop_add_fd(
        wl_display_get_event_loop(server.display()), STDIN_FILENO, WL_EVENT_READABLE, PhysicalInput::readable, &input
    );
    if (source == nullptr)
      return 1;
    std::println("ready display={} socket={}", std::getenv("WAYLAND_DISPLAY"), std::getenv("UMBRIEL_SOCKET"));
    server.run();
    wl_event_source_remove(source);
    return input.failed() ? 1 : 0;
  } catch (const std::exception& error) {
    std::println(stderr, "input-capture-compositor: {}", error.what());
    return 1;
  }
}
