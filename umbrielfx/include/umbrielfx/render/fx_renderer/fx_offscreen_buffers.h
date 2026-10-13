#ifndef _FX_OFFSCREEN_BUFFERS_H
#define _FX_OFFSCREEN_BUFFERS_H

#include <stdbool.h>
#include <stdint.h>
#include <umbrielfx/render/effect.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

// Scratch buffers sized to the current render target.
struct fx_target_buffers {
  // Blur swaps between the two effects buffers every time it scales the image
  // Buffer used for effects
  struct fx_framebuffer* effects_buffer;
  // Swap buffer used for effects
  struct fx_framebuffer* effects_buffer_swapped;
  // Copy of the target under an in-place effect, which the program samples
  struct fx_framebuffer* in_place_source;
  struct fx_framebuffer* animation_buffers[FX_ANIMATION_DEPTH];
  struct fx_framebuffer* animation_backdrop;
};

/**
 * Per-output renderer framebuffers. Each buffer is allocated on first use and
 * kept until it is no longer needed or the output goes away.
 */
struct fx_offscreen_buffers {
  struct wl_list link; // fx_renderer.offscreen_buffers
  struct wlr_addon addon;
  struct fx_renderer* renderer;
  // Allocator of the output the buffers belong to
  struct wlr_allocator* allocator;

  // Linear-light composition target shared by the output swapchain
  struct fx_framebuffer* blend_buffer;
  bool blend_valid;
  uint64_t blend_generation;
  // sdr_tf the blend buffer was last composited with
  enum wlr_color_transfer_function blend_sdr_tf;
  // SDR view shared by capture of an HDR output, encoded with the pass sdr_tf
  struct fx_framebuffer* sdr_capture_buffer;
  uint64_t sdr_capture_generation;

  // Contains the blurred background for tiled windows
  struct fx_framebuffer* optimized_blur_buffer;
  // Whether the optimized blur buffers hold a completed render
  bool optimized_blur_valid;
  // sdr_tf the optimized blur buffers were rendered in linear light with, or 0
  // when they hold encoded values
  enum wlr_color_transfer_function optimized_blur_sdr_tf;
  // Contains the non-blurred background for tiled windows. Used for blurring
  // optimized surfaces with an alpha. Just as inefficient as the regular blur.
  struct fx_framebuffer* optimized_no_blur_buffer;
  // Contains the original pixels to draw over the areas where artifact are visible
  struct fx_framebuffer* blur_saved_pixels_buffer;
  // Sized to the output
  struct fx_target_buffers output;
  // Sized to a group capture and everything nested in it, so a group capture
  // never resizes the output set. Dropped after a pass that uses none.
  struct fx_target_buffers group;
};

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers* fbos);
void fx_offscreen_buffers_clear_group(struct fx_offscreen_buffers* fbos);
struct fx_offscreen_buffers* fx_offscreen_buffers_try_get(struct wlr_output* output);
void fx_offscreen_buffers_invalidate_blend(struct wlr_output* output);
void fx_renderer_clear_animation_buffers(struct wlr_output* output);

#endif
