#include "server/allocator.h"

#include "server/allocator_policy.h"
#include "wlr.h"

#include <cinttypes>
#include <cstdlib>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include <umbrielfx/render/pixel_format.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/dmabuf.h>
}

namespace umbriel {
  namespace {

    struct AlignedUdmabufAllocator {
      wlr_allocator base;
      int fd;
    };

    struct AlignedUdmabufBuffer {
      wlr_buffer base;
      wlr_dmabuf_attributes dmabuf;
      wlr_shm_attributes shm;
      size_t size;
    };

    AlignedUdmabufBuffer* fromBuffer(wlr_buffer* wlr_buffer) {
      return reinterpret_cast<AlignedUdmabufBuffer*>(
          reinterpret_cast<char*>(wlr_buffer) - offsetof(AlignedUdmabufBuffer, base)
      );
    }

    AlignedUdmabufAllocator* fromAllocator(wlr_allocator* wlr_allocator) {
      return reinterpret_cast<AlignedUdmabufAllocator*>(
          reinterpret_cast<char*>(wlr_allocator) - offsetof(AlignedUdmabufAllocator, base)
      );
    }

    bool buffer_get_dmabuf(wlr_buffer* wlr_buffer, wlr_dmabuf_attributes* dmabuf) {
      AlignedUdmabufBuffer* buffer = fromBuffer(wlr_buffer);
      *dmabuf = buffer->dmabuf;
      return true;
    }

    bool buffer_get_shm(wlr_buffer* wlr_buffer, wlr_shm_attributes* shm) {
      AlignedUdmabufBuffer* buffer = fromBuffer(wlr_buffer);
      *shm = buffer->shm;
      return true;
    }

    void buffer_destroy(wlr_buffer* wlr_buffer) {
      AlignedUdmabufBuffer* buffer = fromBuffer(wlr_buffer);
      wlr_buffer_finish(wlr_buffer);
      wlr_dmabuf_attributes_finish(&buffer->dmabuf);
      close(buffer->shm.fd);
      free(buffer);
    }

    const wlr_buffer_impl buffer_impl = {
        .destroy = buffer_destroy,
        .get_dmabuf = buffer_get_dmabuf,
        .get_shm = buffer_get_shm,
        .begin_data_ptr_access = nullptr,
        .end_data_ptr_access = nullptr,
    };

    wlr_buffer*
    allocator_create_buffer(wlr_allocator* wlr_allocator, int width, int height, const wlr_drm_format* format) {
      AlignedUdmabufAllocator* allocator = fromAllocator(wlr_allocator);
      const int minStride = fx_pixel_format_min_stride(format->format, width);
      if (minStride <= 0) {
        wlr_log(WLR_ERROR, "Unsupported pixel format 0x%" PRIX32, format->format);
        return nullptr;
      }
      long page_size = sysconf(_SC_PAGE_SIZE);
      if (page_size == -1) {
        wlr_log_errno(WLR_ERROR, "Failed to query page size");
        return nullptr;
      }
      auto* buffer = static_cast<AlignedUdmabufBuffer*>(calloc(1, sizeof(AlignedUdmabufBuffer)));
      if (buffer == nullptr) {
        return nullptr;
      }
      wlr_buffer_init(&buffer->base, &buffer_impl, width, height);

      const AlignedBufferLayout layout = alignedBufferLayout(minStride, height, page_size);
      const int stride = layout.stride;
      const size_t size = layout.size;

      int memfd = memfd_create("wlroots-aligned", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (memfd < 0) {
        wlr_log_errno(WLR_ERROR, "memfd_create() failed");
        free(buffer);
        return nullptr;
      }

      if (ftruncate(memfd, static_cast<off_t>(size)) < 0) {
        wlr_log_errno(WLR_ERROR, "ftruncate() failed");
        close(memfd);
        free(buffer);
        return nullptr;
      }

      if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK) < 0) {
        wlr_log_errno(WLR_ERROR, "fcntl(F_ADD_SEALS) failed");
        close(memfd);
        free(buffer);
        return nullptr;
      }

      struct udmabuf_create udmabuf_create = {};
      udmabuf_create.memfd = static_cast<__u32>(memfd);
      udmabuf_create.flags = UDMABUF_FLAGS_CLOEXEC;
      udmabuf_create.offset = 0;
      udmabuf_create.size = size;

      int dmabuf_fd = ioctl(allocator->fd, UDMABUF_CREATE, &udmabuf_create);
      if (dmabuf_fd < 0) {
        wlr_log_errno(WLR_ERROR, "ioctl(UDMABUF_CREATE) failed");
        close(memfd);
        free(buffer);
        return nullptr;
      }

      buffer->size = size;
      buffer->shm = {};
      buffer->shm.fd = memfd;
      buffer->shm.format = format->format;
      buffer->shm.width = width;
      buffer->shm.height = height;
      buffer->shm.stride = stride;
      buffer->shm.offset = 0;

      buffer->dmabuf = {};
      buffer->dmabuf.width = width;
      buffer->dmabuf.height = height;
      buffer->dmabuf.format = format->format;
      buffer->dmabuf.modifier = DRM_FORMAT_MOD_LINEAR;
      buffer->dmabuf.n_planes = 1;
      buffer->dmabuf.offset[0] = 0;
      buffer->dmabuf.stride[0] = static_cast<uint32_t>(stride);
      buffer->dmabuf.fd[0] = dmabuf_fd;

      return &buffer->base;
    }

    void allocator_destroy(wlr_allocator* wlr_allocator) {
      AlignedUdmabufAllocator* allocator = fromAllocator(wlr_allocator);
      close(allocator->fd);
      free(allocator);
    }

    const wlr_allocator_interface allocator_impl = {
        .create_buffer = allocator_create_buffer,
        .destroy = allocator_destroy,
    };

    wlr_allocator* createAlignedUdmabufAllocator() {
      int fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        return nullptr;
      }
      auto* allocator = static_cast<AlignedUdmabufAllocator*>(calloc(1, sizeof(AlignedUdmabufAllocator)));
      if (allocator == nullptr) {
        close(fd);
        return nullptr;
      }
      wlr_allocator_init(&allocator->base, &allocator_impl, WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DMABUF);
      allocator->fd = fd;
      return &allocator->base;
    }

  } // namespace

  wlr_allocator* createAllocator(wlr_backend* backend, wlr_renderer* renderer) {
    if (wantsAlignedSoftwareAllocator(wlr_backend_get_drm_fd(backend), wlr_renderer_get_drm_fd(renderer))) {
      if (wlr_allocator* alloc = createAlignedUdmabufAllocator()) {
        return alloc;
      }
    }

    return wlr_allocator_autocreate(backend, renderer);
  }

} // namespace umbriel
