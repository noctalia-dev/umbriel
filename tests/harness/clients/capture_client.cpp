// Captures outputs or a foreign toplevel through ext-image-copy-capture and prints every delivered frame and its size.
// One process can hold a cursor session on one output and a pixel-only session on another
// over a single connection, matching a portal process, so a same-client mixed capture can
// be regression-tested. Without --cursor the primary target is the pixel-only consumer; with --cursor it also creates
// a cursor session, the consumer whose cursor metadata only reaches a recording through a delivered main frame. The
// metadata payloads themselves are not asserted here: a headless backend has no DRM plane, so its cursor source
// always reports "no cursor". That is the running-session matrix; what the compositor's gate reads is the cursor
// session's attachment to a source, which is why both roles run in this one client.
// CAPTURE_CONTROL enables stdin sync LABEL commands, reporting each output's delivered frame count after a roundtrip.
// --window selects a toplevel identifier; buffer constraints changes recreate the shared-memory target.
// CAPTURE_PROBES="x y ..." logs the RGB value at each point on every delivered frame.
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
  // One capture role; a process runs one or two of these over its single wl_display connection.
  struct Target {
    std::string label;
    wl_output* output = nullptr;
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = MAP_FAILED;
    size_t pixelsSize = 0;
    std::string_view wantedOutput;
    std::string_view wantedWindow;
    ext_foreign_toplevel_handle_v1* toplevel = nullptr;
    bool wantCursor = false;
    bool started = false;
    // Separate from `started`: session's done event arrive after startCapture() set it.
    bool announced = false;
    bool stopped = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bufferWidth = 0;
    uint32_t bufferHeight = 0;
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    uint32_t format = 0;
    uint32_t frames = 0;
    uint32_t damage = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    wl_pointer* pointer = nullptr;
    ext_output_image_capture_source_manager_v1* sourceManager = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* windowSourceManager = nullptr;
    ext_image_copy_capture_manager_v1* copyManager = nullptr;
    Target primary;
    Target pixel;
    std::vector<std::array<uint32_t, 2>> probes;
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
    if (state.primary.wantedWindow.empty()
        && state.primary.output == nullptr
        && (state.primary.wantedOutput.empty() || state.primary.wantedOutput == name)) {
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
    target.frames++;
    std::println(
        "frame {} {} damage {} size {} {}", target.label, target.frames, target.damage, target.frameWidth,
        target.frameHeight
    );
    for (const auto& point : state.probes) {
      if (target.pixels == MAP_FAILED
          || point[0] >= target.frameWidth
          || point[1] >= target.frameHeight
          || target.frameWidth != target.bufferWidth
          || target.frameHeight != target.bufferHeight) {
        printTarget(target, "probe-outside-buffer");
        continue;
      }
      const auto* pixels = static_cast<const uint32_t*>(target.pixels);
      uint32_t rgb = pixels[point[1] * target.frameWidth + point[0]] & 0xFFFFFFU;
      switch (target.format) {
      case WL_SHM_FORMAT_ARGB8888:
      case WL_SHM_FORMAT_XRGB8888:
        break;
      case WL_SHM_FORMAT_ABGR8888:
      case WL_SHM_FORMAT_XBGR8888:
        rgb = ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
        break;
      default:
        printTarget(target, "probe-unsupported-format");
        continue;
      }
      std::println("probe {} frame={} x={} y={} rgb={:06x}", target.label, target.frames, point[0], point[1], rgb);
    }
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    target.frame = nullptr;
    requestFrame(target);
  }
  void frameFailed(void* data, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
    auto& target = *static_cast<Target*>(data);
    std::println("failed {} {}", target.label, reason);
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    target.frame = nullptr;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
      target.stopped = true;
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

  void createBuffer(Target& target) {
    if (state.shm == nullptr
        || target.width == 0
        || target.height == 0
        || (target.buffer != nullptr && target.bufferWidth == target.width && target.bufferHeight == target.height)) {
      return;
    }
    if (target.buffer != nullptr) {
      wl_buffer_destroy(target.buffer);
      if (target.pixels != MAP_FAILED) {
        munmap(target.pixels, target.pixelsSize);
        target.pixels = MAP_FAILED;
      }
    }
    const int stride = static_cast<int>(target.width) * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * target.height;
    const int fd = memfd_create("capture-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
      printTarget(target, "buffer-failed");
      std::exit(1);
    }
    if (!state.probes.empty()) {
      target.pixels = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
      target.pixelsSize = size;
      if (target.pixels == MAP_FAILED) {
        printTarget(target, "probe-map-failed");
        std::exit(1);
      }
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    target.buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int>(target.width), static_cast<int>(target.height), stride,
        target.format != 0 ? target.format : static_cast<std::uint32_t>(WL_SHM_FORMAT_ARGB8888)
    );
    wl_shm_pool_destroy(pool);
    close(fd);
    target.bufferWidth = target.width;
    target.bufferHeight = target.height;
  }

  void requestFrame(Target& target) {
    if (target.stopped || target.session == nullptr || target.buffer == nullptr || target.frame != nullptr) {
      return;
    }
    target.damage = 0;
    target.frameWidth = target.bufferWidth;
    target.frameHeight = target.bufferHeight;
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

  void sessionDmabufDevice(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
  void sessionDmabufFormat(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
  void sessionDone(void* data, ext_image_copy_capture_session_v1*) {
    auto& target = *static_cast<Target*>(data);
    std::println("constraints {} {} {}", target.label, target.width, target.height);
    std::fflush(stdout);
    if (!target.announced) {
      target.announced = true;
      printTarget(target, "session-ready");
    }
    createBuffer(target);
    requestFrame(target);
  }
  void sessionStopped(void* data, ext_image_copy_capture_session_v1*) {
    auto& target = *static_cast<Target*>(data);
    target.stopped = true;
    printTarget(target, "session-stopped");
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
        || state.shm == nullptr
        || state.copyManager == nullptr
        || (target.wantCursor && state.pointer == nullptr)) {
      return;
    }
    if (target.wantedWindow.empty()) {
      if (target.output == nullptr || state.sourceManager == nullptr) {
        return;
      }
      target.source = ext_output_image_capture_source_manager_v1_create_source(state.sourceManager, target.output);
    } else {
      if (target.toplevel == nullptr || state.windowSourceManager == nullptr) {
        return;
      }
      target.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
          state.windowSourceManager, target.toplevel
      );
    }
    target.started = true;
    target.session = ext_image_copy_capture_manager_v1_create_session(state.copyManager, target.source, 0);
    ext_image_copy_capture_session_v1_add_listener(target.session, &kSessionListener, &target);
    if (target.wantCursor) {
      ext_image_copy_capture_manager_v1_create_pointer_cursor_session(state.copyManager, target.source, state.pointer);
    }
    printTarget(target, "started");
  }

  void toplevelClosed(void*, ext_foreign_toplevel_handle_v1*) {}
  void toplevelDone(void*, ext_foreign_toplevel_handle_v1*) {}
  void toplevelTitle(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
  void toplevelAppId(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
  void toplevelIdentifier(void*, ext_foreign_toplevel_handle_v1* handle, const char* identifier) {
    if (state.primary.wantedWindow == identifier) {
      state.primary.toplevel = handle;
      state.primary.label = std::string("window:") + identifier;
    }
  }
  constexpr ext_foreign_toplevel_handle_v1_listener kToplevelListener = {
      .closed = toplevelClosed,
      .done = toplevelDone,
      .title = toplevelTitle,
      .app_id = toplevelAppId,
      .identifier = toplevelIdentifier,
  };
  void listToplevel(void*, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kToplevelListener, nullptr);
  }
  void listFinished(void*, ext_foreign_toplevel_list_v1*) {}
  constexpr ext_foreign_toplevel_list_v1_listener kToplevelListListener = {
      .toplevel = listToplevel,
      .finished = listFinished,
  };
  void registryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const std::string_view iface(interface);
    if (iface == wl_shm_interface.name) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
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
    } else if (iface == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
      state.windowSourceManager = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
          wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1)
      );
    } else if (iface == ext_foreign_toplevel_list_v1_interface.name) {
      auto* list = static_cast<ext_foreign_toplevel_list_v1*>(
          wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1)
      );
      ext_foreign_toplevel_list_v1_add_listener(list, &kToplevelListListener, nullptr);
    }
  }
  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryGlobalRemove};

  bool readCommands(std::string& commands) {
    char buffer[1024];
    const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) {
      return count < 0 && (errno == EINTR || errno == EAGAIN);
    }
    commands.append(buffer, static_cast<size_t>(count));
    size_t newline = 0;
    while ((newline = commands.find('\n')) != std::string::npos) {
      std::istringstream command(commands.substr(0, newline));
      commands.erase(0, newline + 1);
      std::string action;
      std::string label;
      command >> action >> label;
      if (action != "sync" || label.empty() || wl_display_roundtrip(state.display) < 0) {
        printLine("sync-failed");
        return false;
      }
      std::println("synced {} {} {}", label, state.primary.label, state.primary.frames);
      if (pixelWanted()) {
        std::println("synced {} {} {}", label, state.pixel.label, state.pixel.frames);
      }
      std::fflush(stdout);
    }
    return true;
  }
} // namespace

int main(int argc, char** argv) {
  if (const char* probes = std::getenv("CAPTURE_PROBES")) {
    std::istringstream input(probes);
    int x = 0, y = 0;
    while (input >> x) {
      if (!(input >> y) || x < 0 || y < 0) {
        printLine("invalid CAPTURE_PROBES: expected nonnegative x y pairs");
        return 2;
      }
      state.probes.push_back({static_cast<uint32_t>(x), static_cast<uint32_t>(y)});
    }
    if (!input.eof() || state.probes.empty()) {
      printLine("invalid CAPTURE_PROBES: expected nonnegative x y pairs");
      return 2;
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--cursor") {
      state.primary.wantCursor = true;
    } else if (arg == "--output" && i + 1 < argc) {
      state.primary.wantedOutput = argv[++i];
    } else if (arg == "--pixel-output" && i + 1 < argc) {
      state.pixel.wantedOutput = argv[++i];
    } else if (arg == "--window" && i + 1 < argc) {
      state.primary.wantedWindow = argv[++i];
    } else {
      printLine("usage: capture-client [--cursor] [--output NAME | --window ID] [--pixel-output NAME]");
      return 2;
    }
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    printLine("connect-failed");
    return 1;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, nullptr);
  for (int i = 0; i < 4 && (!state.primary.started || (pixelWanted() && !state.pixel.started)); ++i) {
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
  const bool control = std::getenv("CAPTURE_CONTROL") != nullptr;
  std::string commands;
  while (wl_display_dispatch_pending(state.display) >= 0) {
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      break;
    }
    pollfd sources[] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = control ? STDIN_FILENO : -1, .events = POLLIN, .revents = 0},
    };
    if (poll(sources, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((sources[0].revents & (POLLERR | POLLHUP)) != 0
        || ((sources[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0)) {
      break;
    }
    if ((sources[1].revents & (POLLIN | POLLHUP)) != 0 && !readCommands(commands)) {
      break;
    }
  }
  return 1;
}
