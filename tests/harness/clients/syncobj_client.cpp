// Harness helper that maps a toplevel with one explicitly synchronized dmabuf, then waits for the compositor to signal
// a linux-drm-syncobj release point.
// Usage: syncobj-client role-destroy|unmap|superseded[-displayed|-shared-timeline|-locked|-reattached|-interleaved]
//   role-destroy  destroys the xdg role before attaching NULL, the way Chromium hides a window for a tab drag, then
//                 waits for the toplevel buffer's release
//   unmap         attaches NULL and keeps the role, and waits for the toplevel buffer's release
// The superseded modes commit to a synchronized subsurface whose parent does not commit again, the way Firefox renders
// a prompt panel before its main thread commits the parent, so wlroots keeps those commits cached:
//   superseded                  queues A then B: A is released, B is not
//   superseded-displayed        shows A, then queues A again on a new timeline and B: neither is released
//   superseded-shared-timeline  shows A at point 1, then queues C at point 2 of that timeline and B: point 1 stays
//                               pending and B is not released
//   superseded-locked           queues A then B before B's acquire fence exists: A is released only once it does,
//                               and B is not released
//   superseded-reattached       queues A at points 1 and 2 of A's timeline, then B: both A points are released, B is
//                               not
//   superseded-interleaved      queues A at point 1, B, A again at point 2, then C, one timeline per buffer: both A
//                               points and B are released, C is not
// Prints "mapped" once the toplevel buffer has been shown. The NULL modes then print "released" once the release point
// signals; the superseded modes print "ok" once every expectation holds. Exits 1 on the first one that does not.
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
  // How long a point must stay pending to count as held.
  constexpr auto kHoldTime = std::chrono::milliseconds(500);

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

  struct Timeline {
    uint32_t handle = 0;
    wp_linux_drm_syncobj_timeline_v1* object = nullptr;
  };

  bool createTimeline(State& state, int drmFd, Timeline& timeline) {
    if (drmSyncobjCreate(drmFd, 0, &timeline.handle) != 0) {
      return false;
    }
    timeline.object = importTimeline(state, drmFd, timeline.handle);
    return timeline.object != nullptr;
  }

  enum class Wait { Signalled, TimedOut, Failed };

  // Dispatches Wayland events until the timeline point signals or the timeout passes.
  Wait
  waitForPoint(wl_display* display, int drmFd, uint32_t handle, uint64_t point, std::chrono::milliseconds timeout) {
    const int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0 || drmSyncobjEventfd(drmFd, handle, point, fd, 0) != 0) {
      std::println(stderr, "syncobj-client: cannot watch a release point: {}", std::strerror(errno));
      if (fd >= 0) {
        close(fd);
      }
      return Wait::Failed;
    }
    Wait result = Wait::TimedOut;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      wl_display_dispatch_pending(display);
      wl_display_flush(display);
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        break;
      }
      std::array<pollfd, 2> fds{{
          {.fd = fd, .events = POLLIN, .revents = 0},
          {.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0},
      }};
      if (poll(fds.data(), fds.size(), static_cast<int>(remaining)) < 0 && errno != EINTR) {
        result = Wait::Failed;
        break;
      }
      if ((fds[0].revents & POLLIN) != 0) {
        result = Wait::Signalled;
        break;
      }
      if ((fds[1].revents & POLLIN) != 0 && wl_display_dispatch(display) < 0) {
        result = Wait::Failed;
        break;
      }
    }
    close(fd);
    return result;
  }

  bool expectReleased(wl_display* display, int drmFd, const Timeline& timeline, uint64_t point, std::string_view what) {
    if (waitForPoint(display, drmFd, timeline.handle, point, kReleaseTimeout) == Wait::Signalled) {
      return true;
    }
    std::println(stderr, "syncobj-client: {} was not released within {}s", what, kReleaseTimeout.count());
    return false;
  }

  bool expectHeld(wl_display* display, int drmFd, const Timeline& timeline, uint64_t point, std::string_view what) {
    const Wait wait = waitForPoint(display, drmFd, timeline.handle, point, kHoldTime);
    if (wait == Wait::TimedOut) {
      return true;
    }
    if (wait == Wait::Signalled) {
      std::println(stderr, "syncobj-client: {} was released while the compositor could still use it", what);
    }
    return false;
  }

  int runSuperseded(
      std::string_view mode, wl_display* display, State& state, int drmFd, gbm_device* gbm, wl_surface* parent
  ) {
    wl_buffer* a = createBuffer(state, gbm);
    wl_buffer* b = createBuffer(state, gbm);
    wl_buffer* c = createBuffer(state, gbm);
    Timeline acquire;
    Timeline shown;
    Timeline first;
    Timeline middle;
    Timeline last;
    if (a == nullptr
        || b == nullptr
        || c == nullptr
        || !createTimeline(state, drmFd, acquire)
        || !createTimeline(state, drmFd, shown)
        || !createTimeline(state, drmFd, first)
        || !createTimeline(state, drmFd, middle)
        || !createTimeline(state, drmFd, last)) {
      std::println(stderr, "syncobj-client: cannot create the subsurface buffers and timelines");
      return 1;
    }

    wl_surface* child = wl_compositor_create_surface(state.compositor);
    wp_linux_drm_syncobj_surface_v1* childSync = wp_linux_drm_syncobj_manager_v1_get_surface(state.syncobj, child);
    wl_subcompositor_get_subsurface(state.subcompositor, child, parent);
    uint64_t acquirePoint = 0;
    // Commits a buffer to the child at the next acquire point, which is signalled first unless the fence should only
    // arrive later.
    auto commit = [&](wl_buffer* buffer, const Timeline& release, uint64_t releasePoint, bool signalAcquire) {
      ++acquirePoint;
      uint64_t signalled = acquirePoint;
      if (signalAcquire && drmSyncobjTimelineSignal(drmFd, &acquire.handle, &signalled, 1) != 0) {
        return false;
      }
      wp_linux_drm_syncobj_surface_v1_set_acquire_point(childSync, acquire.object, 0, acquirePoint);
      wp_linux_drm_syncobj_surface_v1_set_release_point(childSync, release.object, 0, releasePoint);
      wl_surface_attach(child, buffer, 0, 0);
      wl_surface_damage_buffer(child, 0, 0, kSize, kSize);
      wl_surface_commit(child);
      return true;
    };

    if (mode == "superseded-displayed" || mode == "superseded-shared-timeline") {
      // Show A first: one toplevel commit applies the child's cached state.
      state.frameDone = false;
      wl_callback_add_listener(wl_surface_frame(child), &kFrameListener, &state);
      if (!commit(a, shown, 1, true)) {
        return 1;
      }
      wl_surface_commit(parent);
      if (!dispatchUntil(display, state.frameDone)) {
        return 1;
      }
    }

    // From here on the toplevel never commits again, so every child commit stays cached.
    bool ok = true;
    if (mode == "superseded") {
      ok = commit(a, first, 1, true)
          && commit(b, last, 1, true)
          && expectReleased(display, drmFd, first, 1, "the replaced cached buffer")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    } else if (mode == "superseded-displayed") {
      ok = commit(a, first, 1, true)
          && commit(b, last, 1, true)
          && expectHeld(display, drmFd, first, 1, "the cached commit of the buffer on screen")
          && expectHeld(display, drmFd, shown, 1, "the buffer on screen")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    } else if (mode == "superseded-shared-timeline") {
      ok = commit(c, shown, 2, true)
          && commit(b, last, 1, true)
          && expectHeld(display, drmFd, shown, 1, "the buffer on screen, sharing a timeline with a replaced commit")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    } else if (mode == "superseded-reattached") {
      // A is reattached unmodified, so the first A commit is replaced before it can be shown on its own.
      ok = commit(a, first, 1, true)
          && commit(a, first, 2, true)
          && commit(b, last, 1, true)
          && expectReleased(display, drmFd, first, 1, "the first commit of a reattached buffer")
          && expectReleased(display, drmFd, first, 2, "the second commit of a reattached buffer")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    } else if (mode == "superseded-interleaved") {
      ok = commit(a, first, 1, true)
          && commit(b, middle, 1, true)
          && commit(a, first, 2, true)
          && commit(c, last, 1, true)
          && expectReleased(display, drmFd, first, 1, "the first commit of A")
          && expectReleased(display, drmFd, middle, 1, "the replaced commit of B")
          && expectReleased(display, drmFd, first, 2, "the second commit of A")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    } else {
      // B's acquire fence does not exist yet, so wlroots locks B and A could still be shown on its own.
      ok = commit(a, first, 1, true)
          && commit(b, last, 1, false)
          && expectHeld(display, drmFd, first, 1, "the cached buffer before a locked replacement");
      uint64_t signalled = acquirePoint;
      ok = ok
          && drmSyncobjTimelineSignal(drmFd, &acquire.handle, &signalled, 1) == 0
          && expectReleased(display, drmFd, first, 1, "the replaced cached buffer once the replacement unlocked")
          && expectHeld(display, drmFd, last, 1, "the final cached buffer");
    }
    if (!ok) {
      return 1;
    }
    std::println("ok");
    return 0;
  }

} // namespace

int main(int argc, char** argv) {
  const std::string_view mode = argc == 2 ? argv[1] : "";
  const bool superseded = mode == "superseded"
      || mode == "superseded-displayed"
      || mode == "superseded-shared-timeline"
      || mode == "superseded-locked"
      || mode == "superseded-reattached"
      || mode == "superseded-interleaved";
  if (mode != "role-destroy" && mode != "unmap" && !superseded) {
    std::println(
        stderr,
        "usage: syncobj-client "
        "role-destroy|unmap|superseded[-displayed|-shared-timeline|-locked|-reattached|-interleaved]"
    );
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

  if (superseded) {
    return runSuperseded(mode, display, state, drmFd, gbm, surface);
  }

  if (mode == "role-destroy") {
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdgSurface);
  }
  const int releaseFd = eventfd(0, EFD_CLOEXEC);
  if (releaseFd < 0 || drmSyncobjEventfd(drmFd, release, 1, releaseFd, 0) != 0) {
    std::println(stderr, "syncobj-client: cannot watch the release point: {}", std::strerror(errno));
    return 1;
  }
  wl_surface_attach(surface, nullptr, 0, 0);
  wl_surface_commit(surface);

  const auto deadline = std::chrono::steady_clock::now() + kReleaseTimeout;
  while (true) {
    wl_display_dispatch_pending(display);
    wl_display_flush(display);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
      std::println(
          stderr, "syncobj-client: release point still pending {}s after the NULL commit", kReleaseTimeout.count()
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
