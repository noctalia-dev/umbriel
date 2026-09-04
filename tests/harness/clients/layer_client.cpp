// Maps a top exclusive zone, a full-output background layer when the height is zero, or a 200x200 bottom-layer
// square. It stays mapped until that output closes the layer surface.

#include <wayland-client.h>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {
  struct Output {
    wl_output* resource = nullptr;
    std::string name;
  };

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
    int width = 0;
    int height = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wl_callback* frame = nullptr;
    Buffer buffer;
    bool ready = false;
    bool closed = false;
    bool failed = false;
    uint32_t fillColor = 0xFF202020;
  };

  void
  outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
  void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  void outputDone(void*, wl_output*) {}
  void outputScale(void*, wl_output*, int32_t) {}
  void outputName(void* data, wl_output*, const char* name) {
    static_cast<Output*>(data)->name = name != nullptr ? name : "";
  }
  void outputDescription(void*, wl_output*, const char*) {}

  constexpr wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  Buffer createBuffer(State& state, int width, int height) {
    Buffer buffer{.width = width, .height = height};
    const int stride = width * 4;
    buffer.size = static_cast<size_t>(stride * height);
    const int fd = memfd_create("umbriel-layer-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(buffer.size)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return buffer;
    }
    buffer.pixels = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer.pixels == MAP_FAILED) {
      close(fd);
      return buffer;
    }
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), state.fillColor);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
    buffer = {};
  }

  void frameDone(void* data, wl_callback* callback, uint32_t) {
    auto& state = *static_cast<State*>(data);
    wl_callback_destroy(callback);
    state.frame = nullptr;
    if (!state.ready) {
      state.ready = true;
      std::println("ready");
      std::fflush(stdout);
    }
  }

  constexpr wl_callback_listener kFrameListener = {.done = frameDone};

  void
  layerConfigure(void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t width, uint32_t height) {
    auto& state = *static_cast<State*>(data);
    zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
    const int configuredWidth = std::max(1, static_cast<int>(width));
    const int configuredHeight = std::max(1, static_cast<int>(height));
    if (state.buffer.resource == nullptr
        || state.buffer.width != configuredWidth
        || state.buffer.height != configuredHeight) {
      destroyBuffer(state.buffer);
      state.buffer = createBuffer(state, configuredWidth, configuredHeight);
    }
    if (state.buffer.resource == nullptr) {
      state.failed = true;
      state.closed = true;
      return;
    }
    wl_surface_attach(state.surface, state.buffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, configuredWidth, configuredHeight);
    if (!state.ready && state.frame == nullptr) {
      state.frame = wl_surface_frame(state.surface);
      wl_callback_add_listener(state.frame, &kFrameListener, &state);
    }
    wl_surface_commit(state.surface);
  }

  void layerClosed(void* data, zwlr_layer_surface_v1*) { static_cast<State*>(data)->closed = true; }

  constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = layerConfigure,
      .closed = layerClosed,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
      state.layerShell = static_cast<zwlr_layer_shell_v1*>(
          wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4U))
      );
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
      auto output = std::make_unique<Output>();
      output->resource =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
      wl_output_add_listener(output->resource, &kOutputListener, output.get());
      state.outputs.push_back(std::move(output));
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };
} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::println(stderr, "usage: layer-client <output> <exclusive-height-or-zero-background> [bottom-layer]");
    return EXIT_FAILURE;
  }
  const std::string outputName = argv[1];
  const int exclusiveHeight = std::atoi(argv[2]);
  if (exclusiveHeight < 0) {
    std::println(stderr, "layer-client: exclusive height must not be negative");
    return EXIT_FAILURE;
  }
  // A 200x200 bottom-layer square in the top-left corner, which the overview mirrors into every workspace preview.
  const bool bottom = argc == 4 && std::string(argv[3]) == "bottom-layer";

  State state;
  const bool background = !bottom && exclusiveHeight == 0;
  if (background) {
    state.fillColor = 0xFF5577AA;
  } else if (bottom) {
    state.fillColor = 0xFF00FF00;
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "layer-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  const auto selected =
      std::ranges::find_if(state.outputs, [&outputName](const auto& output) { return output->name == outputName; });
  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.layerShell == nullptr
      || selected == state.outputs.end()) {
    std::println(stderr, "layer-client: missing protocol or output '{}'", outputName);
    return EXIT_FAILURE;
  }

  uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  if (background) {
    layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
  } else if (bottom) {
    layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
  }
  state.surface = wl_compositor_create_surface(state.compositor);
  state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      state.layerShell, state.surface, (*selected)->resource, layer, "umbriel-output-restore-regression"
  );
  zwlr_layer_surface_v1_add_listener(state.layerSurface, &kLayerSurfaceListener, &state);
  uint32_t anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
  if (bottom) {
    zwlr_layer_surface_v1_set_size(state.layerSurface, 200, 200);
  } else {
    zwlr_layer_surface_v1_set_size(state.layerSurface, 0, background ? 0U : static_cast<uint32_t>(exclusiveHeight));
    anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (background) {
      anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    } else {
      zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, exclusiveHeight);
    }
  }
  zwlr_layer_surface_v1_set_anchor(state.layerSurface, anchors);
  wl_surface_commit(state.surface);

  while (!state.closed && wl_display_dispatch(state.display) >= 0) {
  }

  if (state.frame != nullptr) {
    wl_callback_destroy(state.frame);
  }
  zwlr_layer_surface_v1_destroy(state.layerSurface);
  wl_surface_destroy(state.surface);
  destroyBuffer(state.buffer);
  for (const auto& output : state.outputs) {
    wl_output_release(output->resource);
  }
  zwlr_layer_shell_v1_destroy(state.layerShell);
  wl_shm_destroy(state.shm);
  wl_compositor_destroy(state.compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(state.display);
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
