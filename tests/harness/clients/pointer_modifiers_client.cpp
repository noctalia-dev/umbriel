// Exercises core wl_keyboard modifiers on a keyboard-interactivity-none panel.
#include "ext-session-lock-v1-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "pointer-modifiers-client: {}", message);
      std::exit(EXIT_FAILURE);
    }
  }

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    zwlr_layer_shell_v1* shell = nullptr;
    zwlr_virtual_pointer_manager_v1* pointerManager = nullptr;
    zwp_virtual_keyboard_manager_v1* keyboardManager = nullptr;
    ext_session_lock_manager_v1* lockManager = nullptr;
    bool lockedSession = false;
    zwp_input_method_manager_v2* inputMethodManager = nullptr;
    unsigned grabbedModifiers = 0;
    wl_surface* surface = nullptr;
    wl_buffer* buffer = nullptr;
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = nullptr;
    std::array<uint32_t, 4> modifiers{};
    std::array<uint32_t, 4> buttonModifiers{};
    unsigned modifierEvents = 0;
    unsigned enters = 0;
    unsigned keys = 0;
    unsigned buttons = 0;
    bool pointerInside = false;
    bool ready = false;
  };

  void keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto& s = *static_cast<State*>(data);
    require(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0, "invalid seat keymap");
    void* text = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    require(text != MAP_FAILED, "mmap keymap");
    if (s.keymap != nullptr) {
      xkb_keymap_unref(s.keymap);
    }
    s.keymap = xkb_keymap_new_from_buffer(
        s.context, static_cast<const char*>(text), size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    require(s.keymap != nullptr, "parse seat keymap");
    munmap(text, size);
    close(fd);
  }
  void enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { ++static_cast<State*>(data)->enters; }
  void leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
  void key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) { ++static_cast<State*>(data)->keys; }
  void
  modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    auto& s = *static_cast<State*>(data);
    require(s.keymap != nullptr, "modifiers before valid keymap");
    s.modifiers = {depressed, latched, locked, group};
    ++s.modifierEvents;
  }
  void repeat(void*, wl_keyboard*, int32_t, int32_t) {}
  constexpr wl_keyboard_listener kKeyboard = {keymap, enter, leave, key, modifiers, repeat};
  void pointerEnter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    static_cast<State*>(data)->pointerInside = true;
  }
  void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    static_cast<State*>(data)->pointerInside = false;
  }
  void motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
  void button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {
    auto& s = *static_cast<State*>(data);
    s.buttonModifiers = s.modifiers;
    ++s.buttons;
  }
  void axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  constexpr wl_pointer_listener kPointer = [] {
    wl_pointer_listener listener{};
    listener.enter = pointerEnter;
    listener.leave = pointerLeave;
    listener.motion = motion;
    listener.button = button;
    listener.axis = axis;
    return listener;
  }();
  void capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& s = *static_cast<State*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && s.keyboard == nullptr) {
      s.keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(s.keyboard, &kKeyboard, &s);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && s.pointer == nullptr) {
      s.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(s.pointer, &kPointer, &s);
    }
  }
  void seatName(void*, wl_seat*, const char*) {}
  constexpr wl_seat_listener kSeat = {capabilities, seatName};
  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& s = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      s.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      s.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      // v4 avoids pointer frame events, which this probe does not need.
      s.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 4));
      wl_seat_add_listener(s.seat, &kSeat, &s);
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
      s.shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4));
    } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
      s.pointerManager = static_cast<zwlr_virtual_pointer_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1)
      );
    } else if (std::strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
      s.lockManager = static_cast<ext_session_lock_manager_v1*>(
          wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1)
      );
    } else if (std::strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
      s.inputMethodManager = static_cast<zwp_input_method_manager_v2*>(
          wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1)
      );
    } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
      s.keyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(
          wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1)
      );
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistry = {global, globalRemove};
  void sessionLocked(void* data, ext_session_lock_v1*) { static_cast<State*>(data)->lockedSession = true; }
  void sessionFinished(void*, ext_session_lock_v1*) { require(false, "session lock refused"); }
  constexpr ext_session_lock_v1_listener kLock = {sessionLocked, sessionFinished};
  void imeActivate(void*, zwp_input_method_v2*) {}
  void imeSurrounding(void*, zwp_input_method_v2*, const char*, uint32_t, uint32_t) {}
  void imeCause(void*, zwp_input_method_v2*, uint32_t) {}
  void imeContent(void*, zwp_input_method_v2*, uint32_t, uint32_t) {}
  void imeUnavailable(void*, zwp_input_method_v2*) { require(false, "IME unavailable"); }
  constexpr zwp_input_method_v2_listener kIme = {imeActivate, imeActivate, imeSurrounding, imeCause,
                                                 imeContent,  imeActivate, imeUnavailable};
  void grabKeymap(void*, zwp_input_method_keyboard_grab_v2*, uint32_t, int32_t fd, uint32_t) { close(fd); }
  void grabKey(void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t, uint32_t) {}
  void grabModifiers(void* data, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    ++static_cast<State*>(data)->grabbedModifiers;
  }
  void grabRepeat(void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) {}
  constexpr zwp_input_method_keyboard_grab_v2_listener kGrab = {grabKeymap, grabKey, grabModifiers, grabRepeat};
  void sync(State& s) {
    require(wl_display_roundtrip(s.display) >= 0 && wl_display_roundtrip(s.display) >= 0, "connection lost");
  }
  void frame(void* data, wl_callback* callback, uint32_t) {
    static_cast<State*>(data)->ready = true;
    wl_callback_destroy(callback);
  }
  constexpr wl_callback_listener kFrame = {frame};
  void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t, uint32_t) {
    auto& s = *static_cast<State*>(data);
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    wl_surface_attach(s.surface, s.buffer, 0, 0);
    wl_surface_damage_buffer(s.surface, 0, 0, 300, 100);
    wl_callback_add_listener(wl_surface_frame(s.surface), &kFrame, &s);
    wl_surface_commit(s.surface);
  }
  void closed(void*, zwlr_layer_surface_v1*) { require(false, "layer closed"); }
  constexpr zwlr_layer_surface_v1_listener kLayer = {configure, closed};
} // namespace

int main() {
  State s;
  s.display = wl_display_connect(nullptr);
  require(s.display != nullptr && s.context != nullptr, "connect");
  wl_registry_add_listener(wl_display_get_registry(s.display), &kRegistry, &s);
  sync(s);
  require(s.seat && s.shm && s.compositor && s.shell && s.pointerManager && s.keyboardManager, "missing global");
  const xkb_rule_names names{
      .rules = nullptr, .model = nullptr, .layout = "us,de", .variant = nullptr, .options = nullptr
  };
  xkb_keymap* inputKeymap = xkb_keymap_new_from_names(s.context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  require(inputKeymap != nullptr, "input keymap");
  char* text = xkb_keymap_get_as_string(inputKeymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  FILE* file = std::tmpfile();
  require(file != nullptr && text != nullptr, "keymap file");
  const size_t size = std::strlen(text) + 1;
  require(std::fwrite(text, 1, size, file) == size && std::fflush(file) == 0, "write keymap");
  auto* keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(s.keyboardManager, s.seat);
  zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(file), size);
  sync(s);
  std::fclose(file);
  std::free(text);
  auto mask = [&](const char* name) {
    const auto index = xkb_keymap_mod_get_index(inputKeymap, name);
    require(index < 32, "modifier index");
    return uint32_t{1} << index;
  };
  const uint32_t logo = mask(XKB_MOD_NAME_LOGO);
  const uint32_t shift = mask(XKB_MOD_NAME_SHIFT);
  const uint32_t caps = mask(XKB_MOD_NAME_CAPS);
  auto* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(s.pointerManager, s.seat);
  uint32_t time = 0;
  auto move = [&](uint32_t x, uint32_t y) {
    zwlr_virtual_pointer_v1_motion_absolute(pointer, ++time, x, y, 1280, 720);
    zwlr_virtual_pointer_v1_frame(pointer);
    sync(s);
  };
  auto mods = [&](uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    zwp_virtual_keyboard_v1_modifiers(keyboard, depressed, latched, locked, group);
    sync(s);
  };
  auto click = [&](uint32_t state) {
    zwlr_virtual_pointer_v1_button(pointer, ++time, 272, state);
    zwlr_virtual_pointer_v1_frame(pointer);
    sync(s);
  };
  move(600, 400);
  const int fd = memfd_create("pointer-modifiers", MFD_CLOEXEC);
  require(fd >= 0 && ftruncate(fd, 300 * 100 * 4) == 0, "buffer file");
  wl_shm_pool* pool = wl_shm_create_pool(s.shm, fd, 300 * 100 * 4);
  s.buffer = wl_shm_pool_create_buffer(pool, 0, 300, 100, 300 * 4, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  s.surface = wl_compositor_create_surface(s.compositor);
  auto* layer = zwlr_layer_shell_v1_get_layer_surface(
      s.shell, s.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "umbriel-pointer-modifiers"
  );
  zwlr_layer_surface_v1_add_listener(layer, &kLayer, &s);
  zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_size(layer, 300, 100);
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(s.surface);
  while (!s.ready) {
    require(wl_display_dispatch(s.display) >= 0, "map panel");
  }
  mods(logo, shift, caps, 1);
  const auto outsideEvents = s.modifierEvents;
  move(100, 50);
  const std::array<uint32_t, 4> held{logo, shift, caps, 1};
  require(
      s.pointerInside && s.modifierEvents > outsideEvents && s.modifiers == held, "entry must forward current XKB state"
  );
  click(WL_POINTER_BUTTON_STATE_PRESSED);
  require(s.buttons == 1 && s.buttonModifiers == held, "modifiers must precede pointer press");
  move(600, 400);
  require(s.pointerInside, "implicit grab lost pointer focus");
  mods(0, 0, caps, 1);
  require(s.modifiers == std::array<uint32_t, 4>{0, 0, caps, 1}, "release during implicit drag");
  click(WL_POINTER_BUTTON_STATE_RELEASED);
  require(s.buttons == 2 && s.buttonModifiers == s.modifiers, "drag release modifiers");
  move(100, 50);
  mods(logo, shift, caps, 1);
  require(s.modifiers == held, "modifier change during hover");
  move(600, 400);
  require(
      !s.pointerInside && s.modifiers == std::array<uint32_t, 4>{0, 0, caps, 1}, "leave must clear transient state only"
  );
  const auto leaveEvents = s.modifierEvents;
  mods(shift, 0, 0, 0);
  require(s.modifierEvents == leaveEvents, "unfocused client received modifier changes");
  move(100, 50);
  require(s.modifiers == std::array<uint32_t, 4>{shift, 0, 0, 0}, "re-entry used stale state");
  zwp_virtual_keyboard_v1_key(keyboard, ++time, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
  zwp_virtual_keyboard_v1_key(keyboard, ++time, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
  sync(s);
  require(s.enters == 0 && s.keys == 0, "pointer forwarding granted keyboard focus or keys");
  // Binding after pointer entry need not synthesize keyboard focus: the next
  // button must still see the complete current state.
  mods(logo, shift, caps, 1);
  wl_keyboard_release(s.keyboard);
  s.keyboard = wl_seat_get_keyboard(s.seat);
  wl_keyboard_add_listener(s.keyboard, &kKeyboard, &s);
  s.modifiers = {};
  sync(s);
  click(WL_POINTER_BUTTON_STATE_PRESSED);
  require(s.buttonModifiers == held, "late-bound keyboard button snapshot");
  click(WL_POINTER_BUTTON_STATE_RELEASED);

  require(s.lockManager != nullptr, "session-lock global");
  // A separate IME client grabs this already-active keyboard without replacing
  // the seat keymap. Hover state must update even when no button is pressed.
  State ime;
  ime.display = wl_display_connect(nullptr);
  require(ime.display != nullptr, "IME connection");
  wl_registry_add_listener(wl_display_get_registry(ime.display), &kRegistry, &ime);
  sync(ime);
  require(ime.inputMethodManager && ime.seat, "IME globals");
  auto* inputMethod = zwp_input_method_manager_v2_get_input_method(ime.inputMethodManager, ime.seat);
  zwp_input_method_v2_add_listener(inputMethod, &kIme, nullptr);
  auto* grab = zwp_input_method_v2_grab_keyboard(inputMethod);
  zwp_input_method_keyboard_grab_v2_add_listener(grab, &kGrab, &ime);
  sync(ime);
  const auto grabbedBefore = ime.grabbedModifiers;
  mods(shift, 0, caps, 1);
  sync(ime);
  require(ime.grabbedModifiers > grabbedBefore, "IME did not receive grabbed modifiers");
  require(s.modifiers == std::array<uint32_t, 4>{shift, 0, caps, 1}, "IME-grab hover modifiers stale");
  mods(0, 0, caps, 1);
  require(s.modifiers == std::array<uint32_t, 4>{0, 0, caps, 1}, "IME-grab hover release stale");
  zwp_input_method_keyboard_grab_v2_release(grab);
  zwp_input_method_v2_destroy(inputMethod);
  sync(ime);
  wl_display_disconnect(ime.display);
  xkb_keymap_unref(ime.keymap);
  xkb_context_unref(ime.context);
  mods(logo, shift, caps, 1);
  auto* lock = ext_session_lock_manager_v1_lock(s.lockManager);
  ext_session_lock_v1_add_listener(lock, &kLock, &s);
  for (int i = 0; i < 100 && !s.lockedSession; ++i) {
    sync(s);
    usleep(10000);
  }
  require(s.lockedSession, "session lock confirmed");
  require(s.modifiers == std::array<uint32_t, 4>{}, "session lock must neutralize all pointer masks");
  const auto lockedEvents = s.modifierEvents;
  mods(shift, 0, caps, 0);
  require(s.modifierEvents == lockedEvents, "normal pointer client received modifiers while locked");
  ext_session_lock_v1_unlock_and_destroy(lock);
  sync(s);
  move(600, 400);
  move(100, 50);
  require(s.modifiers == std::array<uint32_t, 4>{shift, 0, caps, 0}, "pointer state after unlock");
  require(s.enters == 0 && s.keys == 0, "lifecycle forwarding granted keyboard focus or keys");
  // Same client now owns both foci: ordinary wlroots delivery must not double.
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
  wl_surface_commit(s.surface);
  sync(s);
  require(s.enters == 1, "exclusive panel did not get keyboard focus");
  const auto before = s.modifierEvents;
  mods(logo, 0, 0, 0);
  require(s.modifierEvents == before + 1, "duplicate modifiers for keyboard-focused pointer client");
  move(600, 400);
  require(s.modifiers[0] == logo, "pointer leave cleared keyboard-focused client");
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(s.surface);
  sync(s);
  move(100, 50);
  // Removing the active keyboard must immediately restore the remaining
  // keyboard's masks, without requiring another key or pointer event.
  auto* replacement = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(s.keyboardManager, s.seat);
  text = xkb_keymap_get_as_string(inputKeymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  file = std::tmpfile();
  require(file != nullptr && text != nullptr, "replacement keymap file");
  require(std::fwrite(text, 1, size, file) == size && std::fflush(file) == 0, "write replacement keymap");
  zwp_virtual_keyboard_v1_keymap(replacement, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fileno(file), size);
  zwp_virtual_keyboard_v1_modifiers(replacement, shift, 0, 0, 0);
  sync(s);
  std::fclose(file);
  std::free(text);
  mods(logo, shift, caps, 1);
  require(s.modifiers == held, "held modifiers before device removal");
  zwp_virtual_keyboard_v1_destroy(keyboard);
  sync(s);
  require(s.modifiers == std::array<uint32_t, 4>{shift, 0, 0, 0}, "replacement keyboard masks not restored");
  zwp_virtual_keyboard_v1_destroy(replacement);
  sync(s);
  require(s.modifiers == std::array<uint32_t, 4>{}, "keyboard removal must neutralize pointer masks");
  zwlr_layer_surface_v1_destroy(layer);
  wl_surface_destroy(s.surface);
  sync(s);
  require(!s.pointerInside, "destroying hovered surface did not clear pointer focus");
  std::println(
      "pointer modifiers: entry, ordering, hover, implicit drag, leave, locks/group, no keys/focus, no duplicates "
      "late binding, IME hover, session lock, keyboard removal passed"
  );
  wl_display_disconnect(s.display);
  xkb_keymap_unref(inputKeymap);
  xkb_keymap_unref(s.keymap);
  xkb_context_unref(s.context);
}
