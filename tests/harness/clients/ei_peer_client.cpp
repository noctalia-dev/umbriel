// Tries the private Xwayland EIS endpoint from a different process. A claimed application name must not bypass the
// compositor's socket-peer check; rejection is observed through the real libei handshake.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <libei.h>
#include <poll.h>
#include <print>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::println(stderr, "usage: ei-peer-client SOCKET CLAIMED_NAME");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  ei* context = ei_new_sender(nullptr);
  if (context == nullptr) {
    return 1;
  }
  ei_configure_name(context, argv[2]);
  const int result = ei_setup_backend_socket(context, argv[1]);
  if (result != 0) {
    std::println(stderr, "ei-peer-client: socket setup failed: {}", std::strerror(-result));
    ei_unref(context);
    return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    ei_dispatch(context);
    while (ei_event* event = ei_get_event(context)) {
      const auto type = ei_event_get_type(event);
      ei_event_unref(event);
      if (type == EI_EVENT_DISCONNECT) {
        std::println("rejected");
        ei_unref(context);
        return 0;
      }
      if (type == EI_EVENT_SEAT_ADDED || type == EI_EVENT_DEVICE_ADDED) {
        std::println(stderr, "ei-peer-client: untrusted peer received an input seat");
        ei_unref(context);
        return 1;
      }
    }
    pollfd fd{.fd = ei_get_fd(context), .events = POLLIN, .revents = 0};
    if (poll(&fd, 1, 100) < 0 && errno != EINTR) {
      std::println(stderr, "ei-peer-client: poll failed: {}", std::strerror(errno));
      ei_unref(context);
      return 1;
    }
  }
  std::println(stderr, "ei-peer-client: private endpoint did not reject an untrusted peer");
  ei_unref(context);
  return 1;
}
