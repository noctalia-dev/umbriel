// Captures outputs through ext-image-copy-capture and prints one line per delivered frame, prefixing every line with
// the output it belongs to. One process can hold a cursor session on one output and a pixel-only session on another
// over a single connection — the shape a portal process has — which is the only way a same-client mixed capture can
// be regression-tested. Without --cursor the primary target is the pixel-only consumer; with --cursor it also creates
// a cursor session, the consumer whose cursor metadata only reaches a recording through a delivered main frame. The
// metadata payloads themselves are not asserted here: a headless backend has no DRM plane, so its cursor source
// always reports "no cursor". That is the running-session matrix; what the compositor's gate reads is the cursor
// session's attachment to a source, which is why both roles run in this one client.
// --dmabuf negotiates a GBM buffer on the advertised capture device. It waits for
// implicit write fences before reporting a frame, without adding pixel readback.
// --timings separates request-to-ready latency from the subsequent fence wait.
// --manual accepts "capture primary", "capture pixel", and "destroy <role>" on
// stdin, reporting the SHM center pixel after each completed request. Together
// with --shared-source it exercises sparse requests and shared-source teardown.
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <xf86drm.h>

namespace {
  // One capture role; a process runs one or two of these over its single wl_display connection.
  struct Target {
    std::string label;
    wl_output* output = nullptr;
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    std::string_view wantedOutput;
    bool wantCursor = false;
    bool started = false;
    // Separate from `started`: session's done event arrive after startCapture() set it.
    bool announced = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t frames = 0;
    uint32_t damage = 0;
    dev_t device = 0;
    struct DmabufFormat {
      uint32_t format;
      std::vector<uint64_t> modifiers;
    };
    std::vector<DmabufFormat> dmabufFormats;
    gbm_device* gbm = nullptr;
    gbm_bo* bo = nullptr;
    std::vector<int> planeFds;
    std::chrono::steady_clock::time_point requestedAt;
  };

  struct State {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    bool useDmabuf = false;
    bool reportTimings = false;
    bool manual = false;
    bool sharedSource = false;
    wl_pointer* pointer = nullptr;
    ext_output_image_capture_source_manager_v1* sourceManager = nullptr;
    ext_image_copy_capture_manager_v1* copyManager = nullptr;
    Target primary;
    Target pixel;
  } state;

  void printLine(std::string_view text) {
    std::println("{}", text);
    std::fflush(stdout);
  }

  void printTarget(const Target& target, std::string_view text) {
    std::println("{} {}", text, target.label);
    std::fflush(stdout);
  }

  bool pixelWanted() { return !state.pixel.wantedOutput.empty(); }

  void outputName(void*, wl_output* output, const char* name) {
    const std::string_view entry(name);
    if (state.primary.output == nullptr && (state.primary.wantedOutput.empty() || state.primary.wantedOutput == name)) {
      state.primary.output = output;
      state.primary.label = name;
    }
    if (state.pixel.output == nullptr && !state.pixel.wantedOutput.empty() && state.pixel.wantedOutput == entry) {
      state.pixel.output = output;
      state.pixel.label = name;
    }
  }

  void
  outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
  void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  void outputDone(void*, wl_output*) {}
  void outputScale(void*, wl_output*, int32_t) {}
  void outputDescription(void*, wl_output*, const char*) {}
  constexpr wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  void seatCapabilities(void*, wl_seat* seat, uint32_t capabilities) {
    if (state.primary.wantCursor && state.pointer == nullptr && (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
      state.pointer = wl_seat_get_pointer(seat);
    }
  }
  void seatName(void*, wl_seat*, const char*) {}
  constexpr wl_seat_listener kSeatListener = {.capabilities = seatCapabilities, .name = seatName};

  void requestFrame(Target& target);

  void frameTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
  void frameDamage(void* data, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {
    static_cast<Target*>(data)->damage++;
  }
  void framePresentationTime(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
  void frameReady(void* data, ext_image_copy_capture_frame_v1* frame) {
    auto& target = *static_cast<Target*>(data);
    const auto readyAt =
        state.reportTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // A DMA-BUF ready event may carry an unsignalled implicit write fence.
    // Count a completed capture only after the consumer could read every plane.
    for (int fd : target.planeFds) {
      pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
      int ready;
      do {
        ready = poll(&descriptor, 1, 10000);
      } while (ready < 0 && errno == EINTR);
      if (ready != 1 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0 || (descriptor.revents & POLLIN) == 0) {
        printTarget(target, "dmabuf-fence-failed");
        std::exit(1);
      }
    }
    const auto completedAt =
        state.reportTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    target.frames++;
    if (state.reportTimings) {
      const auto readyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(readyAt - target.requestedAt).count();
      const auto fenceNs = std::chrono::duration_cast<std::chrono::nanoseconds>(completedAt - readyAt).count();
      std::println(
          "timing {} {} request_to_ready_ns {} fence_wait_ns {}", target.label, target.frames, readyNs, fenceNs
      );
    }
    std::println("frame {} {} damage {}", target.label, target.frames, target.damage);
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    target.frame = nullptr;
    if (!state.manual) {
      requestFrame(target);
    }
  }
  void frameFailed(void* data, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
    auto& target = *static_cast<Target*>(data);
    std::println("failed {} {}", target.label, reason);
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    target.frame = nullptr;
    if (state.manual) {
      std::exit(1);
    }
    requestFrame(target);
  }
  constexpr ext_image_copy_capture_frame_v1_listener kFrameListener = {
      .transform = frameTransform,
      .damage = frameDamage,
      .presentation_time = framePresentationTime,
      .ready = frameReady,
      .failed = frameFailed,
  };

  void createDmabuf(Target& target) {
    drmDevice* device = nullptr;
    if (drmGetDeviceFromDevId(target.device, 0, &device) != 0) {
      printTarget(target, "dmabuf-device-failed");
      std::exit(1);
    }
    const int fd = (device->available_nodes & (1 << DRM_NODE_RENDER)) != 0
        ? open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC)
        : -1;
    drmFreeDevice(&device);
    target.gbm = fd >= 0 ? gbm_create_device(fd) : nullptr;
    if (target.gbm == nullptr) {
      printTarget(target, "dmabuf-gbm-failed");
      std::exit(1);
    }
    uint32_t format = 0;
    uint64_t modifier = 0;
    for (const auto& advertised : target.dmabufFormats) {
      if (advertised.format != GBM_FORMAT_XRGB8888 && advertised.format != GBM_FORMAT_ARGB8888) {
        continue;
      }
      for (uint64_t candidate : advertised.modifiers) {
        target.bo = candidate == DRM_FORMAT_MOD_INVALID
            ? gbm_bo_create(target.gbm, target.width, target.height, advertised.format, GBM_BO_USE_RENDERING)
            : gbm_bo_create_with_modifiers2(
                  target.gbm, target.width, target.height, advertised.format, &candidate, 1, GBM_BO_USE_RENDERING
              );
        if (target.bo != nullptr) {
          format = advertised.format;
          modifier = candidate;
          break;
        }
      }
      if (target.bo != nullptr) {
        break;
      }
    }
    if (target.bo == nullptr) {
      printTarget(target, "dmabuf-allocation-failed");
      std::exit(1);
    }
    auto* params = zwp_linux_dmabuf_v1_create_params(state.dmabuf);
    const int planes = gbm_bo_get_plane_count(target.bo);
    for (int plane = 0; plane < planes; ++plane) {
      const int planeFd = gbm_bo_get_fd_for_plane(target.bo, plane);
      if (planeFd < 0) {
        printTarget(target, "dmabuf-export-failed");
        std::exit(1);
      }
      zwp_linux_buffer_params_v1_add(
          params, planeFd, plane, gbm_bo_get_offset(target.bo, plane), gbm_bo_get_stride_for_plane(target.bo, plane),
          modifier >> 32, modifier & 0xffffffffU
      );
      target.planeFds.push_back(planeFd);
    }
    target.buffer = zwp_linux_buffer_params_v1_create_immed(params, target.width, target.height, format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    std::println("buffer-dmabuf {} format {} modifier {} planes {}", target.label, format, modifier, planes);
    std::fflush(stdout);
  }

  void createBuffer(Target& target) {
    if (target.buffer != nullptr || target.width == 0 || target.height == 0) {
      return;
    }
    if (state.useDmabuf) {
      createDmabuf(target);
      return;
    }
    const int stride = static_cast<int>(target.width) * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * target.height;
    const int fd = memfd_create("capture-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
      printTarget(target, "buffer-failed");
      std::exit(1);
    }
    target.pixels = static_cast<uint32_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (target.pixels == MAP_FAILED) {
      printTarget(target, "map-failed");
      std::exit(1);
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    target.buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int>(target.width), static_cast<int>(target.height), stride,
        target.format != 0 ? target.format : static_cast<std::uint32_t>(WL_SHM_FORMAT_ARGB8888)
    );
    wl_shm_pool_destroy(pool);
    close(fd);
  }

  void requestFrame(Target& target) {
    if (target.session == nullptr || target.buffer == nullptr || target.frame != nullptr) {
      return;
    }
    if (state.reportTimings) {
      target.requestedAt = std::chrono::steady_clock::now();
    }
    target.damage = 0;
    target.frame = ext_image_copy_capture_session_v1_create_frame(target.session);
    ext_image_copy_capture_frame_v1_add_listener(target.frame, &kFrameListener, &target);
    ext_image_copy_capture_frame_v1_attach_buffer(target.frame, target.buffer);
    ext_image_copy_capture_frame_v1_capture(target.frame);
  }

  void sessionBufferSize(void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
    auto& target = *static_cast<Target*>(data);
    target.width = width;
    target.height = height;
  }
  void sessionShmFormat(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
    auto& target = *static_cast<Target*>(data);
    if (target.format == 0) {
      target.format = format;
    }
  }

  void sessionDmabufDevice(void* data, ext_image_copy_capture_session_v1*, wl_array* device) {
    auto& target = *static_cast<Target*>(data);
    if (device->size == sizeof(target.device)) {
      std::memcpy(&target.device, device->data, device->size);
    }
  }
  void sessionDmabufFormat(void* data, ext_image_copy_capture_session_v1*, uint32_t format, wl_array* modifiers) {
    Target::DmabufFormat entry{.format = format, .modifiers = {}};
    if (modifiers->size % sizeof(uint64_t) != 0) {
      printLine("invalid-dmabuf-modifiers");
      std::exit(1);
    }
    entry.modifiers.resize(modifiers->size / sizeof(uint64_t));
    if (modifiers->size != 0) {
      std::memcpy(entry.modifiers.data(), modifiers->data, modifiers->size);
    }
    static_cast<Target*>(data)->dmabufFormats.push_back(std::move(entry));
  }
  void sessionDone(void* data, ext_image_copy_capture_session_v1*) {
    auto& target = *static_cast<Target*>(data);
    if (!target.announced) {
      target.announced = true;
      printTarget(target, "session-ready");
    }
    createBuffer(target);
    if (!state.manual) {
      requestFrame(target);
    }
  }
  void sessionStopped(void* data, ext_image_copy_capture_session_v1*) {
    printTarget(*static_cast<Target*>(data), "session-stopped");
  }
  constexpr ext_image_copy_capture_session_v1_listener kSessionListener = {
      .buffer_size = sessionBufferSize,
      .shm_format = sessionShmFormat,
      .dmabuf_device = sessionDmabufDevice,
      .dmabuf_format = sessionDmabufFormat,
      .done = sessionDone,
      .stopped = sessionStopped,
  };

  void startCapture(Target& target) {
    if (target.started
        || target.output == nullptr
        || (state.useDmabuf ? state.dmabuf == nullptr : state.shm == nullptr)
        || state.sourceManager == nullptr
        || state.copyManager == nullptr
        || (target.wantCursor && state.pointer == nullptr)) {
      return;
    }
    target.started = true;
    target.source = state.sharedSource && &target == &state.pixel
        ? state.primary.source
        : ext_output_image_capture_source_manager_v1_create_source(state.sourceManager, target.output);
    target.session = ext_image_copy_capture_manager_v1_create_session(state.copyManager, target.source, 0);
    ext_image_copy_capture_session_v1_add_listener(target.session, &kSessionListener, &target);
    if (target.wantCursor) {
      ext_image_copy_capture_manager_v1_create_pointer_cursor_session(state.copyManager, target.source, state.pointer);
    }
    printTarget(target, "started");
  }
  void registryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const std::string_view iface(interface);
    if (iface == wl_shm_interface.name) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (iface == zwp_linux_dmabuf_v1_interface.name && version >= 3) {
      state.dmabuf =
          static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
    } else if (iface == wl_seat_interface.name) {
      auto* seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
      wl_seat_add_listener(seat, &kSeatListener, nullptr);
    } else if (iface == wl_output_interface.name) {
      // v4 for the name event, so a two-output check can pick its own output.
      auto* output =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
      wl_output_add_listener(output, &kOutputListener, nullptr);
    } else if (iface == ext_output_image_capture_source_manager_v1_interface.name) {
      state.sourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(
          wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1)
      );
    } else if (iface == ext_image_copy_capture_manager_v1_interface.name) {
      state.copyManager = static_cast<ext_image_copy_capture_manager_v1*>(
          wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1)
      );
    }
  }
  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryGlobalRemove};
} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--dmabuf") {
      state.useDmabuf = true;
    } else if (arg == "--manual") {
      state.manual = true;
    } else if (arg == "--shared-source") {
      state.sharedSource = true;
    } else if (arg == "--timings") {
      state.reportTimings = true;
    } else if (arg == "--cursor") {
      state.primary.wantCursor = true;
    } else if (arg == "--output" && i + 1 < argc) {
      state.primary.wantedOutput = argv[++i];
    } else if (arg == "--pixel-output" && i + 1 < argc) {
      state.pixel.wantedOutput = argv[++i];
    } else {
      printLine(
          "usage: capture-client [--dmabuf] [--timings] [--manual] [--shared-source] [--cursor] [--output NAME] "
          "[--pixel-output NAME]"
      );
      return 2;
    }
  }
  if ((state.manual && state.useDmabuf)
      || (state.sharedSource && (!pixelWanted() || state.pixel.wantedOutput != state.primary.wantedOutput))) {
    printLine("manual requires SHM; shared-source requires identical explicit outputs");
    return 2;
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    printLine("connect-failed");
    return 1;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, nullptr);
  for (int i = 0; i < 4 && !(state.primary.started && (!pixelWanted() || state.pixel.started)); ++i) {
    wl_display_roundtrip(state.display);
    startCapture(state.primary);
    if (pixelWanted()) {
      startCapture(state.pixel);
    }
  }
  if (!state.primary.started || (pixelWanted() && !state.pixel.started)) {
    printLine("start-failed");
    return 1;
  }
  if (state.manual) {
    // The harness drives sparse requests through a FIFO. Roundtrips establish
    // protocol boundaries; no wall-clock sleeps decide when pixels are ready.
    if (wl_display_roundtrip(state.display) < 0) {
      return 1;
    }
    printLine("manual-ready");
    std::string action, role;
    while (std::cin >> action >> role) {
      Target* target = role == "primary" ? &state.primary : role == "pixel" ? &state.pixel : nullptr;
      if (target == nullptr || target->session == nullptr) {
        return 2;
      }
      if (action == "capture") {
        if (target->pixels == nullptr
            || (target->format != WL_SHM_FORMAT_ARGB8888 && target->format != WL_SHM_FORMAT_XRGB8888)) {
          return 2;
        }
        requestFrame(*target);
        while (target->frame != nullptr) {
          if (wl_display_dispatch(state.display) < 0) {
            return 1;
          }
        }
        const auto pixel = target->pixels[(target->height / 2) * target->width + target->width / 2];
        std::println("pixel {} {} {} {}", role, (pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255);
      } else if (action == "destroy") {
        ext_image_copy_capture_session_v1_destroy(target->session);
        target->session = nullptr;
      } else {
        return 2;
      }
      if (wl_display_roundtrip(state.display) < 0) {
        return 1;
      }
      std::println("manual-done {} {}", action, role);
      std::fflush(stdout);
    }
    wl_display_disconnect(state.display);
    return 0;
  }
  while (wl_display_dispatch(state.display) != -1) {
  }
  return 1;
}
