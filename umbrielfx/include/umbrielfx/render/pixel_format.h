#ifndef UMBRIELFX_RENDER_PIXEL_FORMAT_H
#define UMBRIELFX_RENDER_PIXEL_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calculates the minimum stride in bytes required for a buffer with the given
 * DRM FourCC format and pixel width. Returns 0 if the format is unsupported
 * or the calculation would overflow.
 */
int32_t fx_pixel_format_min_stride(uint32_t drm_format, int32_t width);

#ifdef __cplusplus
}
#endif

#endif
