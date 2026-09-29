#pragma once

#include <cstddef>

namespace umbriel {

  struct AlignedBufferLayout {
    int stride = 0;
    int allocHeight = 0;
    std::size_t size = 0;
    bool operator==(const AlignedBufferLayout&) const = default;
  };

  // llvmpipe reads rows with aligned SIMD loads and in 4-row blocks, so pad stride and height to 64 and the total to a
  // whole page.
  [[nodiscard]] AlignedBufferLayout alignedBufferLayout(int minStride, int height, long pageSize);

  // The custom aligned udmabuf allocator replaces wlroots's built-in udmabuf allocator,
  // which is only selected on the software rendering path when neither the backend nor the
  // renderer has a DRM device file descriptor.
  [[nodiscard]] bool wantsAlignedSoftwareAllocator(int backendDrmFd, int rendererDrmFd);

} // namespace umbriel
