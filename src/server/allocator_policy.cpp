#include "server/allocator_policy.h"

namespace umbriel {

  namespace {
    constexpr int rowAlignment = 64;
    constexpr int heightAlignment = 64;

    constexpr int alignUp(int value, int alignment) { return (value + alignment - 1) & ~(alignment - 1); }
  } // namespace

  AlignedBufferLayout alignedBufferLayout(int minStride, int height, long pageSize) {
    AlignedBufferLayout layout;
    layout.stride = alignUp(minStride, rowAlignment);
    layout.allocHeight = alignUp(height, heightAlignment);
    layout.size = static_cast<std::size_t>(layout.stride) * static_cast<std::size_t>(layout.allocHeight);
    const auto page = static_cast<std::size_t>(pageSize);
    if (page > 0 && layout.size % page != 0) {
      layout.size += page - (layout.size % page);
    }
    return layout;
  }

  bool wantsAlignedSoftwareAllocator(int backendDrmFd, int rendererDrmFd) {
    return backendDrmFd < 0 && rendererDrmFd < 0;
  }

} // namespace umbriel
