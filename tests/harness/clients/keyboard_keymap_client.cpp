// Verifies that a virtual keyboard is not exposed as the seat keyboard until it has a usable keymap. Some virtual
// keyboard clients create their object before preparing its keymap. Advertising that incomplete device makes wlroots
// send wl_keyboard.keymap with no_keymap and size zero, which leaves strict clients without an XKB keymap.

#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <print>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  enum class Phase {
    BeforeUpload,
    FirstUpload,
    SecondUpload,
  };

  std::string_view phaseName(Phase phase) {
    switch (phase) {
    case Phase::BeforeUpload:
      return "before virtual keymap upload";
    case Phase::FirstUpload:
      return "after first virtual keymap upload";
    case Phase::SecondUpload:
      return "after second virtual keymap upload";
    }
    return "during unknown phase";
  }

  struct State {
    wl_display* display = nullptr;
    wl_seat* seat = nullptr;
    zwp_virtual_keyboard_manager_v1* keyboardManager = nullptr;
    wl_keyboard* keyboard = nullptr;
    xkb_context* xkbContext = nullptr;
    Phase phase = Phase::BeforeUpload;
    size_t validKeymaps = 0;
    size_t invalidKeymaps = 0;
  };

  void releaseKeyboard(State& state) {
    if (state.keyboard == nullptr) {
      return;
    }
    wl_keyboard_release(state.keyboard);
    state.keyboard = nullptr;
  }

  struct KeymapFile {
    FILE* file = nullptr;
    uint32_t size = 0;

    ~KeymapFile() {
      if (file != nullptr) {
        std::fclose(file);
      }
    }

    KeymapFile() = default;
    KeymapFile(const KeymapFile&) = delete;
    KeymapFile& operator=(const KeymapFile&) = delete;
  };

  void keyboardKeymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto& state = *static_cast<State*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
      ++state.invalidKeymaps;
      std::println(
          stderr, "keyboard-keymap-client: received wl_keyboard keymap format={} size={} {}", format, size,
          phaseName(state.phase)
      );
      close(fd);
      return;
    }

    void* mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
      ++state.invalidKeymaps;
      std::println(stderr, "keyboard-keymap-client: could not map wl_keyboard keymap {}", phaseName(state.phase));
      close(fd);
      return;
    }
    xkb_keymap* keymap = xkb_keymap_new_from_buffer(
        state.xkbContext, static_cast<const char*>(mapping), size, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    munmap(mapping, size);
    close(fd);
    if (keymap == nullptr) {
      ++state.invalidKeymaps;
      std::println(stderr, "keyboard-keymap-client: could not compile wl_keyboard keymap {}", phaseName(state.phase));
      return;
    }
    xkb_keymap_unref(keymap);
    ++state.validKeymaps;
  }

  void keyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
  void keyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
  void keyboardKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {}
  void keyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
  void keyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

  constexpr wl_keyboard_listener kKeyboardListener = {
      .keymap = keyboardKeymap,
      .enter = keyboardEnter,
      .leave = keyboardLeave,
      .key = keyboardKey,
      .modifiers = keyboardModifiers,
      .repeat_info = keyboardRepeatInfo,
  };

  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && state.keyboard == nullptr) {
      state.keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(state.keyboard, &kKeyboardListener, &state);
    } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
      releaseKeyboard(state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
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

  bool createKeymapFile(xkb_context* context, const char* layout, KeymapFile& output) {
    const xkb_rule_names names{
        .rules = nullptr,
        .model = nullptr,
        .layout = layout,
        .variant = nullptr,
        .options = nullptr,
    };
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
      std::println(stderr, "keyboard-keymap-client: failed to compile the '{}' XKB keymap", layout);
      return false;
    }
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    if (text == nullptr) {
      std::println(stderr, "keyboard-keymap-client: failed to serialize the '{}' XKB keymap", layout);
      return false;
    }

    const size_t size = std::strlen(text) + 1;
    if (size > std::numeric_limits<uint32_t>::max()) {
      std::println(stderr, "keyboard-keymap-client: serialized keymap is too large");
      std::free(text);
      return false;
    }
    output.file = std::tmpfile();
    if (output.file == nullptr) {
      std::println(stderr, "keyboard-keymap-client: failed to create keymap file");
      std::free(text);
      return false;
    }
    const bool wroteKeymap = std::fwrite(text, 1, size, output.file) == size && std::fflush(output.file) == 0;
    std::free(text);
    if (!wroteKeymap) {
      std::println(stderr, "keyboard-keymap-client: failed to write keymap file");
      return false;
    }
    std::rewind(output.file);
    output.size = static_cast<uint32_t>(size);
    return true;
  }

  bool roundtripTwice(State& state) {
    return wl_display_roundtrip(state.display) >= 0 && wl_display_roundtrip(state.display) >= 0;
  }

  bool sendKeymap(State& state, zwp_virtual_keyboard_v1* keyboard, KeymapFile& keymap, Phase phase) {
    state.phase = phase;
    const size_t validBefore = state.validKeymaps;
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(keymap.file), keymap.size);
    if (!roundtripTwice(state)) {
      std::println(stderr, "keyboard-keymap-client: connection lost while uploading a virtual keymap");
      return false;
    }
    if (state.invalidKeymaps != 0) {
      return false;
    }
    if (state.validKeymaps == validBefore) {
      std::println(stderr, "keyboard-keymap-client: virtual keymap upload was not broadcast to wl_keyboard clients");
      return false;
    }
    return true;
  }

} // namespace

int main() {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "keyboard-keymap-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }
  state.xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (state.xkbContext == nullptr) {
    std::println(stderr, "keyboard-keymap-client: failed to create XKB context");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0 || state.seat == nullptr || state.keyboardManager == nullptr) {
    std::println(stderr, "keyboard-keymap-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  KeymapFile firstKeymap;
  KeymapFile secondKeymap;
  if (!createKeymapFile(state.xkbContext, "us", firstKeymap)
      || !createKeymapFile(state.xkbContext, "de", secondKeymap)) {
    return EXIT_FAILURE;
  }

  zwp_virtual_keyboard_v1* pendingKeyboard =
      zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.keyboardManager, state.seat);
  state.phase = Phase::BeforeUpload;
  if (!roundtripTwice(state)) {
    std::println(stderr, "keyboard-keymap-client: connection lost while creating the virtual keyboard");
    return EXIT_FAILURE;
  }
  zwp_virtual_keyboard_v1_destroy(pendingKeyboard);
  if (wl_display_roundtrip(state.display) < 0) {
    std::println(stderr, "keyboard-keymap-client: connection lost while destroying the pending virtual keyboard");
    return EXIT_FAILURE;
  }

  zwp_virtual_keyboard_v1* virtualKeyboard =
      zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(state.keyboardManager, state.seat);
  if (!roundtripTwice(state)) {
    std::println(stderr, "keyboard-keymap-client: connection lost while recreating the virtual keyboard");
    return EXIT_FAILURE;
  }
  if (state.invalidKeymaps != 0) {
    return EXIT_FAILURE;
  }

  if (!sendKeymap(state, virtualKeyboard, firstKeymap, Phase::FirstUpload)
      || !sendKeymap(state, virtualKeyboard, secondKeymap, Phase::SecondUpload)) {
    return EXIT_FAILURE;
  }

  std::println("valid-keymaps {}", state.validKeymaps);
  zwp_virtual_keyboard_v1_destroy(virtualKeyboard);
  releaseKeyboard(state);
  zwp_virtual_keyboard_manager_v1_destroy(state.keyboardManager);
  wl_seat_destroy(state.seat);
  wl_registry_destroy(registry);
  xkb_context_unref(state.xkbContext);
  wl_display_disconnect(state.display);
  return EXIT_SUCCESS;
}
