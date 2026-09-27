#pragma once

struct wlr_allocator;
struct wlr_backend;
struct wlr_renderer;

namespace umbriel {

  wlr_allocator* createAllocator(wlr_backend* backend, wlr_renderer* renderer);

} // namespace umbriel
