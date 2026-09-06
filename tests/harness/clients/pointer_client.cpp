// Drives a virtual pointer against a running compositor, for the harness. The headless backend has no input devices
// (wlroots 0.20 dropped wlr_headless_add_input_device), so this is the only way to exercise pointer hit-testing without
// a physical mouse: bind zwp_virtual_pointer_manager_v1, create a pointer, and feed it absolute motion and button
// events. The compositor attaches it to its wlr_cursor like any other pointer, so these events run the same path a real
// mouse does. Usage: pointer-client <width> <height> <command>... move <x> <y> absolute motion within the given extent
// click <button> press and release (button is an evdev BTN_* code) press <button> release <button> notch <dir> one
// vertical wheel notch, -1 up / 1 down notch-horizontal <dir> the same notch on the horizontal wheel axis, -1 left /
// 1 right mod <name|none> hold one modifier (shift, control, alt, or logo) tap <key> press and release one evdev key
// pause <ms> keep the pointer connection and current input state. Commands run in order, each followed by a frame and
// a roundtrip so the compositor has processed one before the next is sent.
// axis <horizontal|vertical> <delta> sends smooth finger input; axis-stop <horizontal|vertical> ends it.

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <thread>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  struct State {
    wl_seat* seat = nullptr;
    zwlr_virtual_pointer_manager_v1* pointerManager = nullptr;
    zwp_virtual_keyboard_manager_v1* keyboardManager = nullptr;
  };

  void handleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
      state->pointerManager = static_cast<zwlr_virtual_pointer_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2)
      );
    } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
      state->keyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(
          wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1)
      );
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };

  struct VirtualKeyboard {
    zwp_virtual_keyboard_v1* protocol = nullptr;
    xkb_context* context = nullptr;
    xkb_keymap* keymap = nullptr;
  };

  bool initializeKeyboard(VirtualKeyboard& keyboard, const State& state, wl_display* display) {
    if (state.keyboardManager == nullptr || state.seat == nullptr) {
      std::println(stderr, "pointer-client: compositor does not offer virtual keyboard input");
      return false;
    }
    keyboard.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keyboard.keymap = keyboard.context != nullptr
        ? xkb_keymap_new_from_names(keyboard.context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS)
        : nullptr;
    if (keyboard.keymap == nullptr) {
      std::println(stderr, "pointer-client: failed to compile the default XKB keymap");
      return false;
    }

    char* text = xkb_keymap_get_as_string(keyboard.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    FILE* file = std::tmpfile();
    if (text == nullptr || file == nullptr) {
      std::println(stderr, "pointer-client: failed to create the virtual keyboard keymap");
      std::free(text);
      if (file != nullptr) {
        std::fclose(file);
      }
      return false;
    }
    const size_t size = std::strlen(text) + 1;
    const bool wroteKeymap = std::fwrite(text, 1, size, file) == size && std::fflush(file) == 0;
    std::free(text);
    if (!wroteKeymap) {
      std::println(stderr, "pointer-client: failed to write the virtual keyboard keymap");
      std::fclose(file);
      return false;
    }
    std::rewind(file);

    keyboard.protocol = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.keyboardManager, state.seat);
    zwp_virtual_keyboard_v1_keymap(
        keyboard.protocol, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(file), static_cast<uint32_t>(size)
    );
    const bool ready = wl_display_roundtrip(display) >= 0;
    std::fclose(file);
    if (!ready) {
      std::println(stderr, "pointer-client: connection lost while creating the virtual keyboard");
    }
    return ready;
  }

  uint32_t modifierMask(const VirtualKeyboard& keyboard, const std::string& name) {
    const char* xkbName = nullptr;
    if (name == "shift") {
      xkbName = XKB_MOD_NAME_SHIFT;
    } else if (name == "control" || name == "ctrl") {
      xkbName = XKB_MOD_NAME_CTRL;
    } else if (name == "alt") {
      xkbName = XKB_MOD_NAME_ALT;
    } else if (name == "logo" || name == "super") {
      xkbName = XKB_MOD_NAME_LOGO;
    } else if (name == "none") {
      return 0;
    } else {
      std::println(stderr, "pointer-client: unknown modifier '{}'", name);
      std::exit(EXIT_FAILURE);
    }

    const xkb_mod_index_t index = xkb_keymap_mod_get_index(keyboard.keymap, xkbName);
    if (index == XKB_MOD_INVALID || index >= 32) {
      std::println(stderr, "pointer-client: keymap has no usable '{}' modifier", name);
      std::exit(EXIT_FAILURE);
    }
    return uint32_t{1} << index;
  }

  void destroyKeyboard(VirtualKeyboard& keyboard) {
    if (keyboard.protocol != nullptr) {
      zwp_virtual_keyboard_v1_destroy(keyboard.protocol);
    }
    if (keyboard.keymap != nullptr) {
      xkb_keymap_unref(keyboard.keymap);
    }
    if (keyboard.context != nullptr) {
      xkb_context_unref(keyboard.context);
    }
  }

  // Release velocity needs pauses to advance the input clock as well.
  uint32_t nextTime() {
    static const auto start = std::chrono::steady_clock::now();
    static uint32_t time = 1000;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    time = std::max(time + 1, 1000 + static_cast<uint32_t>(elapsed.count()));
    return time;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::println(stderr, "usage: {} <width> <height> <command>...", argv[0]);
    return EXIT_FAILURE;
  }
  const auto extentWidth = static_cast<uint32_t>(std::atoi(argv[1]));
  const auto extentHeight = static_cast<uint32_t>(std::atoi(argv[2]));

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "pointer-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);

  if (state.pointerManager == nullptr) {
    std::println(stderr, "pointer-client: compositor does not offer zwlr_virtual_pointer_manager_v1");
    return EXIT_FAILURE;
  }

  zwlr_virtual_pointer_v1* pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(state.pointerManager, state.seat);
  if (pointer == nullptr) {
    std::println(stderr, "pointer-client: failed to create a virtual pointer");
    return EXIT_FAILURE;
  }

  const std::vector<std::string> args(argv + 3, argv + argc);
  const bool needsKeyboard =
      std::ranges::find(args, "mod") != args.end() || std::ranges::find(args, "tap") != args.end();
  VirtualKeyboard keyboard;
  if (needsKeyboard && !initializeKeyboard(keyboard, state, display)) {
    return EXIT_FAILURE;
  }
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& command = args[i];
    auto needs = [&](size_t count) {
      if (i + count >= args.size()) {
        std::println(stderr, "pointer-client: '{}' needs {} argument(s)", command, count);
        std::exit(EXIT_FAILURE);
      }
    };

    if (command == "move") {
      needs(2);
      const auto x = static_cast<uint32_t>(std::atoi(args[i + 1].c_str()));
      const auto y = static_cast<uint32_t>(std::atoi(args[i + 2].c_str()));
      i += 2;
      zwlr_virtual_pointer_v1_motion_absolute(pointer, nextTime(), x, y, extentWidth, extentHeight);
    } else if (command == "press" || command == "release" || command == "click") {
      needs(1);
      const auto button = static_cast<uint32_t>(std::atoi(args[i + 1].c_str()));
      i += 1;
      if (command != "release") {
        zwlr_virtual_pointer_v1_button(pointer, nextTime(), button, WL_POINTER_BUTTON_STATE_PRESSED);
      }
      if (command == "click") {
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_roundtrip(display);
      }
      if (command != "press") {
        zwlr_virtual_pointer_v1_button(pointer, nextTime(), button, WL_POINTER_BUTTON_STATE_RELEASED);
      }
    } else if (command == "notch" || command == "notch-horizontal") {
      needs(1);
      const int dir = std::atoi(args[i + 1].c_str()) < 0 ? -1 : 1;
      i += 1;
      const uint32_t axis = command == "notch" ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
      // A real wheel sends the smooth value and the discrete step together. The overview counts notches, so the
      // discrete half is the one that matters here, but sending only that is not something a wheel does.
      zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
      zwlr_virtual_pointer_v1_axis_discrete(pointer, nextTime(), axis, wl_fixed_from_double(dir * 15.0), dir);
    } else if (command == "axis" || command == "axis-stop") {
      needs(command == "axis" ? 2 : 1);
      const std::string& orientation = args[i + 1];
      if (orientation != "horizontal" && orientation != "vertical") {
        std::println(stderr, "pointer-client: unknown axis '{}'", orientation);
        return EXIT_FAILURE;
      }
      const uint32_t axis =
          orientation == "horizontal" ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
      zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_FINGER);
      if (command == "axis") {
        const double delta = std::atof(args[i + 2].c_str());
        i += 2;
        zwlr_virtual_pointer_v1_axis(pointer, nextTime(), axis, wl_fixed_from_double(delta));
      } else {
        i += 1;
        zwlr_virtual_pointer_v1_axis_stop(pointer, nextTime(), axis);
      }
    } else if (command == "mod") {
      needs(1);
      const uint32_t depressed = modifierMask(keyboard, args[i + 1]);
      i += 1;
      zwp_virtual_keyboard_v1_modifiers(keyboard.protocol, depressed, 0, 0, 0);
    } else if (command == "tap") {
      needs(1);
      const auto key = static_cast<uint32_t>(std::atoi(args[i + 1].c_str()));
      i += 1;
      zwp_virtual_keyboard_v1_key(keyboard.protocol, nextTime(), key, WL_KEYBOARD_KEY_STATE_PRESSED);
      zwlr_virtual_pointer_v1_frame(pointer);
      wl_display_roundtrip(display);
      zwp_virtual_keyboard_v1_key(keyboard.protocol, nextTime(), key, WL_KEYBOARD_KEY_STATE_RELEASED);
    } else if (command == "pause") {
      needs(1);
      const auto duration = std::chrono::milliseconds(std::atoi(args[i + 1].c_str()));
      i += 1;
      std::this_thread::sleep_for(duration);
    } else {
      std::println(stderr, "pointer-client: unknown command '{}'", command);
      return EXIT_FAILURE;
    }

    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_roundtrip(display) < 0) {
      std::println(stderr, "pointer-client: connection lost");
      return EXIT_FAILURE;
    }
  }

  zwlr_virtual_pointer_v1_destroy(pointer);
  destroyKeyboard(keyboard);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);
  return EXIT_SUCCESS;
}
