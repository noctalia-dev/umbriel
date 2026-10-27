#pragma once

#include "scene/ssd_frame_geometry.h"

#include <cstdint>
#include <optional>

struct wlr_surface;

namespace umbriel {

  class LayerSurface;
  class Output;
  class Server;
  class View;
  class Workspace;

  struct SsdFrameHit {
    View* view = nullptr;
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  // Why focus is moving. The reason decides whether the layout scrolls to reveal the newly focused window, which is not
  // something the focus mechanism can infer: an interactive grab must not scroll (the tile is about to move and a
  // reveal would shift the grab offsets under the pointer), and a hover has a configurable limit on how far it may
  // scroll before it declines to focus at all.
  enum class FocusReason : uint8_t {
    Directional,  // window-focus-* keybinds, focus-adjacent after close
    PointerPress, // plain click-to-focus
    PointerHover, // follows_mouse enter
    Grab,         // Mod+drag / Mod+resize start
    DragDrop,     // tile/float drag finished
    Gesture,      // touchpad pan finished: the gesture already placed the strip
    Startup,      // map, setFloating, refocus fallback
    XdgActivation,
    ForeignActivation,
  };

  // Who holds keyboard focus, and everything that has to change when that moves. Focus is not one piece of state. It is
  // the seat's keyboard focus, the activation flag each client reads, the focus ring, the foreign-toplevel activation
  // other clients see, the workspace's own idea of its focused view, and the registry's recency order: all of which
  // have to agree. Every method here exists to keep them agreeing through one kind of transition. Layer-shell surfaces
  // and the overview can both take the seat away. When they hold it, focus still tracks internally so it can be
  // replayed on release; the difference is whether the keyboard enter is sent now or deferred.
  class FocusManager {
  public:
    explicit FocusManager(Server& server) : m_server(server) {}

    void focusView(View* view, FocusReason reason = FocusReason::Startup);
    void xwaylandHintsChanged(View* view);

    // A data-device drag suppresses seat keyboard enters while its grab is
    // active. Replay the view whose activation chrome already won once that
    // grab ends, without choosing a new focus target.
    void restoreActivatedViewKeyboardFocus();

    // Reconcile focus after infrastructure changes. A still-valid keyboard
    // focus owner stays authoritative; otherwise focus returns to a visible
    // scratchpad that held it last, or the pointer output supplies a
    // workspace fallback.
    void refocus();
    // Reconcile focus on `preferred`, keeping a visible scratchpad focused.
    // Workspace transitions use this overload; otherwise it delegates to
    // refocusExplicit.
    void refocus(Output* preferred);
    // Deliberately select a fallback on `preferred`. Explicit focus actions use
    // this overload, including with null when no output is available.
    void refocusExplicit(Output* preferred);

    // Drop activation, focus ring, and foreign-activated on every mapped view
    // except `except`.
    void deactivateViews(View* except = nullptr);
    // deactivateViews plus a keyboard clear: nothing focused, pointer untouched.
    void clearKeyboardFocus();
    // Keyboard *and* pointer clear, for a session lock taking the seat away
    // entirely.
    void clearNormalFocus();

    // The layer surface holding exclusive keyboard focus, if any. While one
    // exists it owns the seat and views only get chrome, not the keyboard.
    [[nodiscard]] LayerSurface* exclusiveKeyboardLayer() const;

    // Hit-test the scene. Returns the view under the point, or nullptr with `layer` set when a layer surface is there
    // instead. `surface` receives the wlr_surface under the point in either case, with `sx`/`sy` surface-local.
    [[nodiscard]] std::optional<SsdFrameHit> ssdFrameAt(double lx, double ly) const;
    View* viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer = nullptr);

  private:
    [[nodiscard]] bool retainCurrentKeyboardFocus();
    void refocusFallback(Output* preferred);

    Server& m_server;
    // The last raise decision focusView made. retainCurrentKeyboardFocus replays it, so a focus kept through a
    // popup grab or a seat handoff restacks exactly as the transition that selected the view did.
    bool m_lastRaise = true;
  };

  // The pointer output's active workspace: the target of explicit workspace and
  // layout actions, which intentionally follow the pointer.
  Workspace* activeWorkspace(Server& server);

  // The view holding the seat's keyboard focus, if any.
  View* seatFocusedWindow(Server& server);

  // Scratchpad focus follows the seat, not the pointer-selected output. A
  // cursor on another monitor must not expose that monitor's remembered
  // workspace focus to window-relative actions.
  View* focusedScratchpadWindow(Server& server);

  // Whether a detached scratchpad owns the seat: its window holds the keyboard,
  // or no window does while a scratchpad is active on the pointer output.
  bool scratchpadHoldsFocus(Server& server);

  // The window window-relative actions act on: the seat's focused view, else
  // none while a scratchpad holds the seat (the pointer output's remembered
  // focus must not leak), else the active workspace's remembered focus. A
  // layer surface or the overview borrowing the keyboard leaves the answer on
  // that remembered focus.
  View* focusedWindow(Server& server);

} // namespace umbriel
