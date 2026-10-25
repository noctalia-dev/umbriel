#include "xwayland/xwayland.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"
#include "xwayland/window.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace umbriel {

  namespace {
    constexpr Logger kLog("xwayland");

    // Interns `names` over a short-lived connection, so no reply is awaited on the window manager's connection.
    // Unresolved names stay XCB_ATOM_NONE.
    template <size_t N>
    std::array<xcb_atom_t, N> resolveAtoms(const char* displayName, const std::array<const char*, N>& names) {
      std::array<xcb_atom_t, N> atoms{};
      atoms.fill(XCB_ATOM_NONE);
      if (displayName == nullptr) {
        return atoms;
      }
      xcb_connection_t* connection = xcb_connect(displayName, nullptr);
      if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        if (connection != nullptr) {
          xcb_disconnect(connection);
        }
        return atoms;
      }
      std::array<xcb_intern_atom_cookie_t, N> cookies{};
      for (size_t i = 0; i < N; ++i) {
        cookies[i] = xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(names[i])), names[i]);
      }
      for (size_t i = 0; i < N; ++i) {
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(connection, cookies[i], nullptr);
        if (reply != nullptr) {
          atoms[i] = reply->atom;
        }
        std::free(reply);
      }
      xcb_disconnect(connection);
      return atoms;
    }
  } // namespace

  void grantConfigureRequest(wlr_xwayland_surface* xsurface, const wlr_xwayland_surface_configure_event* event) {
    const uint16_t mask = event->mask;
    wlr_xwayland_surface_configure(
        xsurface, (mask & XCB_CONFIG_WINDOW_X) != 0 ? event->x : xsurface->x,
        (mask & XCB_CONFIG_WINDOW_Y) != 0 ? event->y : xsurface->y,
        (mask & XCB_CONFIG_WINDOW_WIDTH) != 0 ? event->width : xsurface->width,
        (mask & XCB_CONFIG_WINDOW_HEIGHT) != 0 ? event->height : xsurface->height
    );
  }

  Xwayland::Xwayland(Server& server)
      : m_server(server), m_nativeResolution(config().general.xwaylandNativeResolution),
        m_xsettingsEnabled(config().general.xwaylandXsettings) {
    m_wlr = wlr_xwayland_create(server.display(), server.compositor(), /*lazy=*/true);
    if (m_wlr == nullptr) {
      kLog.error("failed to create the Xwayland server");
      return;
    }
    m_outputs = std::make_unique<XwaylandOutputs>(server.display(), server.outputLayout(), m_nativeResolution);
    // wlroots drops the seat whenever the window manager goes away, which happens each time the lazy Xwayland exits
    // with its last client, so hand it over again before every start. wlroots passes the seat to the window manager
    // when Xwayland is ready; setting it from the ready handler instead makes X requests there that can leave the
    // first client's MapRequest unread in xcb's queue.
    m_serverStart.notify = onServerStart;
    wl_signal_add(&m_wlr->server->events.start, &m_serverStart);
    applyCursor(server.cursor()->xcursorManager());
    m_ready.notify = onReady;
    wl_signal_add(&m_wlr->events.ready, &m_ready);
    m_newSurface.notify = onNewSurface;
    wl_signal_add(&m_wlr->events.new_surface, &m_newSurface);
    m_focusClearTimer = wl_event_loop_add_timer(wl_display_get_event_loop(server.display()), onFocusClearTimer, this);
    m_selectionRefreshTimer =
        wl_event_loop_add_timer(wl_display_get_event_loop(server.display()), onSelectionRefreshTimer, this);
    m_keyboardFocusChange.notify = onKeyboardFocusChange;
    wl_signal_add(&server.seat()->wlr()->keyboard_state.events.focus_change, &m_keyboardFocusChange);
    kLog.info("Xwayland listening on DISPLAY={}", m_wlr->display_name);
  }

  Xwayland::~Xwayland() {
    if (m_ownsEiSocketEnvironment) {
      if (m_inheritedEiSocket) {
        setenv("LIBEI_SOCKET", m_inheritedEiSocket->c_str(), 1);
      } else {
        unsetenv("LIBEI_SOCKET");
      }
    }
    if (m_wlr == nullptr) {
      return;
    }
    wl_list_remove(&m_keyboardFocusChange.link);
    if (m_selectionRefreshTimer != nullptr) {
      wl_event_source_remove(m_selectionRefreshTimer);
    }
    // Windows only detach here; the server deletes views before destroying this.
    m_windows.clear();
    wl_list_remove(&m_serverStart.link);
    wl_list_remove(&m_ready.link);
    wl_list_remove(&m_newSurface.link);
    if (m_focusClearTimer != nullptr) {
      wl_event_source_remove(m_focusClearTimer);
    }
    wlr_xwayland_destroy(m_wlr);
  }

  const char* Xwayland::displayName() const { return m_wlr != nullptr ? m_wlr->display_name : nullptr; }

  bool Xwayland::setInputSocket(int fd) {
    if (m_ownsEiSocketEnvironment) {
      return true;
    }
    if (const char* inherited = std::getenv("LIBEI_SOCKET")) {
      m_inheritedEiSocket = inherited;
    }
    // Xwayland inherits this endpoint. The unlinked socket stays pinned by a CLOEXEC descriptor in the compositor;
    // unrelated applications do not inherit that descriptor or receive authority to connect.
    const auto path = fd >= 0 ? std::format("/proc/{}/fd/{}", getpid(), fd) : std::string("/proc/self/fd/-1");
    m_ownsEiSocketEnvironment = setenv("LIBEI_SOCKET", path.c_str(), 1) == 0;
    return m_ownsEiSocketEnvironment;
  }

  bool Xwayland::authenticateInputPeer(pid_t peer) const {
    if (peer <= 0 || peer == getpid() || m_inputSocketInode == 0) {
      return false;
    }
    const auto path = std::format("/proc/{}/fd", peer);
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
      return false;
    }
    bool authenticated = false;
    while (const dirent* entry = readdir(directory)) {
      if (entry->d_name[0] == '.') {
        continue;
      }
      struct stat info{};
      if (fstatat(dirfd(directory), entry->d_name, &info, 0) == 0
          && S_ISSOCK(info.st_mode)
          && info.st_dev == m_inputSocketDevice
          && info.st_ino == m_inputSocketInode) {
        authenticated = true;
        break;
      }
    }
    closedir(directory);
    return authenticated;
  }

  void Xwayland::applyCursor(wlr_xcursor_manager* manager) {
    if (m_wlr == nullptr || manager == nullptr || !wlr_xcursor_manager_load(manager, 1.0F)) {
      return;
    }
    wlr_xcursor* xcursor = wlr_xcursor_manager_get_xcursor(manager, "default", 1.0F);
    if (xcursor == nullptr || xcursor->image_count == 0) {
      return;
    }
    wlr_xcursor_image* image = xcursor->images[0];
    wlr_xwayland_set_cursor(
        m_wlr, wlr_xcursor_image_get_buffer(image), static_cast<int32_t>(image->hotspot_x),
        static_cast<int32_t>(image->hotspot_y)
    );
    publishXsettings();
  }

  void Xwayland::forget(XwaylandWindow* window) {
    std::erase_if(m_windows, [window](const std::unique_ptr<XwaylandWindow>& entry) { return entry.get() == window; });
  }

  double Xwayland::surfaceScale(wlr_surface* surface) const {
    if (!m_nativeResolution || surface == nullptr) {
      return 1.0;
    }
    const wlr_xwayland_surface* xsurface = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xsurface == nullptr) {
      return 1.0;
    }
    const auto found = std::ranges::find(m_windows, xsurface, &XwaylandWindow::xsurface);
    return found != m_windows.end() ? (*found)->scale() : 1.0;
  }

  bool Xwayland::ownsSurface(const wlr_surface* surface) const {
    return surface != nullptr
        && surface->resource != nullptr
        && m_wlr != nullptr
        && m_wlr->server != nullptr
        && m_wlr->server->client != nullptr
        && wl_resource_get_client(surface->resource) == m_wlr->server->client;
  }

  bool Xwayland::advertiseOutputManager(const wl_client* client, const wl_global* global) const {
    return m_outputs->advertise(client, global, m_wlr->server != nullptr ? m_wlr->server->client : nullptr);
  }

  void Xwayland::handleOutputLayoutChange() {
    if (!m_outputs->update()) {
      return;
    }
    for (const auto& window : m_windows) {
      window->syncGeometry();
    }
    publishXsettings();
  }

  void Xwayland::scheduleFocusClear() {
    if (m_focusClearTimer != nullptr) {
      // A zero Wayland timer is disarmed. One millisecond puts this on the next event-loop turn, after the current
      // dispatch returns and wl_display_run flushes the native client's keyboard enter.
      wl_event_source_timer_update(m_focusClearTimer, 1);
    }
  }

  void Xwayland::onServerStart(wl_listener* listener, void* /*data*/) {
    Xwayland* self = wl_container_of(listener, self, m_serverStart); // NOLINT(modernize-use-auto)
    // wlroots double-forks Xwayland, so its server PID is not the final peer PID. Authenticate the EIS connection
    // against possession of Xwayland's private Wayland socket, which only that launched process inherits.
    struct stat info{};
    self->m_inputSocketDevice = self->m_inputSocketInode = 0;
    if (fstat(self->m_wlr->server->wl_fd[1], &info) == 0) {
      self->m_inputSocketDevice = info.st_dev;
      self->m_inputSocketInode = info.st_ino;
    }
    wlr_xwayland_set_seat(self->m_wlr, self->m_server.seat()->wlr());
  }

  void Xwayland::onReady(wl_listener* listener, void* /*data*/) {
    Xwayland* self = wl_container_of(listener, self, m_ready); // NOLINT(modernize-use-auto)
    self->handleReady();
  }

  void Xwayland::onNewSurface(wl_listener* listener, void* data) {
    Xwayland* self = wl_container_of(listener, self, m_newSurface); // NOLINT(modernize-use-auto)
    self->handleNewSurface(static_cast<wlr_xwayland_surface*>(data));
  }

  void Xwayland::onKeyboardFocusChange(wl_listener* listener, void* data) {
    Xwayland* self = wl_container_of(listener, self, m_keyboardFocusChange); // NOLINT(modernize-use-auto)
    self->handleKeyboardFocusChange(data);
  }

  int Xwayland::onSelectionRefreshTimer(void* data) {
    auto* self = static_cast<Xwayland*>(data);
    wlr_seat* seat = self->m_server.seat()->wlr();
    if (seat->keyboard_state.focused_surface == nullptr
        || wlr_xwayland_surface_try_from_wlr_surface(seat->keyboard_state.focused_surface) == nullptr
        || self->m_wlr->xwm == nullptr) {
      return 0;
    }
    // Wine reads targets on notification, including while unfocused when wlroots denies the read. Rebinding the
    // same seat republishes native selections after activation without replacing X11-owned sources.
    wlr_xwayland_set_seat(self->m_wlr, seat);
    return 0;
  }

  int Xwayland::onFocusClearTimer(void* data) {
    auto* self = static_cast<Xwayland*>(data);
    wlr_surface* focused = self->m_server.seat()->wlr()->keyboard_state.focused_surface;
    if (focused != nullptr && wlr_xwayland_surface_try_from_wlr_surface(focused) == nullptr) {
      self->clearFocus();
    }
    return 0;
  }

  void Xwayland::handleReady() {
    const auto atoms =
        resolveAtoms<4>(m_wlr->display_name, {"_NET_ACTIVE_WINDOW", "_XSETTINGS_S0", "_XSETTINGS_SETTINGS", "MANAGER"});
    m_netActiveWindow = atoms[0];
    if (m_netActiveWindow == XCB_ATOM_NONE) {
      kLog.warn("could not resolve _NET_ACTIVE_WINDOW");
    }
    if (m_xsettingsEnabled) {
      if (std::ranges::contains(atoms, static_cast<xcb_atom_t>(XCB_ATOM_NONE))) {
        kLog.warn("could not resolve the XSETTINGS atoms");
      } else {
        m_xsettingsSettingsAtom = atoms[2];
        startXsettings(atoms[1], atoms[3]);
      }
    }
    kLog.info("Xwayland ready on DISPLAY={}", m_wlr->display_name);
  }

  void Xwayland::handleNewSurface(wlr_xwayland_surface* xsurface) {
    m_windows.push_back(std::make_unique<XwaylandWindow>(m_server, *this, xsurface));
  }

  void Xwayland::handleKeyboardFocusChange(void* data) {
    auto* event = static_cast<wlr_seat_keyboard_focus_change_event*>(data);
    if (event->new_surface == nullptr
        || wlr_xwayland_surface_try_from_wlr_surface(event->new_surface) == nullptr
        || (event->old_surface != nullptr && wlr_xwayland_surface_try_from_wlr_surface(event->old_surface) != nullptr)
        || m_selectionRefreshTimer == nullptr) {
      return;
    }
    // Unmanaged X11 windows can activate after entering the seat. Wait until this dispatch has completed.
    wl_event_source_timer_update(m_selectionRefreshTimer, 1);
  }

  void Xwayland::clearFocus() {
    xcb_connection_t* connection = wlr_xwayland_get_xwm_connection(m_wlr);
    if (connection == nullptr) {
      return;
    }
    constexpr xcb_window_t kNoWindow = XCB_WINDOW_NONE;
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_NONE, kNoWindow, XCB_CURRENT_TIME);
    if (m_netActiveWindow != XCB_ATOM_NONE) {
      xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
      if (screens.rem != 0) {
        xcb_change_property(
            connection, XCB_PROP_MODE_REPLACE, screens.data->root, m_netActiveWindow, XCB_ATOM_WINDOW, 32, 1, &kNoWindow
        );
      }
    }
    xcb_flush(connection);
  }

  void Xwayland::startXsettings(uint32_t selectionAtom, uint32_t managerAtom) {
    xcb_connection_t* connection = wlr_xwayland_get_xwm_connection(m_wlr);
    if (connection == nullptr) {
      return;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    if (screens.rem == 0) {
      return;
    }
    const xcb_window_t root = screens.data->root;
    m_xsettingsConnection = connection;
    m_xsettingsWindow = xcb_generate_id(connection);
    m_xsettingsSerial = 0;
    xcb_create_window(
        connection, 0, m_xsettingsWindow, root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0,
        nullptr
    );
    // The property exists before the selection is announced, so a client reacting to MANAGER finds it.
    m_xsettings = {};
    publishXsettings();
    xcb_set_selection_owner(connection, m_xsettingsWindow, selectionAtom, XCB_CURRENT_TIME);
    xcb_client_message_event_t manager{};
    manager.response_type = XCB_CLIENT_MESSAGE;
    manager.format = 32;
    manager.window = root;
    manager.type = managerAtom;
    manager.data.data32[0] = XCB_CURRENT_TIME;
    manager.data.data32[1] = selectionAtom;
    manager.data.data32[2] = m_xsettingsWindow;
    xcb_send_event(connection, 0, root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<const char*>(&manager));
    xcb_flush(connection);
  }

  void Xwayland::publishXsettings() {
    xcb_connection_t* connection = m_wlr != nullptr ? wlr_xwayland_get_xwm_connection(m_wlr) : nullptr;
    if (connection == nullptr || connection != m_xsettingsConnection) {
      return;
    }
    const Config::Input::Cursor& cursor = config().input.cursor;
    XsettingsValues values = xsettingsFor(m_outputs->scale(), cursor.theme, cursor.size);
    if (m_xsettingsSerial != 0 && values == m_xsettings) {
      return;
    }
    m_xsettings = std::move(values);
    ++m_xsettingsSerial;
    const std::vector<uint8_t> data = encodeXsettings(m_xsettingsSerial, m_xsettings);
    xcb_change_property(
        connection, XCB_PROP_MODE_REPLACE, m_xsettingsWindow, m_xsettingsSettingsAtom, m_xsettingsSettingsAtom, 8,
        static_cast<uint32_t>(data.size()), data.data()
    );
    xcb_flush(connection);
    kLog.debug(
        "XSETTINGS {}: scale {} unscaled DPI {} Xft/DPI {}", m_xsettingsSerial, m_xsettings.windowScalingFactor,
        m_xsettings.unscaledDpi, m_xsettings.xftDpi
    );
  }

} // namespace umbriel
