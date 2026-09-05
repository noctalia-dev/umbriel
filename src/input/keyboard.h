#pragma once

#include "config/config.h"

#include <string>
#include <string_view>
#include <unordered_set>
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
    // Lock the next XKB group, wrapping at the end. False when this physical
    // keyboard has fewer than two layouts.
    bool cycleLayout();
    // Adopt the source keyboard's effective layout when this keymap contains
    // the same named layout, without selecting this keyboard on the seat.
    void syncLayoutFrom(const Keyboard& source);
    // Select a named layout on this physical keyboard without making it the
    // seat's active input device. False when the keymap lacks that layout.
    bool setLayoutByName(std::string_view name);

  private:
    static void onModifiers(wl_listener* listener, void* data);
    static void onKey(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleModifiers();
    void handleKey(void* data);
    void handleDestroy();
    void setLayout(xkb_layout_index_t group);
    // Report a real effective-group transition. Initial setup, config reloads,
    // and compositor-driven synchronization update the tracked index directly.
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
    bool m_syncingLayout = false;
    xkb_layout_index_t m_lastNotifiedLayout = XKB_LAYOUT_INVALID;

    wl_listener m_modifiers{};
    wl_listener m_key{};
    wl_listener m_destroy{};

    wl_event_source* m_repeatTimer = nullptr;
    Keybind m_repeatBind{};
    uint32_t m_repeatKeycode = 0;
    int m_repeatIntervalMs = 0;
    bool m_repeatArmed = false;
    // A keybind consumes both halves of its key event. Without remembering the
    // press, the release reaches the focused client as a release-only key and
    // applications may still act on it (for example, Space toggling playback).
    std::unordered_set<uint32_t> m_consumedKeycodes;
  };

} // namespace umbriel
