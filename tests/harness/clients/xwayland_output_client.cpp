// Reads real RandR output identities and the primary selection, or sets it to the connector passed on the command
// line. No window is mapped, so a separate holder controls lazy Xwayland lifetime in the harness.
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <string_view>
#include <xcb/randr.h>
#include <xcb/xcb.h>

namespace {
  template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
}

int main(int argc, char** argv) {
  const std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection(
      xcb_connect(nullptr, nullptr), xcb_disconnect
  );
  xcb_connection_t* conn = connection.get();
  if (xcb_connection_has_error(conn) != 0) {
    return 1;
  }
  const xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
  const Reply<xcb_randr_query_version_reply_t> version(
      xcb_randr_query_version_reply(conn, xcb_randr_query_version(conn, 1, 3), nullptr), std::free
  );
  if (!version) {
    return 1;
  }
  const Reply<xcb_randr_get_screen_resources_current_reply_t> resources(
      xcb_randr_get_screen_resources_current_reply(conn, xcb_randr_get_screen_resources_current(conn, root), nullptr),
      std::free
  );
  const Reply<xcb_randr_get_output_primary_reply_t> primary(
      xcb_randr_get_output_primary_reply(conn, xcb_randr_get_output_primary(conn, root), nullptr), std::free
  );
  if (!resources || !primary) {
    return 1;
  }
  nlohmann::json state{{"primary", ""}, {"id", primary->output}, {"outputs", nlohmann::json::array()}};
  xcb_randr_output_t selected = XCB_NONE;
  bool found = argc == 1 || std::string_view(argv[1]) == "none";
  const xcb_randr_output_t* ids = xcb_randr_get_screen_resources_current_outputs(resources.get());
  const int count = xcb_randr_get_screen_resources_current_outputs_length(resources.get());
  for (int i = 0; i < count; ++i) {
    const Reply<xcb_randr_get_output_info_reply_t> info(
        xcb_randr_get_output_info_reply(
            conn, xcb_randr_get_output_info(conn, ids[i], resources->config_timestamp), nullptr
        ),
        std::free
    );
    if (!info || info->crtc == XCB_NONE) {
      continue;
    }
    const std::string name(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(info.get())), info->name_len);
    const Reply<xcb_randr_get_crtc_info_reply_t> crtc(
        xcb_randr_get_crtc_info_reply(
            conn, xcb_randr_get_crtc_info(conn, info->crtc, resources->config_timestamp), nullptr
        ),
        std::free
    );
    if (!crtc) {
      return 1;
    }
    state["outputs"].push_back(
        {{"name", name}, {"id", ids[i]}, {"x", crtc->x}, {"y", crtc->y}, {"w", crtc->width}, {"h", crtc->height}}
    );
    if (ids[i] == primary->output) {
      state["primary"] = name;
    }
    if (argc == 2 && name == argv[1]) {
      selected = ids[i];
      found = true;
    }
  }
  if (!found || argc > 2) {
    return 1;
  }
  if (argc == 2) {
    const Reply<xcb_generic_error_t> error(
        xcb_request_check(conn, xcb_randr_set_output_primary_checked(conn, root, selected)), std::free
    );
    return error ? 1 : 0;
  }
  std::println("{}", state.dump());
}
