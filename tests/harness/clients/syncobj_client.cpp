// Harness helper that maps a toplevel with one explicitly synchronized dmabuf, then waits for the compositor to signal
// a linux-drm-syncobj release point.
// Usage: syncobj-client role-destroy|unmap|superseded
//   role-destroy  destroys the xdg role before attaching NULL, the way Chromium hides a window for a tab drag, then
//                 waits for the toplevel buffer's release
//   unmap         attaches NULL and keeps the role, and waits for the toplevel buffer's release
//   superseded    commits two buffers to a new synchronized subsurface without committing the toplevel again, the way
//                 Firefox renders a prompt panel before its main thread commits the parent, and waits for the first
//                 buffer's release
// Prints "mapped" once the toplevel buffer has been shown and "released" once the release point signals; exits 1 if
// the point is still pending after five seconds.
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <print>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

namespace {

  constexpr int kSize = 64;
  constexpr auto kReleaseTimeout = std::chrono::seconds(5);

  struct State {
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    xdg_wm_base* wmBase = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    wp_linux_drm_syncobj_manager_v1* syncobj = nullptr;
    dev_t mainDevice = 0;
    bool feedbackDone = false;
    bool configured = false;
    uint32_t configureSerial = 0;
    bool frameDone = false;
  };

  void handleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    const std::string_view advertised = interface;
    if (advertised == wl_compositor_interface.name) {
      state->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (advertised == wl_subcompositor_interface.name) {
      state->subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (advertised == xdg_wm_base_interface.name) {
      state->wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (advertised == zwp_linux_dmabuf_v1_interface.name && version >= 4) {
      state->dmabuf =
          static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4));
    } else if (advertised == wp_linux_drm_syncobj_manager_v1_interface.name) {
      state->syncobj = static_cast<wp_linux_drm_syncobj_manager_v1*>(
          wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1)
      );
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };

  void handleFeedbackDone(void* data, zwp_linux_dmabuf_feedback_v1*) { static_cast<State*>(data)->feedbackDone = true; }

  void handleFormatTable(void*, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t) { close(fd); }

  void handleMainDevice(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) {
    if (device->size == sizeof(dev_t)) {
      std::memcpy(&static_cast<State*>(data)->mainDevice, device->data, sizeof(dev_t));
    }
  }

  void handleTrancheDone(void*, zwp_linux_dmabuf_feedback_v1*) {}
  void handleTrancheTargetDevice(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
  void handleTrancheFormats(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
  void handleTrancheFlags(void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {}

  constexpr zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
      .done = handleFeedbackDone,
      .format_table = handleFormatTable,
      .main_device = handleMainDevice,
      .tranche_done = handleTrancheDone,
      .tranche_target_device = handleTrancheTargetDevice,
      .tranche_formats = handleTrancheFormats,
      .tranche_flags = handleTrancheFlags,
  };

  void handlePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = handlePing};

  void handleSurfaceConfigure(void* data, xdg_surface*, uint32_t serial) {
    auto* state = static_cast<State*>(data);
    state->configured = true;
    state->configureSerial = serial;
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {.configure = handleSurfaceConfigure};

  void handleToplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void handleToplevelClose(void*, xdg_toplevel*) {}
  void handleToplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void handleToplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = handleToplevelConfigure,
      .close = handleToplevelClose,
      .configure_bounds = handleToplevelBounds,
      .wm_capabilities = handleToplevelCapabilities,
  };

  void handleFrameDone(void* data, wl_callback* callback, uint32_t) {
    static_cast<State*>(data)->frameDone = true;
    wl_callback_destroy(callback);
  }

  constexpr wl_callback_listener kFrameListener = {.done = handleFrameDone};

  wp_linux_drm_syncobj_timeline_v1* importTimeline(State& state, int drmFd, uint32_t handle) {
    int fd = -1;
    if (drmSyncobjHandleToFD(drmFd, handle, &fd) != 0) {
      return nullptr;
    }
    wp_linux_drm_syncobj_timeline_v1* timeline = wp_linux_drm_syncobj_manager_v1_import_timeline(state.syncobj, fd);
    close(fd);
    return timeline;
  }

  int openRenderNode(dev_t device) {
    drmDevice* drm = nullptr;
    if (drmGetDeviceFromDevId(device, 0, &drm) != 0) {
      return -1;
    }
    int fd = -1;
    if ((drm->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
      fd = open(drm->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
    }
    drmFreeDevice(&drm);
    return fd;
  }

  wl_buffer* createBuffer(State& state, gbm_device* gbm) {
    gbm_bo* bo = gbm_bo_create(gbm, kSize, kSize, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (bo == nullptr) {
      return nullptr;
    }
    const uint64_t modifier = gbm_bo_get_modifier(bo);
    const int boFd = gbm_bo_get_fd(bo);
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(state.dmabuf);
    zwp_linux_buffer_params_v1_add(
        params, boFd, 0, gbm_bo_get_offset(bo, 0), gbm_bo_get_stride(bo), static_cast<uint32_t>(modifier >> 32),
        static_cast<uint32_t>(modifier & 0xFFFFFFFFU)
    );
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, kSize, kSize, GBM_FORMAT_XRGB8888, 0);
    close(boFd);
    return buffer;
  }

  bool dispatchUntil(wl_display* display, const bool& done) {
    while (!done) {
      if (wl_display_dispatch(display) < 0) {
        return false;
      }
    }
    return true;
  }

} // namespace

int main(int argc, char** argv) {
  const std::string_view mode = argc == 2 ? argv[1] : "";
  if (mode != "role-destroy" && mode != "unmap" && mode != "superseded") {
    std::println(stderr, "usage: syncobj-client role-destroy|unmap|superseded");
    return 2;
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "syncobj-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }
  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  if (state.compositor == nullptr
      || state.subcompositor == nullptr
      || state.wmBase == nullptr
      || state.dmabuf == nullptr
      || state.syncobj == nullptr) {
    std::println(stderr, "syncobj-client: the compositor does not advertise linux-dmabuf v4 and linux-drm-syncobj");
    return 1;
  }
  xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);

  zwp_linux_dmabuf_feedback_v1* feedback = zwp_linux_dmabuf_v1_get_default_feedback(state.dmabuf);
  zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedbackListener, &state);
  if (!dispatchUntil(display, state.feedbackDone)) {
    return 1;
  }
  const int drmFd = openRenderNode(state.mainDevice);
  if (drmFd < 0) {
    std::println(stderr, "syncobj-client: cannot open the compositor's render node");
    return 1;
  }

  gbm_device* gbm = gbm_create_device(drmFd);
  wl_buffer* buffer = gbm != nullptr ? createBuffer(state, gbm) : nullptr;
  if (buffer == nullptr) {
    std::println(stderr, "syncobj-client: cannot allocate a dmabuf");
    return 1;
  }

  uint32_t acquire = 0;
  uint32_t release = 0;
  uint64_t point = 1;
  if (drmSyncobjCreate(drmFd, 0, &acquire) != 0
      || drmSyncobjCreate(drmFd, 0, &release) != 0
      || drmSyncobjTimelineSignal(drmFd, &acquire, &point, 1) != 0) {
    std::println(stderr, "syncobj-client: cannot create timelines");
    return 1;
  }
  wp_linux_drm_syncobj_timeline_v1* acquireTimeline = importTimeline(state, drmFd, acquire);
  wp_linux_drm_syncobj_timeline_v1* releaseTimeline = importTimeline(state, drmFd, release);
  if (acquireTimeline == nullptr || releaseTimeline == nullptr) {
    std::println(stderr, "syncobj-client: cannot export timelines");
    return 1;
  }

  wl_surface* surface = wl_compositor_create_surface(state.compositor);
  wp_linux_drm_syncobj_surface_v1* syncSurface = wp_linux_drm_syncobj_manager_v1_get_surface(state.syncobj, surface);
  xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, surface);
  xdg_surface_add_listener(xdgSurface, &kXdgSurfaceListener, &state);
  xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
  xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_app_id(toplevel, "syncobj-client");
  wl_surface_commit(surface);
  if (!dispatchUntil(display, state.configured)) {
    return 1;
  }
  xdg_surface_ack_configure(xdgSurface, state.configureSerial);
  wp_linux_drm_syncobj_surface_v1_set_acquire_point(syncSurface, acquireTimeline, 0, 1);
  wp_linux_drm_syncobj_surface_v1_set_release_point(syncSurface, releaseTimeline, 0, 1);
  wl_surface_attach(surface, buffer, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, kSize, kSize);
  wl_callback_add_listener(wl_surface_frame(surface), &kFrameListener, &state);
  wl_surface_commit(surface);
  if (!dispatchUntil(display, state.frameDone)) {
    return 1;
  }
  std::println("mapped");
  std::fflush(stdout);

  uint32_t watched = release;
  if (mode == "superseded") {
    // The toplevel never commits again, so wlroots keeps both child commits cached. The first buffer is replaced
    // before it can be shown; the second stays cached until the parent commits.
    wl_buffer* first = createBuffer(state, gbm);
    wl_buffer* second = createBuffer(state, gbm);
    uint32_t childAcquire = 0;
    uint32_t firstRelease = 0;
    uint32_t secondRelease = 0;
    const std::array<uint64_t, 2> acquirePoints{1, 2};
    if (first == nullptr
        || second == nullptr
        || drmSyncobjCreate(drmFd, 0, &childAcquire) != 0
        || drmSyncobjCreate(drmFd, 0, &firstRelease) != 0
        || drmSyncobjCreate(drmFd, 0, &secondRelease) != 0) {
      std::println(stderr, "syncobj-client: cannot create the subsurface buffers");
      return 1;
    }
    for (const uint64_t acquirePoint : acquirePoints) {
      uint64_t signalled = acquirePoint;
      if (drmSyncobjTimelineSignal(drmFd, &childAcquire, &signalled, 1) != 0) {
        std::println(stderr, "syncobj-client: cannot signal the subsurface acquire points");
        return 1;
      }
    }
    wp_linux_drm_syncobj_timeline_v1* childAcquireTimeline = importTimeline(state, drmFd, childAcquire);
    wp_linux_drm_syncobj_timeline_v1* firstReleaseTimeline = importTimeline(state, drmFd, firstRelease);
    wp_linux_drm_syncobj_timeline_v1* secondReleaseTimeline = importTimeline(state, drmFd, secondRelease);
    if (childAcquireTimeline == nullptr || firstReleaseTimeline == nullptr || secondReleaseTimeline == nullptr) {
      std::println(stderr, "syncobj-client: cannot export the subsurface timelines");
      return 1;
    }
    wl_surface* child = wl_compositor_create_surface(state.compositor);
    wp_linux_drm_syncobj_surface_v1* childSync = wp_linux_drm_syncobj_manager_v1_get_surface(state.syncobj, child);
    wl_subcompositor_get_subsurface(state.subcompositor, child, surface);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(childSync, childAcquireTimeline, 0, 1);
    wp_linux_drm_syncobj_surface_v1_set_release_point(childSync, firstReleaseTimeline, 0, 1);
    wl_surface_attach(child, first, 0, 0);
    wl_surface_damage_buffer(child, 0, 0, kSize, kSize);
    wl_surface_commit(child);
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(childSync, childAcquireTimeline, 0, 2);
    wp_linux_drm_syncobj_surface_v1_set_release_point(childSync, secondReleaseTimeline, 0, 1);
    wl_surface_attach(child, second, 0, 0);
    wl_surface_damage_buffer(child, 0, 0, kSize, kSize);
    wl_surface_commit(child);
    watched = firstRelease;
  }

  const int releaseFd = eventfd(0, EFD_CLOEXEC);
  if (releaseFd < 0 || drmSyncobjEventfd(drmFd, watched, 1, releaseFd, 0) != 0) {
    std::println(stderr, "syncobj-client: cannot watch the release point: {}", std::strerror(errno));
    return 1;
  }
  if (mode == "role-destroy") {
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdgSurface);
  }
  if (mode != "superseded") {
    wl_surface_attach(surface, nullptr, 0, 0);
    wl_surface_commit(surface);
  }

  const auto deadline = std::chrono::steady_clock::now() + kReleaseTimeout;
  while (true) {
    wl_display_dispatch_pending(display);
    wl_display_flush(display);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
      std::println(
          stderr, "syncobj-client: release point still pending {}s after the {} commit", kReleaseTimeout.count(),
          mode == "superseded" ? "replacing" : "NULL"
      );
      return 1;
    }
    std::array<pollfd, 2> fds{{
        {.fd = releaseFd, .events = POLLIN, .revents = 0},
        {.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0},
    }};
    if (poll(fds.data(), fds.size(), static_cast<int>(remaining)) < 0 && errno != EINTR) {
      return 1;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      break;
    }
    if ((fds[1].revents & POLLIN) != 0 && wl_display_dispatch(display) < 0) {
      return 1;
    }
  }
  std::println("released");
  return 0;
}
