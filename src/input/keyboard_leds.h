#pragma once

#include <functional>
#include <list>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_keyboard;
struct xkb_state;

namespace umbriel {

  // Share named locks across backend keyboards and publish consistent indicators,
  // including input from LED-less proxies. Virtual keyboards retain their own state.
  class KeyboardLeds {
  public:
    explicit KeyboardLeds(wl_event_loop* loop, std::function<bool(wlr_keyboard*)> canUseSource = {});
    ~KeyboardLeds();

    KeyboardLeds(const KeyboardLeds&) = delete;
    KeyboardLeds& operator=(const KeyboardLeds&) = delete;

    void add(wlr_keyboard* keyboard, bool virtualDevice = false, bool initialNumLock = false);
    void remove(wlr_keyboard* keyboard);
    // A source contributes changes since its previous observation. Ordinary input
    // cannot replace shared locks with a different device's stale snapshot.
    void update(wlr_keyboard* source = nullptr);
    [[nodiscard]] bool syncing() const { return m_syncing; }

  private:
    struct Entry {
      KeyboardLeds* owner = nullptr;
      wlr_keyboard* keyboard = nullptr;
      bool virtualDevice = false;
      bool initialNumLock = false;
      std::vector<std::string> locked;
      xkb_state* indicators = nullptr;
      wl_listener keymap{};
      wl_listener destroy{};
    };

    static void onKeymap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onIdle(void* data);

    [[nodiscard]] bool usableSource(wlr_keyboard* keyboard) const;
    [[nodiscard]] wlr_keyboard* fallbackSource() const;
    void observeLocks(Entry& entry);
    void initializeLocks(const Entry& entry);
    void syncPhysicalLocks();
    void flush();

    wl_event_loop* m_loop = nullptr;
    wl_event_source* m_idle = nullptr;
    std::function<bool(wlr_keyboard*)> m_canUseSource;
    std::list<Entry> m_entries;
    wlr_keyboard* m_source = nullptr;
    std::vector<std::string> m_locked;
    bool m_initialized = false;
    bool m_syncing = false;
  };

} // namespace umbriel
