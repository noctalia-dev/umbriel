#pragma once

#include "input/swipe_tracker.h"

#include <cstdint>
#include <wayland-server-core.h>

namespace umbriel {

  class Output;
  class Server;
  class Workspace;
  class WorkspaceGroup;

  class Gestures {
  public:
    explicit Gestures(Server& server);
    ~Gestures();

    Gestures(const Gestures&) = delete;
    Gestures& operator=(const Gestures&) = delete;

    void cancelForOutput(Output* output);

  private:
    enum class State { Idle, Forward, Pending, Scroll, Switch, Overview, OverviewSelect };

    static void onSwipeBegin(wl_listener* listener, void* data);
    static void onSwipeUpdate(wl_listener* listener, void* data);
    static void onSwipeEnd(wl_listener* listener, void* data);
    static void onPinchBegin(wl_listener* listener, void* data);
    static void onPinchUpdate(wl_listener* listener, void* data);
    static void onPinchEnd(wl_listener* listener, void* data);
    static void onHoldBegin(wl_listener* listener, void* data);
    static void onHoldEnd(wl_listener* listener, void* data);

    void handleSwipeBegin(void* data);
    void handleSwipeUpdate(void* data);
    void handleSwipeEnd(void* data);
    void handlePinchBegin(void* data);
    void handlePinchUpdate(void* data);
    void handlePinchEnd(void* data);
    void handleHoldBegin(void* data);
    void handleHoldEnd(void* data);

    void cancelActive();
    void finishScroll(bool cancelled, uint32_t timeMsec);
    void finishSwitch(bool cancelled);
    void finishOverview(bool cancelled);
    void silentCancel();

    // Finger travel that moves the strip by one viewport width.
    [[nodiscard]] double scrollNormFactor() const;

    Server* m_server = nullptr;
    State m_state = State::Idle;
    double m_accumX = 0;
    double m_accumY = 0;
    Output* m_output = nullptr;
    int m_naturalScrollDirection = 1;

    // Scroll state (horizontal 3-finger).
    Workspace* m_scrollWorkspace = nullptr;
    double m_scrollStart = 0;
    bool m_scrollStartCentered = false;
    int m_viewportPrimary = 0;
    SwipeTracker m_scrollTracker;

    // Switch state (vertical 3-finger).
    WorkspaceGroup* m_switchGroup = nullptr;
    double m_progress = 0;
    double m_velocity = 0;
    uint32_t m_lastTimeMsec = 0;
    bool m_hasPrev = false;
    bool m_hasNext = false;

    // Overview state (vertical 4-finger): swipe up opens, swipe down closes.
    bool m_overviewWasOpen = false;

    // OverviewSelect state (vertical 3-finger, overview up) reuses m_accumY as
    // the travel left over since the last row step.

    wl_listener m_swipeBegin{};
    wl_listener m_swipeUpdate{};
    wl_listener m_swipeEnd{};
    wl_listener m_pinchBegin{};
    wl_listener m_pinchUpdate{};
    wl_listener m_pinchEnd{};
    wl_listener m_holdBegin{};
    wl_listener m_holdEnd{};
  };

} // namespace umbriel
