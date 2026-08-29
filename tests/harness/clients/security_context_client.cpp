#include "security-context-v1-client-protocol.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct State {
    wp_security_context_manager_v1* manager = nullptr;
  };

  void handleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, wp_security_context_manager_v1_interface.name) == 0) {
      state->manager = static_cast<wp_security_context_manager_v1*>(
          wl_registry_bind(registry, name, &wp_security_context_manager_v1_interface, 1)
      );
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };

  int childStatus(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
      if (errno != EINTR) {
        std::println(stderr, "security-context-client: waitpid failed: {}", std::strerror(errno));
        return 2;
      }
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
      return 128 + WTERMSIG(status);
    }
    return 2;
  }
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(stderr, "usage: security-context-client COMMAND [ARG ...]");
    return 2;
  }

  const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
  if (runtimeDir == nullptr || *runtimeDir == '\0') {
    std::println(stderr, "security-context-client: XDG_RUNTIME_DIR is not set");
    return 2;
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "security-context-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(display) < 0 || state.manager == nullptr) {
    std::println(stderr, "security-context-client: compositor does not offer wp_security_context_manager_v1");
    wl_display_disconnect(display);
    return 2;
  }
  wl_registry_destroy(registry);

  const std::string socketName = "umbriel-security-context-" + std::to_string(getpid());
  const std::string socketPath = std::string(runtimeDir) + "/" + socketName;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(address.sun_path)) {
    std::println(stderr, "security-context-client: socket path is too long");
    wl_display_disconnect(display);
    return 2;
  }
  std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);

  const int listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listenFd < 0
      || bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0
      || listen(listenFd, 1) < 0) {
    std::println(stderr, "security-context-client: cannot create listener: {}", std::strerror(errno));
    if (listenFd >= 0) {
      close(listenFd);
    }
    wl_display_disconnect(display);
    unlink(socketPath.c_str());
    return 2;
  }

  int closePipe[2];
  if (pipe2(closePipe, O_CLOEXEC) < 0) {
    std::println(stderr, "security-context-client: pipe2 failed: {}", std::strerror(errno));
    close(listenFd);
    wl_display_disconnect(display);
    unlink(socketPath.c_str());
    return 2;
  }

  wp_security_context_v1* context =
      wp_security_context_manager_v1_create_listener(state.manager, listenFd, closePipe[0]);
  wp_security_context_v1_set_sandbox_engine(context, "org.umbriel.harness");
  wp_security_context_v1_set_app_id(context, "org.umbriel.SecurityContextTest");
  wp_security_context_v1_set_instance_id(context, socketName.c_str());
  wp_security_context_v1_commit(context);
  close(listenFd);
  close(closePipe[0]);

  if (wl_display_roundtrip(display) < 0) {
    std::println(stderr, "security-context-client: security context commit failed");
    close(closePipe[1]);
    wl_display_disconnect(display);
    unlink(socketPath.c_str());
    return 2;
  }

  // The listener must remain usable after the client that created it leaves.
  wl_display_disconnect(display);

  const pid_t child = fork();
  if (child < 0) {
    std::println(stderr, "security-context-client: fork failed: {}", std::strerror(errno));
    close(closePipe[1]);
    unlink(socketPath.c_str());
    return 2;
  }
  if (child == 0) {
    close(closePipe[1]);
    setenv("WAYLAND_DISPLAY", socketName.c_str(), 1);
    unsetenv("WAYLAND_SOCKET");
    execvp(argv[1], argv + 1);
    std::println(stderr, "security-context-client: cannot execute '{}': {}", argv[1], std::strerror(errno));
    _exit(127);
  }

  const int status = childStatus(child);
  close(closePipe[1]);
  unlink(socketPath.c_str());
  return status;
}
