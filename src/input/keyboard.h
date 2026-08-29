#pragma once

#include "config/config.h"

#include <string>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct wlr_input_device;
struct wlr_input_method_keyboard_grab_v2;
struct wlr_keyboard;

namespace umbriel {

  class Server;

  class Keyboard {
  public:
    Keyboard(Server& server, wlr_input_device* device);
    ~Keyboard();

    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    [[nodiscard]] wlr_keyboard* wlr() const { return m_keyboard; }
    // Virtual keyboards own their keymap and groups; nothing compositor-side
    // may read or rotate them.
    [[nodiscard]] bool virtualDevice() const { return m_virtual; }
    void applyConfig();
    // Lock the next XKB group in the keymap, wrapping at the end. False when the
    // keyboard has nothing to switch to (single-layout keymap, virtual keyboard).
    bool cycleLayout();

  private:
    static void onModifiers(wl_listener* listener, void* data);
    static void onKey(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleModifiers();
    void handleKey(void* data);
    void handleDestroy();
    // Fire the IPC keyboard-layout event when the effective group changed since the last notification. A single
    // keyboard drives the event stream, so the tracked index is per-keyboard and the first notification always fires.
    void notifyLayoutIfChanged();
    // The input-method grab for this keyboard, or null when none applies. Always null while locked, so an IME cannot
    // keylog the lock screen.
    [[nodiscard]] wlr_input_method_keyboard_grab_v2* activeInputMethodGrab() const;
    void armRepeat(const Keybind& bind, uint32_t keycode);
    void cancelRepeat();
    static int onRepeatTimer(void* data);

    Server* m_server = nullptr;
    wlr_keyboard* m_keyboard = nullptr;
    bool m_virtual = false;
    std::string m_deviceName;
    xkb_layout_index_t m_lastNotifiedLayout = XKB_LAYOUT_INVALID;

    wl_listener m_modifiers{};
    wl_listener m_key{};
    wl_listener m_destroy{};

    wl_event_source* m_repeatTimer = nullptr;
    Keybind m_repeatBind{};
    uint32_t m_repeatKeycode = 0;
    int m_repeatIntervalMs = 0;
    bool m_repeatArmed = false;
  };

} // namespace umbriel
