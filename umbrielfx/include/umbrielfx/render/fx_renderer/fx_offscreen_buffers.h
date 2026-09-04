#ifndef _FX_OFFSCREEN_BUFFERS_H
#define _FX_OFFSCREEN_BUFFERS_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

/**
 * Per-output renderer framebuffers. Each buffer is allocated on first use and
 * kept, sized to the output, until it is no longer needed or the output goes
 * away.
 */
struct fx_offscreen_buffers {
	struct wl_list link; // fx_renderer.offscreen_buffers
	struct wlr_addon addon;
	struct fx_renderer *renderer;
	// Allocator of the output the buffers belong to
	struct wlr_allocator *allocator;

	// Linear-light composition target shared by the output swapchain
	struct fx_framebuffer *blend_buffer;
	bool blend_valid;
	uint64_t blend_generation;
	// Gamma 2.2 view shared by SDR capture of an HDR output
	struct fx_framebuffer *sdr_capture_buffer;
	uint64_t sdr_capture_generation;

	// Contains the blurred background for tiled windows
	struct fx_framebuffer *optimized_blur_buffer;
	// Contains the non-blurred background for tiled windows. Used for blurring
	// optimized surfaces with an alpha. Just as inefficient as the regular blur.
	struct fx_framebuffer *optimized_no_blur_buffer;
	// Contains the original pixels to draw over the areas where artifact are visible
	struct fx_framebuffer *blur_saved_pixels_buffer;
	// Blur swaps between the two effects buffers every time it scales the image
	// Buffer used for effects
	struct fx_framebuffer *effects_buffer;
	// Swap buffer used for effects
	struct fx_framebuffer *effects_buffer_swapped;
};

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers *fbos);
struct fx_offscreen_buffers *fx_offscreen_buffers_try_get(struct wlr_output *output);
void fx_offscreen_buffers_invalidate_blend(struct wlr_output *output);

#endif
