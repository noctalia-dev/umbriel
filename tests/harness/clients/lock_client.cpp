// Takes an ext-session-lock, paints one lock surface per output, and reports
// "locked" once the compositor confirms the lock. Any line on stdin unlocks the
// session and prints "unlocked", which is what lets a check compare focus
// before and after a real lock cycle. "finished" means the compositor refused
// or dropped the lock.

#include "ext-session-lock-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
  struct State;

  struct LockOutput {
    State* state = nullptr;
    wl_output* output = nullptr;
    wl_surface* surface = nullptr;
    ext_session_lock_surface_v1* lockSurface = nullptr;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    ext_session_lock_manager_v1* manager = nullptr;
    ext_session_lock_v1* lock = nullptr;
    std::vector<LockOutput> outputs;
    bool locked = false;
    bool finished = false;
  };

  wl_buffer* createBuffer(State& state, int width, int height) {
    const int stride = width * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    const int fd = memfd_create("umbriel-lock-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return nullptr;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return nullptr;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), 0xFF102030);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    return buffer;
  }

  // A lock surface must ack every configure and commit a buffer of the size it
  // was given, or the compositor keeps waiting and never reports the lock.
  void lockSurfaceConfigure(
      void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height
  ) {
    auto& entry = *static_cast<LockOutput*>(data);
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    wl_buffer* buffer = createBuffer(*entry.state, static_cast<int>(width), static_cast<int>(height));
    if (buffer != nullptr) {
      wl_surface_attach(entry.surface, buffer, 0, 0);
      wl_surface_damage_buffer(entry.surface, 0, 0, static_cast<int>(width), static_cast<int>(height));
    }
    wl_surface_commit(entry.surface);
  }
  constexpr ext_session_lock_surface_v1_listener kLockSurfaceListener = {.configure = lockSurfaceConfigure};

  void lockLocked(void* data, ext_session_lock_v1*) {
    static_cast<State*>(data)->locked = true;
    std::println("locked");
    std::fflush(stdout);
  }
  void lockFinished(void* data, ext_session_lock_v1*) {
    static_cast<State*>(data)->finished = true;
    std::println("finished");
    std::fflush(stdout);
  }
  constexpr ext_session_lock_v1_listener kLockListener = {.locked = lockLocked, .finished = lockFinished};

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
      state.manager = static_cast<ext_session_lock_manager_v1*>(
          wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1)
      );
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
      LockOutput entry;
      entry.state = &state;
      entry.output =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 3U)));
      state.outputs.push_back(entry);
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

  // Dispatches Wayland events until the lock settles or stdin asks for the unlock.
  bool pumpUntilLocked(State& state) {
    while (!state.locked && !state.finished) {
      if (wl_display_dispatch(state.display) < 0) {
        return false;
      }
    }
    return state.locked;
  }

  bool waitForUnlockRequest(State& state) {
    const int displayFd = wl_display_get_fd(state.display);
    while (true) {
      wl_display_flush(state.display);
      pollfd fds[2] = {
          {.fd = displayFd, .events = POLLIN, .revents = 0},
          {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
      };
      if (poll(fds, 2, -1) < 0) {
        return false;
      }
      if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
        return false;
      }
      if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        char buffer[64];
        const ssize_t read = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        return read > 0;
      }
      if (state.finished) {
        return false;
      }
    }
  }
} // namespace

int main() {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "lock-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.manager == nullptr || state.outputs.empty()) {
    std::println(stderr, "lock-client: missing required global");
    return EXIT_FAILURE;
  }

  state.lock = ext_session_lock_manager_v1_lock(state.manager);
  ext_session_lock_v1_add_listener(state.lock, &kLockListener, &state);
  for (LockOutput& entry : state.outputs) {
    entry.surface = wl_compositor_create_surface(state.compositor);
    entry.lockSurface = ext_session_lock_v1_get_lock_surface(state.lock, entry.surface, entry.output);
    ext_session_lock_surface_v1_add_listener(entry.lockSurface, &kLockSurfaceListener, &entry);
  }
  wl_display_flush(state.display);

  if (!pumpUntilLocked(state)) {
    std::println(stderr, "lock-client: the session never locked");
    return EXIT_FAILURE;
  }
  if (!waitForUnlockRequest(state)) {
    std::println(stderr, "lock-client: lost the lock before unlocking");
    return EXIT_FAILURE;
  }

  ext_session_lock_v1_unlock_and_destroy(state.lock);
  wl_display_roundtrip(state.display);
  std::println("unlocked");
  std::fflush(stdout);
  return EXIT_SUCCESS;
}
