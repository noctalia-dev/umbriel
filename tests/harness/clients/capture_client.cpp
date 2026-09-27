// Captures one output through ext-image-copy-capture and prints one line per delivered frame. Without --cursor it is
// the pixel-only consumer; with --cursor it also creates a cursor session, which is the consumer whose cursor metadata
// only reaches a recording through a delivered main frame. The metadata payloads themselves are not asserted here: a
// headless backend has no DRM plane, so its cursor source always reports "no cursor". That is the running-session
// matrix, and only the cursor session's existence is what the compositor's gate looks at.

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct State {
    wl_display* display = nullptr;
    wl_shm* shm = nullptr;
    wl_output* output = nullptr;
    wl_pointer* pointer = nullptr;
    ext_output_image_capture_source_manager_v1* sourceManager = nullptr;
    ext_image_copy_capture_manager_v1* copyManager = nullptr;
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
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
  } state;

  void printLine(std::string_view text) {
    std::println("{}", text);
    std::fflush(stdout);
  }

  void outputName(void*, wl_output* output, const char* name) {
    if (state.output == nullptr && (state.wantedOutput.empty() || state.wantedOutput == name)) {
      state.output = output;
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
    if (state.wantCursor && state.pointer == nullptr && (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
      state.pointer = wl_seat_get_pointer(seat);
    }
  }
  void seatName(void*, wl_seat*, const char*) {}
  constexpr wl_seat_listener kSeatListener = {.capabilities = seatCapabilities, .name = seatName};

  void requestFrame();

  void frameTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
  void frameDamage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) { state.damage++; }
  void framePresentationTime(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
  void frameReady(void*, ext_image_copy_capture_frame_v1* frame) {
    state.frames++;
    std::println("frame {} damage {}", state.frames, state.damage);
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    state.frame = nullptr;
    requestFrame();
  }
  void frameFailed(void*, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
    std::println("failed {}", reason);
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    state.frame = nullptr;
    requestFrame();
  }
  constexpr ext_image_copy_capture_frame_v1_listener kFrameListener = {
      .transform = frameTransform,
      .damage = frameDamage,
      .presentation_time = framePresentationTime,
      .ready = frameReady,
      .failed = frameFailed,
  };

  void createBuffer() {
    if (state.buffer != nullptr || state.shm == nullptr || state.width == 0 || state.height == 0) {
      return;
    }
    const int stride = static_cast<int>(state.width) * 4;
    const std::size_t size = static_cast<std::size_t>(stride) * state.height;
    const int fd = memfd_create("capture-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
      printLine("buffer-failed");
      std::exit(1);
    }
    mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    state.buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int>(state.width), static_cast<int>(state.height), stride,
        state.format != 0 ? state.format : static_cast<std::uint32_t>(WL_SHM_FORMAT_ARGB8888)
    );
    wl_shm_pool_destroy(pool);
    close(fd);
  }

  void requestFrame() {
    if (state.session == nullptr || state.buffer == nullptr || state.frame != nullptr) {
      return;
    }
    state.damage = 0;
    state.frame = ext_image_copy_capture_session_v1_create_frame(state.session);
    ext_image_copy_capture_frame_v1_add_listener(state.frame, &kFrameListener, nullptr);
    ext_image_copy_capture_frame_v1_attach_buffer(state.frame, state.buffer);
    ext_image_copy_capture_frame_v1_capture(state.frame);
  }

  void sessionBufferSize(void*, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
    state.width = width;
    state.height = height;
  }
  void sessionShmFormat(void*, ext_image_copy_capture_session_v1*, uint32_t format) {
    if (state.format == 0) {
      state.format = format;
    }
  }

  void sessionDmabufDevice(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
  void sessionDmabufFormat(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
  void sessionDone(void*, ext_image_copy_capture_session_v1*) {
    if (!state.announced) {
      state.announced = true;
      printLine("session-ready");
    }
    createBuffer();
    requestFrame();
  }
  void sessionStopped(void*, ext_image_copy_capture_session_v1*) { printLine("session-stopped"); }
  constexpr ext_image_copy_capture_session_v1_listener kSessionListener = {
      .buffer_size = sessionBufferSize,
      .shm_format = sessionShmFormat,
      .dmabuf_device = sessionDmabufDevice,
      .dmabuf_format = sessionDmabufFormat,
      .done = sessionDone,
      .stopped = sessionStopped,
  };

  void startCapture() {
    if (state.started
        || state.output == nullptr
        || state.shm == nullptr
        || state.sourceManager == nullptr
        || state.copyManager == nullptr
        || (state.wantCursor && state.pointer == nullptr)) {
      return;
    }
    state.started = true;
    state.source = ext_output_image_capture_source_manager_v1_create_source(state.sourceManager, state.output);
    state.session = ext_image_copy_capture_manager_v1_create_session(state.copyManager, state.source, 0);
    ext_image_copy_capture_session_v1_add_listener(state.session, &kSessionListener, nullptr);
    if (state.wantCursor) {
      ext_image_copy_capture_manager_v1_create_pointer_cursor_session(state.copyManager, state.source, state.pointer);
    }
    printLine("started");
  }
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
    }
  }
  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryGlobalRemove};
} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--cursor") {
      state.wantCursor = true;
    } else if (arg == "--output" && i + 1 < argc) {
      state.wantedOutput = argv[++i];
    } else {
      printLine("usage: capture-client [--cursor] [--output NAME]");
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
  for (int i = 0; i < 4 && !state.started; ++i) {
    wl_display_roundtrip(state.display);
    startCapture();
  }
  if (!state.started) {
    printLine("start-failed");
    return 1;
  }
  while (wl_display_dispatch(state.display) != -1) {
  }
  return 1;
}
