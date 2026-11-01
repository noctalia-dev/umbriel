#include "render_fixture.h"

#include <wlr/interfaces/wlr_buffer.h>

static bool test_hdr_capture(struct fixture* fixture, uint32_t format) {
  struct wlr_buffer* targets[] = {
      create_output_buffer(fixture, format, TEST_WIDTH, TEST_HEIGHT),
      create_output_buffer(fixture, format, TEST_WIDTH, TEST_HEIGHT),
  };
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
  bool ok = check(targets[0] != NULL && targets[1] != NULL && transform != NULL, "allocate HDR capture targets");
  struct fx_framebuffer* previous_capture = NULL;
  const struct {
    float red;
    uint8_t expected_red;
  } frames[] = {{0.2F, 51}, {0.6F, 153}};

  for (size_t i = 0; ok && i < sizeof(targets) / sizeof(targets[0]); i++) {
    const struct wlr_buffer_pass_options options = {.color_transform = transform};
    struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, targets[i], &options);
    if (!check(pass != NULL, "begin HDR output pass")) {
      ok = false;
      break;
    }
    struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
    struct fx_offscreen_buffers* buffers = fx_pass->output_buffers;
    fx_pass->output_buffer->capture_sdr = true;
    wlr_render_pass_add_rect(
        pass,
        &(struct wlr_render_rect_options){
            .box = {.width = TEST_WIDTH, .height = TEST_HEIGHT},
            .color = {.r = frames[i].red, .g = 0.25F, .b = 0.5F, .a = 1.0F},
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        }
    );
    if (!check(wlr_render_pass_submit(pass), "submit HDR output frame")) {
      ok = false;
      break;
    }

    struct wlr_texture* capture = wlr_texture_from_buffer(fixture->renderer, targets[i]);
    if (!check(capture != NULL, "HDR output exposes an SDR capture")) {
      ok = false;
      break;
    }
    uint8_t pixel[4] = {0};
    bool read = wlr_texture_read_pixels(
        capture,
        &(struct wlr_texture_read_pixels_options){
            .data = pixel,
            .format = DRM_FORMAT_ABGR8888,
            .stride = sizeof(pixel),
            .src_box = {.x = TEST_WIDTH / 2, .y = TEST_HEIGHT / 2, .width = 1, .height = 1},
        }
    );
    wlr_texture_destroy(capture);
    ok = check(read, "read the SDR capture as 8-bit RGBA") && ok;
    const uint8_t expected[] = {frames[i].expected_red, 64, 128, 255};
    for (size_t channel = 0; channel < sizeof(pixel); channel++) {
      ok = check(abs(pixel[channel] - expected[channel]) <= 1, "SDR capture contains the current frame's pixels") && ok;
    }
    ok =
        check(buffers != NULL && buffers->sdr_capture_buffer != NULL, "capture uses an output-local SDR sidecar") && ok;
    if (buffers != NULL) {
      if (previous_capture != NULL) {
        ok = check(buffers->sdr_capture_buffer == previous_capture, "successive frames reuse the SDR sidecar") && ok;
      }
      previous_capture = buffers->sdr_capture_buffer;
    }
  }

  wlr_color_transform_unref(transform);
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    if (targets[i] != NULL) {
      wlr_buffer_drop(targets[i]);
    }
  }
  if (!ok) {
    fprintf(stderr, "FAIL: HDR capture format 0x%08X\n", format);
  }
  return ok;
}

int main(void) {
  struct fixture fixture;
  if (!fixture_init(&fixture)) {
    fprintf(stderr, "SKIP: no FP16-capable DRM render node\n");
    fixture_finish(&fixture);
    return 77;
  }
  const uint32_t formats[] = {DRM_FORMAT_ABGR8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010};
  bool ok = true;
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    ok = test_hdr_capture(&fixture, formats[i]) && ok;
  }
  fixture_finish(&fixture);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
