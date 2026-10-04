#include <assert.h>
#include <drm_fourcc.h>
#include <umbrielfx/render/pixel_format.h>
#include <wlr/util/log.h>
#include "render/pixel_format.h"

static const struct wlr_pixel_format_info pixel_format_info[] = {
	{
		.drm_format = DRM_FORMAT_XRGB8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_ARGB8888,
		.opaque_substitute = DRM_FORMAT_XRGB8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_XBGR8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_ABGR8888,
		.opaque_substitute = DRM_FORMAT_XBGR8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_RGBX8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_RGBA8888,
		.opaque_substitute = DRM_FORMAT_RGBX8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_BGRX8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_BGRA8888,
		.opaque_substitute = DRM_FORMAT_BGRX8888,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_R8,
		.bytes_per_block = 1,
	},
	{
		.drm_format = DRM_FORMAT_R16F,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_R32F,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_GR88,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_GR1616F,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_GR3232F,
		.bytes_per_block = 8,
	},
	{
		.drm_format = DRM_FORMAT_RGB888,
		.bytes_per_block = 3,
	},
	{
		.drm_format = DRM_FORMAT_BGR888,
		.bytes_per_block = 3,
	},
	{
		.drm_format = DRM_FORMAT_BGR161616,
		.bytes_per_block = 6,
	},
	{
		.drm_format = DRM_FORMAT_BGR161616F,
		.bytes_per_block = 6,
	},
	{
		.drm_format = DRM_FORMAT_BGR323232F,
		.bytes_per_block = 12,
	},
	{
		.drm_format = DRM_FORMAT_RGBX4444,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_RGBA4444,
		.opaque_substitute = DRM_FORMAT_RGBX4444,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_BGRX4444,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_BGRA4444,
		.opaque_substitute = DRM_FORMAT_BGRX4444,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_RGBX5551,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_RGBA5551,
		.opaque_substitute = DRM_FORMAT_RGBX5551,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_BGRX5551,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_BGRA5551,
		.opaque_substitute = DRM_FORMAT_BGRX5551,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_XRGB1555,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_ARGB1555,
		.opaque_substitute = DRM_FORMAT_XRGB1555,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_RGB565,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_BGR565,
		.bytes_per_block = 2,
	},
	{
		.drm_format = DRM_FORMAT_XRGB2101010,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_ARGB2101010,
		.opaque_substitute = DRM_FORMAT_XRGB2101010,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_XBGR2101010,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_ABGR2101010,
		.opaque_substitute = DRM_FORMAT_XBGR2101010,
		.bytes_per_block = 4,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616F,
		.bytes_per_block = 8,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616F,
		.opaque_substitute = DRM_FORMAT_XBGR16161616F,
		.bytes_per_block = 8,
	},
	{
		.drm_format = DRM_FORMAT_ABGR32323232F,
		.bytes_per_block = 16,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616,
		.bytes_per_block = 8,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616,
		.opaque_substitute = DRM_FORMAT_XBGR16161616,
		.bytes_per_block = 8,
	},
	{
		.drm_format = DRM_FORMAT_YVYU,
		.bytes_per_block = 4,
		.block_width = 2,
		.block_height = 1,
	},
	{
		.drm_format = DRM_FORMAT_VYUY,
		.bytes_per_block = 4,
		.block_width = 2,
		.block_height = 1,
	},
};

static const uint32_t opaque_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_R8,
	DRM_FORMAT_GR88,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGBX4444,
	DRM_FORMAT_BGRX4444,
	DRM_FORMAT_RGBX5551,
	DRM_FORMAT_BGRX5551,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_XBGR16161616F,
	DRM_FORMAT_XBGR16161616,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_NV12,
	DRM_FORMAT_P010,
};

static const size_t pixel_format_info_size =
	sizeof(pixel_format_info) / sizeof(pixel_format_info[0]);

static const size_t opaque_pixel_formats_size =
	sizeof(opaque_pixel_formats) / sizeof(opaque_pixel_formats[0]);

const struct wlr_pixel_format_info *drm_get_pixel_format_info(uint32_t fmt) {
	for (size_t i = 0; i < pixel_format_info_size; ++i) {
		if (pixel_format_info[i].drm_format == fmt) {
			return &pixel_format_info[i];
		}
	}

	return NULL;
}

uint32_t convert_wl_shm_format_to_drm(enum wl_shm_format fmt) {
	switch (fmt) {
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	default:
		return (uint32_t)fmt;
	}
}

enum wl_shm_format convert_drm_format_to_wl_shm(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	default:
		return (enum wl_shm_format)fmt;
	}
}

uint32_t pixel_format_info_pixels_per_block(const struct wlr_pixel_format_info *info) {
	uint32_t pixels = info->block_width * info->block_height;
	return pixels > 0 ? pixels : 1;
}

static int32_t div_round_up(int32_t dividend, int32_t divisor) {
	int32_t quotient = dividend / divisor;
	if (dividend % divisor != 0) {
		quotient++;
	}
	return quotient;
}

int32_t pixel_format_info_min_stride(const struct wlr_pixel_format_info *fmt, int32_t width) {
	int32_t pixels_per_block = (int32_t)pixel_format_info_pixels_per_block(fmt);
	int32_t bytes_per_block = (int32_t)fmt->bytes_per_block;
	if (width > INT32_MAX / bytes_per_block) {
		wlr_log(WLR_DEBUG, "Invalid width %d (overflow)", width);
		return 0;
	}
	return div_round_up(width * bytes_per_block, pixels_per_block);
}

bool pixel_format_info_check_stride(const struct wlr_pixel_format_info *fmt,
		int32_t stride, int32_t width) {
	int32_t bytes_per_block = (int32_t)fmt->bytes_per_block;
	if (stride % bytes_per_block != 0) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (incompatible with %d "
			"bytes-per-block)", stride, bytes_per_block);
		return false;
	}

	int32_t min_stride = pixel_format_info_min_stride(fmt, width);
	if (min_stride <= 0) {
		return false;
	} else if (stride < min_stride) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (too small for %d "
			"bytes-per-block and width %d)", stride, bytes_per_block, width);
		return false;
	}

	return true;
}

bool pixel_format_has_alpha(uint32_t fmt) {
	for (size_t i = 0; i < opaque_pixel_formats_size; i++) {
		if (fmt == opaque_pixel_formats[i]) {
			return false;
		}
	}
	return true;
}

bool pixel_format_is_ycbcr(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_AYUV:
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV15:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV20:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV30:
	case DRM_FORMAT_NV42:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_P010:
	case DRM_FORMAT_P012:
	case DRM_FORMAT_P016:
	case DRM_FORMAT_P030:
	case DRM_FORMAT_P210:
	case DRM_FORMAT_Q401:
	case DRM_FORMAT_Q410:
	case DRM_FORMAT_S010:
	case DRM_FORMAT_S012:
	case DRM_FORMAT_S016:
	case DRM_FORMAT_S210:
	case DRM_FORMAT_S212:
	case DRM_FORMAT_S216:
	case DRM_FORMAT_S410:
	case DRM_FORMAT_S412:
	case DRM_FORMAT_S416:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VUY101010:
	case DRM_FORMAT_VUY888:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_X0L0:
	case DRM_FORMAT_X0L2:
	case DRM_FORMAT_XVYU12_16161616:
	case DRM_FORMAT_XVYU16161616:
	case DRM_FORMAT_XVYU2101010:
	case DRM_FORMAT_XYUV8888:
	case DRM_FORMAT_Y0L0:
	case DRM_FORMAT_Y0L2:
	case DRM_FORMAT_Y210:
	case DRM_FORMAT_Y212:
	case DRM_FORMAT_Y216:
	case DRM_FORMAT_Y410:
	case DRM_FORMAT_Y412:
	case DRM_FORMAT_Y416:
	case DRM_FORMAT_YUV410:
	case DRM_FORMAT_YUV411:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YUV420_10BIT:
	case DRM_FORMAT_YUV420_8BIT:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YUV444:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVU410:
	case DRM_FORMAT_YVU411:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU422:
	case DRM_FORMAT_YVU444:
	case DRM_FORMAT_YVYU:
		return true;
	}
	return false;
}

int32_t fx_pixel_format_min_stride(uint32_t drm_format, int32_t width) {
	const struct wlr_pixel_format_info *info = drm_get_pixel_format_info(drm_format);
	if (info == NULL) {
		return 0;
	}
	return pixel_format_info_min_stride(info, width);
}
