#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
  struct Toplevel {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string identifier;
    bool closed = false;
  };

  struct State {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* sources = nullptr;
    ext_image_copy_capture_manager_v1* manager = nullptr;
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_buffer* buffer = nullptr;
    void* memory = MAP_FAILED;
    size_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = UINT32_MAX;
    unsigned wantedFrames = 0;
    unsigned captured = 0;
    bool constraintsReady = false;
    bool failed = false;
    std::vector<std::unique_ptr<Toplevel>> toplevels;

    ~State() {
      if (frame)
        ext_image_copy_capture_frame_v1_destroy(frame);
      if (session)
        ext_image_copy_capture_session_v1_destroy(session);
      if (source)
        ext_image_capture_source_v1_destroy(source);
      if (buffer)
        wl_buffer_destroy(buffer);
      if (memory != MAP_FAILED)
        munmap(memory, bytes);
      for (const auto& toplevel : toplevels)
        ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
      if (manager)
        ext_image_copy_capture_manager_v1_destroy(manager);
      if (sources)
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(sources);
      if (list)
        ext_foreign_toplevel_list_v1_destroy(list);
      if (shm)
        wl_shm_destroy(shm);
      if (registry)
        wl_registry_destroy(registry);
      if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
      }
    }
  };

  void topClosed(void* data, ext_foreign_toplevel_handle_v1*) { static_cast<Toplevel*>(data)->closed = true; }
  void topDone(void*, ext_foreign_toplevel_handle_v1*) {}
  void topTitle(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
  void topAppId(void*, ext_foreign_toplevel_handle_v1*, const char*) {}
  void topIdentifier(void* data, ext_foreign_toplevel_handle_v1*, const char* value) {
    static_cast<Toplevel*>(data)->identifier = value;
  }
  constexpr ext_foreign_toplevel_handle_v1_listener kTopListener{
      .closed = topClosed, .done = topDone, .title = topTitle, .app_id = topAppId, .identifier = topIdentifier
  };
  void listToplevel(void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
    auto& state = *static_cast<State*>(data);
    auto toplevel = std::make_unique<Toplevel>();
    toplevel->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kTopListener, toplevel.get());
    state.toplevels.push_back(std::move(toplevel));
  }
  void listFinished(void*, ext_foreign_toplevel_list_v1*) {}
  constexpr ext_foreign_toplevel_list_v1_listener kListListener{.toplevel = listToplevel, .finished = listFinished};

  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& state = *static_cast<State*>(data);
    const std::string_view type(interface);
    if (type == wl_shm_interface.name) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (type == ext_foreign_toplevel_list_v1_interface.name) {
      state.list = static_cast<ext_foreign_toplevel_list_v1*>(
          wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1)
      );
      ext_foreign_toplevel_list_v1_add_listener(state.list, &kListListener, &state);
    } else if (type == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
      state.sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
          wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1)
      );
    } else if (type == ext_image_copy_capture_manager_v1_interface.name) {
      state.manager = static_cast<ext_image_copy_capture_manager_v1*>(
          wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1)
      );
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener{.global = global, .global_remove = globalRemove};

  void bufferSize(void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
    auto& state = *static_cast<State*>(data);
    if (state.buffer && (state.width != width || state.height != height)) {
      state.failed = true; // Fixture geometry is fixed; don't read a resized old allocation.
    }
    state.width = width;
    state.height = height;
  }
  void shmFormat(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
    auto& state = *static_cast<State*>(data);
    if (format == WL_SHM_FORMAT_ARGB8888 || (format == WL_SHM_FORMAT_XRGB8888 && state.format == UINT32_MAX)) {
      state.format = format;
    }
  }
  void dmabufDevice(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
  void dmabufFormat(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
  void sessionDone(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<State*>(data)->constraintsReady = true;
  }
  void sessionStopped(void* data, ext_image_copy_capture_session_v1*) { static_cast<State*>(data)->failed = true; }
  constexpr ext_image_copy_capture_session_v1_listener kSessionListener{
      .buffer_size = bufferSize,
      .shm_format = shmFormat,
      .dmabuf_device = dmabufDevice,
      .dmabuf_format = dmabufFormat,
      .done = sessionDone,
      .stopped = sessionStopped
  };

  void requestFrame(State& state);
  void frameTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
  void frameDamage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
  void frameTime(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
  void frameReady(void* data, ext_image_copy_capture_frame_v1* frame) {
    auto& state = *static_cast<State*>(data);
    const auto* pixels = static_cast<const uint32_t*>(state.memory);
    const uint32_t pixel = pixels[(state.height / 2) * state.width + state.width / 2];
    ++state.captured;
    std::println(
        R"({{"frame":{},"r":{},"g":{},"b":{}}})", state.captured, (pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255
    );
    std::fflush(stdout);
    ext_image_copy_capture_frame_v1_destroy(frame);
    state.frame = nullptr;
    if (state.captured < state.wantedFrames)
      requestFrame(state);
  }
  void frameFailed(void* data, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
    auto& state = *static_cast<State*>(data);
    std::println(stderr, "capture failed: {}", reason);
    ext_image_copy_capture_frame_v1_destroy(frame);
    state.frame = nullptr;
    state.failed = true;
  }
  constexpr ext_image_copy_capture_frame_v1_listener kFrameListener{
      .transform = frameTransform,
      .damage = frameDamage,
      .presentation_time = frameTime,
      .ready = frameReady,
      .failed = frameFailed
  };
  void requestFrame(State& state) {
    state.frame = ext_image_copy_capture_session_v1_create_frame(state.session);
    ext_image_copy_capture_frame_v1_add_listener(state.frame, &kFrameListener, &state);
    ext_image_copy_capture_frame_v1_attach_buffer(state.frame, state.buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(
        state.frame, 0, 0, static_cast<int32_t>(state.width), static_cast<int32_t>(state.height)
    );
    ext_image_copy_capture_frame_v1_capture(state.frame);
  }

  bool createBuffer(State& state) {
    if (state.width == 0
        || state.height == 0
        || state.width > 8192
        || state.height > 8192
        || state.format == UINT32_MAX)
      return false;
    state.bytes = static_cast<size_t>(state.width) * state.height * 4;
    const int fd = memfd_create("umbriel-capture-fixture", MFD_CLOEXEC);
    if (fd < 0)
      return false;
    if (ftruncate(fd, static_cast<off_t>(state.bytes)) != 0) {
      close(fd);
      return false;
    }
    state.memory = mmap(nullptr, state.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state.memory == MAP_FAILED) {
      close(fd);
      return false;
    }
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int32_t>(state.bytes));
    state.buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int32_t>(state.width), static_cast<int32_t>(state.height),
        static_cast<int32_t>(state.width * 4), state.format
    );
    wl_shm_pool_destroy(pool);
    close(fd);
    return state.buffer != nullptr;
  }
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::println(stderr, "usage: toplevel-capture-client IDENTIFIER FRAMES");
    return 2;
  }
  State state;
  const std::string_view count(argv[2]);
  const auto parsed = std::from_chars(count.data(), count.data() + count.size(), state.wantedFrames);
  if (parsed.ec != std::errc{}
      || parsed.ptr != count.data() + count.size()
      || state.wantedFrames == 0
      || state.wantedFrames > 1000)
    return 2;
  state.display = wl_display_connect(nullptr);
  if (!state.display)
    return 2;
  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0
      || wl_display_roundtrip(state.display) < 0
      || !state.shm
      || !state.sources
      || !state.manager)
    return 2;
  const auto target = std::ranges::find_if(state.toplevels, [&](const auto& top) {
    return top->identifier == argv[1] && !top->closed;
  });
  if (target == state.toplevels.end())
    return 2;
  state.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(state.sources, (*target)->handle);
  state.session = ext_image_copy_capture_manager_v1_create_session(state.manager, state.source, 0);
  ext_image_copy_capture_session_v1_add_listener(state.session, &kSessionListener, &state);
  if (wl_display_roundtrip(state.display) < 0 || !state.constraintsReady || !createBuffer(state))
    return 2;
  std::println("{{\"session\":true}}");
  std::fflush(stdout);
  requestFrame(state);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!state.failed && state.captured < state.wantedFrames && std::chrono::steady_clock::now() < deadline) {
    while (wl_display_prepare_read(state.display) != 0) {
      if (wl_display_dispatch_pending(state.display) < 0)
        return 1;
    }
    wl_display_flush(state.display);
    pollfd channel{.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0};
    const int result = poll(&channel, 1, 50);
    if (result > 0 && (channel.revents & POLLIN) != 0) {
      if (wl_display_read_events(state.display) < 0)
        return 1;
    } else {
      wl_display_cancel_read(state.display);
      if (result < 0 || (channel.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
        return 1;
    }
    if (wl_display_dispatch_pending(state.display) < 0)
      return 1;
  }
  return !state.failed && state.captured == state.wantedFrames ? 0 : 1;
}
