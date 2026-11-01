#include "xwayland/primary.h"

#include "core/log.h"
#include "wlr.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <xcb/randr.h>
#include <xcb/xcbext.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("xwayland");
    constexpr auto kReplyTimeout = std::chrono::seconds(2);

    template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;

    template <typename T> Reply<T> takeReply(xcb_connection_t* connection, uint32_t& cookie, bool& failed) {
      void* reply = nullptr;
      xcb_generic_error_t* error = nullptr;
      if (cookie != 0 && xcb_poll_for_reply(connection, cookie, &reply, &error) != 0) {
        cookie = 0;
        failed |= reply == nullptr || error != nullptr;
      }
      std::free(error);
      return Reply<T>(static_cast<T*>(reply), std::free);
    }
  } // namespace

  XwaylandPrimary::XwaylandPrimary(wl_display* display) {
    m_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), onTimer, this);
  }

  XwaylandPrimary::~XwaylandPrimary() {
    stop();
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  void XwaylandPrimary::start(wlr_xwayland* xwayland) {
    stop();
    m_xwayland = xwayland;
    m_connection = wlr_xwayland_get_xwm_connection(xwayland);
    if (m_connection == nullptr || m_timer == nullptr) {
      return;
    }
    const xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    if (screens.rem == 0) {
      stop();
      return;
    }
    m_root = screens.data->root;
    m_wakeupWindow = xcb_generate_id(m_connection);
    xcb_create_window(
        m_connection, 0, m_wakeupWindow, m_root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0,
        nullptr
    );
    // The explicit query is a reply barrier for libxcb's prefetched extension cache, as in XwaylandShapes.
    xcb_prefetch_extension_data(m_connection, &xcb_randr_id);
    constexpr char kRandr[] = "RANDR";
    m_extensionCookie = xcb_query_extension(m_connection, sizeof(kRandr) - 1, kRandr).sequence;
    m_requested = std::chrono::steady_clock::now();
    m_dirty = m_output.has_value();
    xcb_flush(m_connection);
    wl_event_source_timer_update(m_timer, 1);
  }

  void XwaylandPrimary::stop() {
    if (m_timer != nullptr) {
      wl_event_source_timer_update(m_timer, 0);
    }
    // The XWM connection may already be destroyed. Only discard saved state here.
    m_connection = nullptr;
    m_xwayland = nullptr;
    m_available = m_clear = m_dirty = false;
    m_firstEvent = 0;
    m_root = m_wakeupWindow = 0;
    m_extensionCookie = m_versionCookie = m_resourcesCookie = m_primaryCookie = 0;
    m_current = m_candidate = 0;
    m_outputs.clear();
  }

  void XwaylandPrimary::setOutput(std::optional<std::string> name) {
    if (name == m_output) {
      return;
    }
    m_clear |= m_output.has_value() && !name.has_value();
    m_output = std::move(name);
    refresh();
  }

  void XwaylandPrimary::refresh() {
    if (m_connection == nullptr || m_timer == nullptr || (!m_output && !m_clear)) {
      return;
    }
    m_dirty = true;
    wl_event_source_timer_update(m_timer, 1);
  }

  bool XwaylandPrimary::handleEvent(xcb_generic_event_t* event) {
    const uint8_t type = event->response_type & 0x7f;
    if ((type == XCB_CREATE_NOTIFY
         && reinterpret_cast<const xcb_create_notify_event_t*>(event)->window == m_wakeupWindow)
        || (type == XCB_CLIENT_MESSAGE
            && reinterpret_cast<const xcb_client_message_event_t*>(event)->window == m_wakeupWindow)) {
      return true;
    }
    if (m_available
        && (type == m_firstEvent + XCB_RANDR_SCREEN_CHANGE_NOTIFY || type == m_firstEvent + XCB_RANDR_NOTIFY)) {
      refresh();
    }
    return false;
  }

  void XwaylandPrimary::request() {
    m_dirty = false;
    m_candidate = 0;
    m_outputs.clear();
    if (m_output && !m_output->empty()) {
      m_resourcesCookie = xcb_randr_get_screen_resources_current(m_connection, m_root).sequence;
    }
    m_primaryCookie = xcb_randr_get_output_primary(m_connection, m_root).sequence;
    m_requested = std::chrono::steady_clock::now();
    xcb_flush(m_connection);
  }

  void XwaylandPrimary::discard() {
    for (uint32_t* cookie : {&m_extensionCookie, &m_versionCookie, &m_resourcesCookie, &m_primaryCookie}) {
      if (*cookie != 0) {
        xcb_discard_reply(m_connection, *cookie);
        *cookie = 0;
      }
    }
    for (const Output& output : m_outputs) {
      if (output.cookie != 0) {
        xcb_discard_reply(m_connection, output.cookie);
      }
    }
    m_outputs.clear();
  }

  void XwaylandPrimary::wakeWindowManager() {
    // Reply polling may queue unrelated X events. A fresh readable event makes wlroots drain those too.
    xcb_client_message_event_t wake{};
    wake.response_type = XCB_CLIENT_MESSAGE;
    wake.format = 32;
    wake.window = m_wakeupWindow;
    xcb_send_event(m_connection, 0, m_wakeupWindow, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&wake));
    xcb_flush(m_connection);
  }

  int XwaylandPrimary::onTimer(void* data) {
    static_cast<XwaylandPrimary*>(data)->poll();
    return 0;
  }

  bool XwaylandPrimary::pending() const {
    return m_extensionCookie != 0
        || m_versionCookie != 0
        || m_resourcesCookie != 0
        || m_primaryCookie != 0
        || std::ranges::any_of(m_outputs, [](const Output& output) { return output.cookie != 0; });
  }

  void XwaylandPrimary::poll() {
    if (m_connection == nullptr
        || wlr_xwayland_get_xwm_connection(m_xwayland) != m_connection
        || xcb_connection_has_error(m_connection) != 0) {
      stop();
      return;
    }
    if (pending() && std::chrono::steady_clock::now() - m_requested >= kReplyTimeout) {
      kLog.warn("timed out updating the Xwayland primary output");
      discard();
      wakeWindowManager();
      m_dirty = false;
      if (!m_available) {
        stop();
      }
      return;
    }
    bool failed = false;
    if (const auto reply = takeReply<xcb_query_extension_reply_t>(m_connection, m_extensionCookie, failed)) {
      if (reply->present == 0) {
        kLog.warn("Xwayland does not support RandR primary outputs");
        stop();
        return;
      }
      m_firstEvent = reply->first_event;
      m_versionCookie = xcb_randr_query_version(m_connection, 1, 4).sequence;
      xcb_flush(m_connection);
    }
    if (const auto reply = takeReply<xcb_randr_query_version_reply_t>(m_connection, m_versionCookie, failed)) {
      if (reply->major_version < 1 || (reply->major_version == 1 && reply->minor_version < 3)) {
        kLog.warn("Xwayland needs RandR 1.3 for primary outputs");
        stop();
        return;
      }
      m_available = true;
      uint16_t mask =
          XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE;
      if (reply->major_version > 1 || reply->minor_version >= 4) {
        mask |= XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE;
      }
      xcb_randr_select_input(m_connection, m_root, mask);
      xcb_flush(m_connection);
    }
    if (const auto reply =
            takeReply<xcb_randr_get_screen_resources_current_reply_t>(m_connection, m_resourcesCookie, failed)) {
      const xcb_randr_output_t* outputs = xcb_randr_get_screen_resources_current_outputs(reply.get());
      const int count = xcb_randr_get_screen_resources_current_outputs_length(reply.get());
      for (int i = 0; i < count; ++i) {
        m_outputs.push_back({
            .id = outputs[i],
            .cookie = xcb_randr_get_output_info(m_connection, outputs[i], reply->config_timestamp).sequence,
        });
      }
      xcb_flush(m_connection);
    }
    if (const auto reply = takeReply<xcb_randr_get_output_primary_reply_t>(m_connection, m_primaryCookie, failed)) {
      m_current = reply->output;
    }
    for (Output& output : m_outputs) {
      if (const auto reply = takeReply<xcb_randr_get_output_info_reply_t>(m_connection, output.cookie, failed)) {
        const std::string_view name(
            reinterpret_cast<const char*>(xcb_randr_get_output_info_name(reply.get())), reply->name_len
        );
        if (m_output
            && name == *m_output
            && reply->crtc != XCB_NONE
            && reply->connection == XCB_RANDR_CONNECTION_CONNECTED) {
          m_candidate = output.id;
        }
      }
    }
    if (failed) {
      kLog.warn("could not query the Xwayland primary output");
      discard();
    }
    if (!failed && !pending() && m_available) {
      if (m_dirty) {
        request();
      } else if (m_output || m_clear) {
        if (m_current != m_candidate) {
          xcb_randr_set_output_primary(m_connection, m_root, m_candidate);
          kLog.debug("Xwayland primary output: {}", m_candidate != 0 && m_output ? *m_output : "none");
        }
        m_clear = false;
      }
    }
    wakeWindowManager();
    if (failed && !m_available) {
      stop();
    } else if (pending() || m_dirty) {
      wl_event_source_timer_update(m_timer, 1);
    }
  }

} // namespace umbriel
