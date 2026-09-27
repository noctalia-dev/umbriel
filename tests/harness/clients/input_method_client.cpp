// Holds an input-method keyboard grab and an input-method-owned virtual keyboard. Optional lifecycle modes create the
// grab only while text input is active, with either a transient or persistent owned keyboard, matching both Fcitx
// modes. A separate harness virtual keyboard drives physical modifiers through the grab.

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  struct State {
    wl_display* display = nullptr;
    wl_seat* seat = nullptr;
    zwp_input_method_manager_v2* inputMethodManager = nullptr;
    zwp_virtual_keyboard_manager_v1* keyboardManager = nullptr;
    zwp_virtual_keyboard_v1* ownedKeyboard = nullptr;
    zwp_input_method_v2* inputMethod = nullptr;
    zwp_input_method_keyboard_grab_v2* grab = nullptr;
    bool activationLifecycle = false;
    bool persistentKeyboard = false;
    bool pendingActivate = false;
    bool pendingDeactivate = false;
  };

  void createKeyboardGrab(State& state);

  void destroyGrab(State& state) {
    if (state.grab != nullptr) {
      zwp_input_method_keyboard_grab_v2_release(state.grab);
      state.grab = nullptr;
    }
  }

  void destroyOwnedKeyboard(State& state) {
    if (state.ownedKeyboard != nullptr) {
      zwp_virtual_keyboard_v1_destroy(state.ownedKeyboard);
      state.ownedKeyboard = nullptr;
    }
  }

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 1U)));
    } else if (std::strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
      state.inputMethodManager = static_cast<zwp_input_method_manager_v2*>(
          wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, std::min(version, 1U))
      );
    } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
      state.keyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(
          wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, std::min(version, 1U))
      );
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void inputMethodActivate(void* data, zwp_input_method_v2*) {
    auto& state = *static_cast<State*>(data);
    state.pendingActivate = true;
  }
  void inputMethodDeactivate(void* data, zwp_input_method_v2*) {
    auto& state = *static_cast<State*>(data);
    state.pendingDeactivate = true;
  }
  void inputMethodSurroundingText(void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {}
  void inputMethodTextChangeCause(void*, zwp_input_method_v2*, uint32_t) {}
  void inputMethodContentType(void*, zwp_input_method_v2*, uint32_t, uint32_t) {}
  void inputMethodDone(void* data, zwp_input_method_v2*) {
    auto& state = *static_cast<State*>(data);
    if (!state.activationLifecycle) {
      return;
    }
    if (state.pendingDeactivate) {
      state.pendingDeactivate = false;
      destroyGrab(state);
      if (!state.persistentKeyboard) {
        destroyOwnedKeyboard(state);
      }
      std::println("deactivated");
      std::fflush(stdout);
    }
    if (state.pendingActivate) {
      state.pendingActivate = false;
      destroyGrab(state);
      if (!state.persistentKeyboard) {
        destroyOwnedKeyboard(state);
      }
      createKeyboardGrab(state);
      std::println("activated");
      std::fflush(stdout);
    }
  }
  void inputMethodUnavailable(void*, zwp_input_method_v2*) {
    std::println(stderr, "input-method-client: input method became unavailable");
    std::exit(EXIT_FAILURE);
  }

  constexpr zwp_input_method_v2_listener kInputMethodListener = {
      .activate = inputMethodActivate,
      .deactivate = inputMethodDeactivate,
      .surrounding_text = inputMethodSurroundingText,
      .text_change_cause = inputMethodTextChangeCause,
      .content_type = inputMethodContentType,
      .done = inputMethodDone,
      .unavailable = inputMethodUnavailable,
  };

  void grabKeymap(void* data, zwp_input_method_keyboard_grab_v2*, uint32_t format, int32_t fd, uint32_t size) {
    auto& state = *static_cast<State*>(data);
    if (state.ownedKeyboard != nullptr) {
      zwp_virtual_keyboard_v1_keymap(state.ownedKeyboard, format, fd, size);
    }
    wl_display_flush(state.display);
    close(fd);
  }
  void
  grabKey(void* data, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t time, uint32_t key, uint32_t keyState) {
    auto& state = *static_cast<State*>(data);
    if (state.ownedKeyboard != nullptr) {
      zwp_virtual_keyboard_v1_key(state.ownedKeyboard, time, key, keyState);
    }
  }
  void grabModifiers(
      void* data, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
      uint32_t group
  ) {
    auto& state = *static_cast<State*>(data);
    if (state.ownedKeyboard != nullptr) {
      zwp_virtual_keyboard_v1_modifiers(state.ownedKeyboard, depressed, latched, locked, group);
    }
    wl_display_flush(state.display);
  }
  void grabRepeatInfo(void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) {}

  constexpr zwp_input_method_keyboard_grab_v2_listener kGrabListener = {
      .keymap = grabKeymap,
      .key = grabKey,
      .modifiers = grabModifiers,
      .repeat_info = grabRepeatInfo,
  };

  void createKeyboardGrab(State& state) {
    if (state.ownedKeyboard == nullptr) {
      state.ownedKeyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.keyboardManager, state.seat);
    }
    state.grab = zwp_input_method_v2_grab_keyboard(state.inputMethod);
    zwp_input_method_keyboard_grab_v2_add_listener(state.grab, &kGrabListener, &state);
    std::println("grabbed");
    std::fflush(stdout);
  }

} // namespace

int main(int argc, char** argv) {
  const bool lifecycle = argc == 2
      && (std::strcmp(argv[1], "activation-lifecycle") == 0
          || std::strcmp(argv[1], "persistent-activation-lifecycle") == 0);
  if (argc > 2 || (argc == 2 && !lifecycle)) {
    std::println(stderr, "usage: {} [activation-lifecycle|persistent-activation-lifecycle]", argv[0]);
    return EXIT_FAILURE;
  }
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "input-method-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  State state;
  state.display = display;
  state.activationLifecycle = lifecycle;
  state.persistentKeyboard = argc == 2 && std::strcmp(argv[1], "persistent-activation-lifecycle") == 0;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);

  if (state.seat == nullptr || state.inputMethodManager == nullptr || state.keyboardManager == nullptr) {
    std::println(stderr, "input-method-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }
  state.inputMethod = zwp_input_method_manager_v2_get_input_method(state.inputMethodManager, state.seat);
  zwp_input_method_v2_add_listener(state.inputMethod, &kInputMethodListener, &state);
  if (state.persistentKeyboard) {
    state.ownedKeyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.keyboardManager, state.seat);
  }
  if (!state.activationLifecycle) {
    createKeyboardGrab(state);
  }
  if (wl_display_roundtrip(display) < 0) {
    std::println(stderr, "input-method-client: connection lost while creating the keyboard grab");
    return EXIT_FAILURE;
  }
  if (wl_display_roundtrip(display) < 0) {
    std::println(stderr, "input-method-client: connection lost while synchronizing the owned keyboard");
    return EXIT_FAILURE;
  }

  while (wl_display_dispatch(display) >= 0) {
  }

  destroyGrab(state);
  destroyOwnedKeyboard(state);
  zwp_input_method_v2_destroy(state.inputMethod);
  zwp_input_method_manager_v2_destroy(state.inputMethodManager);
  wl_display_disconnect(display);
  return EXIT_SUCCESS;
}
