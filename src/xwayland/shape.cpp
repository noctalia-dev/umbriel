#include "xwayland/shape.h"

#include "core/log.h"
#include "wlr.h"

#include <cstdlib>
#include <vector>
#include <xcb/shape.h>
#include <xcb/xcbext.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("xwayland");
    constexpr auto kReplyTimeout = std::chrono::seconds(2);
  } // namespace

  struct XwaylandShapes::Surface {
    explicit Surface(wlr_xwayland_surface* xsurface) : xsurface(xsurface) { pixman_region32_init(&region); }
    ~Surface() { pixman_region32_fini(&region); }

    wlr_xwayland_surface* xsurface;
    pixman_region32_t region;
    uint32_t extentsCookie = 0;
    uint32_t rectanglesCookie = 0;
    bool known = false;
    bool shaped = false;
    std::chrono::steady_clock::time_point requested;
  };

  XwaylandShapes::XwaylandShapes(wl_display* display) {
    wl_signal_init(&m_changed);
    m_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), onTimer, this);
  }

  XwaylandShapes::~XwaylandShapes() {
    stop();
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  void XwaylandShapes::start(wlr_xwayland* xwayland) {
    stop();
    m_xwayland = xwayland;
    m_connection = wlr_xwayland_get_xwm_connection(xwayland);
    if (m_connection == nullptr || m_timer == nullptr) {
      return;
    }
    const xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    if (screens.rem == 0) {
      m_connection = nullptr;
      return;
    }
    m_wakeupWindow = xcb_generate_id(m_connection);
    xcb_create_window(
        m_connection, 0, m_wakeupWindow, screens.data->root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT, 0, nullptr
    );
    // Generated SHAPE requests use libxcb's extension cache. The second query is a reply barrier: once it is ready,
    // the prefetched cache reply is already available, so generated requests cannot block discovering the opcode.
    xcb_prefetch_extension_data(m_connection, &xcb_shape_id);
    constexpr char kShape[] = "SHAPE";
    m_extensionCookie = xcb_query_extension(m_connection, sizeof(kShape) - 1, kShape).sequence;
    m_extensionStarted = std::chrono::steady_clock::now();
    xcb_flush(m_connection);
    arm();
  }

  void XwaylandShapes::stop() {
    if (m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, 0);
    }
    // The XWM may already have disconnected. Never dereference a saved connection during lazy-server teardown.
    m_connection = nullptr;
    m_xwayland = nullptr;
    m_extensionCookie = 0;
    m_wakeupWindow = 0;
    m_firstEvent = 0;
    m_available = false;
    m_surfaces.clear();
  }

  void XwaylandShapes::add(wlr_xwayland_surface* xsurface) {
    auto surface = std::make_unique<Surface>(xsurface);
    if (m_available) {
      xcb_shape_select_input(m_connection, xsurface->window_id, 1);
      request(*surface);
    } else if (m_extensionCookie == 0) {
      surface->known = true;
    }
    m_surfaces.insert_or_assign(xsurface->window_id, std::move(surface));
    arm();
  }

  void XwaylandShapes::remove(wlr_xwayland_surface* xsurface) {
    const auto found = m_surfaces.find(xsurface->window_id);
    if (found == m_surfaces.end()) {
      return;
    }
    if (m_connection != nullptr && wlr_xwayland_get_xwm_connection(m_xwayland) == m_connection) {
      discard(*found->second);
    }
    m_surfaces.erase(found);
  }

  const pixman_region32_t* XwaylandShapes::shape(wlr_surface* surface) const {
    if (surface == nullptr) {
      return nullptr;
    }
    const wlr_xwayland_surface* xsurface = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xsurface == nullptr) {
      return nullptr;
    }
    const auto found = m_surfaces.find(xsurface->window_id);
    if (found == m_surfaces.end()) {
      return nullptr;
    }
    const Surface& shape = *found->second;
    return !shape.known || shape.shaped ? &shape.region : nullptr;
  }

  void XwaylandShapes::discard(Surface& surface) {
    if (surface.extentsCookie != 0) {
      xcb_discard_reply(m_connection, surface.extentsCookie);
      surface.extentsCookie = 0;
    }
    if (surface.rectanglesCookie != 0) {
      xcb_discard_reply(m_connection, surface.rectanglesCookie);
      surface.rectanglesCookie = 0;
    }
  }

  void XwaylandShapes::request(Surface& surface) {
    discard(surface);
    surface.extentsCookie = xcb_shape_query_extents(m_connection, surface.xsurface->window_id).sequence;
    surface.rectanglesCookie =
        xcb_shape_get_rectangles(m_connection, surface.xsurface->window_id, XCB_SHAPE_SK_BOUNDING).sequence;
    surface.requested = std::chrono::steady_clock::now();
    xcb_flush(m_connection);
    arm();
  }

  void XwaylandShapes::resolve(Surface& surface, bool shaped) {
    surface.known = true;
    surface.shaped = shaped;
    if (!shaped) {
      pixman_region32_clear(&surface.region);
    }
  }

  void XwaylandShapes::notify(uint32_t window) {
    const auto found = m_surfaces.find(window);
    if (found != m_surfaces.end() && found->second->xsurface->surface != nullptr) {
      wl_signal_emit_mutable(&m_changed, found->second->xsurface->surface);
    }
  }

  bool XwaylandShapes::handleEvent(xcb_generic_event_t* event) {
    const uint8_t type = event->response_type & 0x7f;
    if (type == XCB_CREATE_NOTIFY
        && reinterpret_cast<const xcb_create_notify_event_t*>(event)->window == m_wakeupWindow) {
      return true;
    }
    if (type == XCB_CLIENT_MESSAGE) {
      const auto* message = reinterpret_cast<const xcb_client_message_event_t*>(event);
      if (m_wakeupWindow != 0 && message->window == m_wakeupWindow) {
        return true;
      }
    }
    if (!m_available || type != m_firstEvent + XCB_SHAPE_NOTIFY) {
      return false;
    }
    const auto* shape = reinterpret_cast<const xcb_shape_notify_event_t*>(event);
    if (shape->shape_kind != XCB_SHAPE_SK_BOUNDING) {
      return true;
    }
    const auto found = m_surfaces.find(shape->affected_window);
    if (found == m_surfaces.end()) {
      return true;
    }
    Surface& surface = *found->second;
    if (shape->shaped != 0) {
      // Keep the old mask until the complete new region arrives; notification extents lose holes and disjoint parts.
      request(surface);
    } else {
      discard(surface);
      resolve(surface, false);
      notify(shape->affected_window);
    }
    return true;
  }

  void XwaylandShapes::arm() {
    if (m_timer != nullptr && m_connection != nullptr) {
      wl_event_source_timer_update(m_timer, 1);
    }
  }

  void XwaylandShapes::wakeWindowManager() {
    // Polling replies can move unrelated events from the socket into libxcb's queue. Give wlroots a fresh readable
    // event so its XWM dispatches that queue even when no other X client sends anything afterwards.
    xcb_client_message_event_t wake{};
    wake.response_type = XCB_CLIENT_MESSAGE;
    wake.format = 32;
    wake.window = m_wakeupWindow;
    xcb_send_event(m_connection, 0, m_wakeupWindow, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&wake));
    xcb_flush(m_connection);
  }

  int XwaylandShapes::onTimer(void* data) {
    static_cast<XwaylandShapes*>(data)->poll();
    return 0;
  }

  void XwaylandShapes::poll() {
    if (m_connection == nullptr || wlr_xwayland_get_xwm_connection(m_xwayland) != m_connection) {
      stop();
      return;
    }
    std::vector<uint32_t> changed;
    const auto now = std::chrono::steady_clock::now();
    if (xcb_connection_has_error(m_connection) != 0) {
      m_available = false;
      m_extensionCookie = 0;
      for (auto& [window, surface] : m_surfaces) {
        surface->extentsCookie = surface->rectanglesCookie = 0;
        resolve(*surface, false);
        changed.push_back(window);
      }
    } else {
      if (m_extensionCookie != 0) {
        void* reply = nullptr;
        xcb_generic_error_t* error = nullptr;
        const bool ready = xcb_poll_for_reply(m_connection, m_extensionCookie, &reply, &error) != 0;
        if (ready || now - m_extensionStarted >= kReplyTimeout) {
          const auto* extension = static_cast<xcb_query_extension_reply_t*>(reply);
          m_available = extension != nullptr && error == nullptr && extension->present != 0;
          m_firstEvent = m_available ? extension->first_event : 0;
          if (!ready) {
            xcb_discard_reply(m_connection, m_extensionCookie);
          }
          m_extensionCookie = 0;
          for (auto& [window, surface] : m_surfaces) {
            if (m_available) {
              xcb_shape_select_input(m_connection, window, 1);
              request(*surface);
            } else {
              resolve(*surface, false);
              changed.push_back(window);
            }
          }
        }
        std::free(reply);
        std::free(error);
      }
      for (auto& [window, ptr] : m_surfaces) {
        Surface& surface = *ptr;
        if (surface.extentsCookie == 0 && surface.rectanglesCookie == 0) {
          continue;
        }
        if (now - surface.requested >= kReplyTimeout) {
          discard(surface);
          if (!surface.known) {
            resolve(surface, false);
          }
          changed.push_back(window);
          kLog.warn("timed out reading X11 bounding shape for window {}", window);
          continue;
        }
        if (surface.extentsCookie != 0) {
          void* reply = nullptr;
          xcb_generic_error_t* error = nullptr;
          if (xcb_poll_for_reply(m_connection, surface.extentsCookie, &reply, &error) == 0) {
            continue;
          }
          surface.extentsCookie = 0;
          const auto* extents = static_cast<xcb_shape_query_extents_reply_t*>(reply);
          const bool valid = extents != nullptr && error == nullptr;
          const bool shaped = valid && extents->bounding_shaped != 0;
          std::free(reply);
          std::free(error);
          if (!shaped) {
            discard(surface);
            if (valid || !surface.known) {
              resolve(surface, false);
            }
            changed.push_back(window);
            continue;
          }
        }
        void* reply = nullptr;
        xcb_generic_error_t* error = nullptr;
        if (xcb_poll_for_reply(m_connection, surface.rectanglesCookie, &reply, &error) == 0) {
          continue;
        }
        surface.rectanglesCookie = 0;
        auto* rectangles = static_cast<xcb_shape_get_rectangles_reply_t*>(reply);
        const bool shaped = rectangles != nullptr && error == nullptr;
        if (shaped) {
          pixman_region32_clear(&surface.region);
          const xcb_rectangle_t* rects = xcb_shape_get_rectangles_rectangles(rectangles);
          const int count = xcb_shape_get_rectangles_rectangles_length(rectangles);
          for (int i = 0; i < count; ++i) {
            pixman_region32_union_rect(
                &surface.region, &surface.region, rects[i].x, rects[i].y, rects[i].width, rects[i].height
            );
          }
        }
        if (shaped || !surface.known) {
          resolve(surface, shaped);
        }
        changed.push_back(window);
        std::free(reply);
        std::free(error);
      }
      wakeWindowManager();
    }
    bool pending = m_extensionCookie != 0;
    for (const auto& [window, surface] : m_surfaces) {
      pending |= surface->extentsCookie != 0 || surface->rectanglesCookie != 0;
    }
    if (pending) {
      arm();
    }
    for (uint32_t window : changed) {
      notify(window);
    }
  }

} // namespace umbriel
