#include "render/fx_renderer/effect.h"
#include "render_fixture.h"

#include <wlr/interfaces/wlr_buffer.h>

static bool submit_solid_frame(struct wlr_render_pass* pass, int width, int height, float red) {
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = {.width = width, .height = height},
          .color = {.r = red, .g = 0.25f, .b = 0.5f, .a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  return wlr_render_pass_submit(pass);
}

static float linear_to_pq(float value);
static float srgb_to_linear(float value);
static uint8_t to_u8(float value);
static bool codes_close(const uint8_t actual[static 4], const uint8_t expected[static 4], int tolerance);

// Unset scene colorimetry renders, explicit SDR metadata changes its PQ
// interpretation, and unsetting it restores the output's default curve.
static bool test_scene_unset_primaries(struct fixture* fixture) {
  bool ok = true;
  struct wlr_scene* scene = NULL;
  struct wlr_buffer* source = NULL;

  struct wlr_output_state enable_state;
  wlr_output_state_init(&enable_state);
  wlr_output_state_set_enabled(&enable_state, true);
  wlr_output_state_set_custom_mode(&enable_state, TEST_WIDTH, TEST_HEIGHT, 0);
  bool enabled = wlr_output_commit_state(fixture->output, &enable_state);
  wlr_output_state_finish(&enable_state);
  if (!check(enabled, "enable scene output")) {
    return false;
  }

  source = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(source != NULL, "allocate scene source buffer")) {
    return false;
  }

  const float grey[4] = {16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, 1.0f};
  struct wlr_render_pass* source_pass = wlr_renderer_begin_buffer_pass(fixture->renderer, source, NULL);
  if (!check(source_pass != NULL, "begin scene source pass")) {
    ok = false;
    goto out;
  }
  wlr_render_pass_add_rect(
      source_pass,
      &(struct wlr_render_rect_options){
          .box = {.width = TEST_WIDTH, .height = TEST_HEIGHT},
          .color = {.r = grey[0], .g = grey[1], .b = grey[2], .a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  if (!check(wlr_render_pass_submit(source_pass), "submit scene source pass")) {
    ok = false;
    goto out;
  }

  scene = wlr_scene_create();
  if (!check(scene != NULL, "create scene")) {
    ok = false;
    goto out;
  }
  struct wlr_scene_output* scene_output = wlr_scene_output_create(scene, fixture->output);
  if (!check(scene_output != NULL, "create scene output")) {
    ok = false;
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(scene_output, false);

  struct wlr_scene_buffer* scene_buffer = wlr_scene_buffer_create(&scene->tree, source);
  if (!check(scene_buffer != NULL, "create scene buffer")) {
    ok = false;
    goto out;
  }
  ok = check(scene_buffer->primaries == 0, "new scene buffer leaves primaries unset") && ok;

  struct wlr_output_state render_state;
  wlr_output_state_init(&render_state);
  bool built = wlr_scene_output_build_state(scene_output, &render_state, NULL);
  ok = check(built, "render scene buffer with unset primaries") && ok;
  ok = check(
           built && (render_state.committed & WLR_OUTPUT_STATE_BUFFER) != 0 && render_state.buffer != NULL,
           "scene render produced an output buffer"
       )
      && ok;
  wlr_output_state_finish(&render_state);

  wlr_scene_output_set_sdr_white_level(scene_output, 203.0f);
  const struct {
    const char* name;
    enum wlr_color_transfer_function transfer_function;
  } metadata[] = {
      {"untagged scene buffer", 0},
      {"explicit sRGB scene buffer", WLR_COLOR_TRANSFER_FUNCTION_SRGB},
      {"scene buffer after unsetting sRGB", 0},
      {"explicit gamma 2.2 scene buffer", WLR_COLOR_TRANSFER_FUNCTION_GAMMA22},
  };
  for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); i++) {
    wlr_scene_buffer_set_transfer_function(scene_buffer, metadata[i].transfer_function);
    wlr_output_state_init(&render_state);
    wlr_output_state_set_image_description(
        &render_state,
        &(struct wlr_output_image_description){
            .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
            .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        }
    );
    uint8_t shown[4] = {0};
    built = wlr_scene_output_build_state(scene_output, &render_state, NULL)
        && render_state.buffer != NULL
        && fixture_read_display_pixel(fixture, render_state.buffer, TEST_WIDTH / 2, TEST_HEIGHT / 2, shown);
    wlr_output_state_finish(&render_state);
    const float linear = metadata[i].transfer_function == WLR_COLOR_TRANSFER_FUNCTION_SRGB ? srgb_to_linear(grey[0])
                                                                                           : powf(grey[0], 2.2f);
    const int expected = to_u8(linear_to_pq(linear * 203.0f / 10000.0f));
    if (!check(built, metadata[i].name)) {
      ok = false;
    } else if (abs((int)shown[2] - expected) > 1) {
      fprintf(stderr, "%s on PQ: shows %u, expected %d\n", metadata[i].name, shown[2], expected);
      ok = false;
    }
  }

  // On a PQ output, the transform the scene builds from the image description
  // must reach the renderer as PQ, so a 16/255 rect decodes with gamma 2.2.
  // The opaque rect covers the buffer; it checks the transform, not the buffer.
  if (!check(wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, grey) != NULL, "create PQ scene rect")) {
    ok = false;
    goto out;
  }
  wlr_output_state_init(&render_state);
  wlr_output_state_set_image_description(
      &render_state,
      &(struct wlr_output_image_description){
          .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
          .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
      }
  );
  uint8_t shown[4] = {0};
  built = wlr_scene_output_build_state(scene_output, &render_state, NULL)
      && render_state.buffer != NULL
      && fixture_read_display_pixel(fixture, render_state.buffer, TEST_WIDTH / 2, TEST_HEIGHT / 2, shown);
  wlr_output_state_finish(&render_state);
  const int expected = to_u8(linear_to_pq(powf(16.0f / 255.0f, 2.2f) * 203.0f / 10000.0f));
  if (!check(built, "render scene on a PQ output")) {
    ok = false;
  } else if (abs((int)shown[2] - expected) > 1) {
    fprintf(stderr, "scene rect on PQ: shows %u, gamma 2.2 expects %d\n", shown[2], expected);
    ok = false;
  }

out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static size_t output_lut_count(struct fixture* fixture) {
  size_t count = 0;
  struct fx_output_lut* lut;
  wl_list_for_each(lut, &fx_get_renderer(fixture->renderer)->output_luts, link) { count++; }
  return count;
}

static bool render_texture(
    struct fixture* fixture, int width, int height, uint32_t output_format, uint32_t input_format,
    uint32_t input_stride, const void* input_data, enum wlr_color_transfer_function input_tf,
    const struct wlr_color_primaries* input_primaries, const float* luminance_multiplier,
    struct wlr_color_transform* output_transform, struct blur_data* blur_data, bool save_restore, uint32_t read_format,
    uint32_t read_stride, void* read_data
) {
  struct wlr_texture* texture =
      wlr_texture_from_pixels(fixture->renderer, input_format, input_stride, 1, 1, input_data);
  if (!check(texture != NULL, "create input texture")) {
    return false;
  }
  struct wlr_buffer* output = create_output_buffer(fixture, output_format, width, height);
  if (!check(output != NULL, "allocate output buffer")) {
    wlr_texture_destroy(texture);
    return false;
  }

  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(
      fixture->renderer, output,
      &(struct wlr_buffer_pass_options){
          .color_transform = output_transform,
      }
  );
  if (!check(pass != NULL, "begin render pass")) {
    wlr_buffer_drop(output);
    wlr_texture_destroy(texture);
    return false;
  }

  struct wlr_box box = {.width = width, .height = height};
  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, width, height);
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = box,
          .color = {.a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  wlr_render_pass_add_texture(
      pass,
      &(struct wlr_render_texture_options){
          .texture = texture,
          .dst_box = box,
          .clip = &region,
          .transform = WL_OUTPUT_TRANSFORM_NORMAL,
          .filter_mode = WLR_SCALE_FILTER_NEAREST,
          .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
          .transfer_function = input_tf,
          .primaries = input_primaries,
          .luminance_multiplier = luminance_multiplier,
      }
  );

  bool ok = true;
  if (blur_data != NULL) {
    ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "initialize FP16 effect buffers");
    if (ok) {
      const float opacity = 1.0f;
      struct fx_render_blur_pass_options blur_options = {
          .tex_options =
              {
                  .base =
                      {
                          .dst_box = box,
                          .clip = &region,
                          .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                          .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                          .alpha = &opacity,
                      },
                  .clip_box = &box,
              },
          .blur_data = blur_data,
          .blur_strength = 1.0f,
      };
      fx_render_pass_add_blur(fx_get_render_pass(pass), &blur_options);
    }
  }
  if (ok && save_restore) {
    // Mirror the blur-artifact compensation in wlr_scene_output_build_state:
    // the padding pixels are copied out of the pass target before the scene
    // renders and copied back afterwards.
    struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
    ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "initialize save/restore buffers");
    struct fx_framebuffer* saved = NULL;
    if (ok) {
      saved = fx_render_pass_blur_saved_pixels_buffer(fx_pass);
      ok = check(saved != NULL, "allocate save/restore buffer");
    }
    if (ok) {
      fx_render_pass_read_to_buffer(fx_pass, &region, saved, fx_pass->buffer);
      fx_render_pass_read_to_buffer(fx_pass, &region, fx_pass->buffer, saved);
    }
  }
  pixman_region32_fini(&region);
  ok = wlr_render_pass_submit(pass) && ok;
  wlr_texture_destroy(texture);
  if (ok) {
    ok = check(read_buffer(fixture, output, read_format, read_stride, read_data), "read output buffer");
  }
  wlr_buffer_drop(output);
  return ok;
}

static float srgb_to_linear(float value) {
  return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float value) {
  return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

static float linear_to_pq(float value) {
  const float c1 = 0.8359375f;
  const float c2 = 18.8515625f;
  const float c3 = 18.6875f;
  const float m = 78.84375f;
  const float n = 0.1593017578125f;
  float powered = powf(fminf(fmaxf(value, 0.0f), 1.0f), n);
  return powf((c1 + c2 * powered) / (1.0f + c3 * powered), m);
}

static uint8_t to_u8(float value) { return lroundf(fminf(fmaxf(value, 0.0f), 1.0f) * 255.0f); }

static uint16_t float_to_half(float value) {
  union {
    float value;
    uint32_t bits;
  } input = {.value = value};
  uint32_t sign = (input.bits >> 16) & 0x8000;
  int exponent = ((input.bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = input.bits & 0x7FFFFF;
  if (exponent <= 0) {
    return sign;
  }
  if (exponent >= 31) {
    return sign | 0x7C00;
  }
  return sign | (exponent << 10) | (mantissa >> 13);
}

static void apply_matrix(float out[static 3], const float matrix[static 9], const float in[static 3]) {
  for (size_t row = 0; row < 3; row++) {
    out[row] = matrix[row * 3] * in[0] + matrix[row * 3 + 1] * in[1] + matrix[row * 3 + 2] * in[2];
  }
}

static bool check_matrix_near(const float actual[static 9], const float expected[static 9], const char* label) {
  for (size_t i = 0; i < 9; i++) {
    if (!isfinite(actual[i]) || fabsf(actual[i] - expected[i]) > 0.0003f) {
      fprintf(stderr, "FAIL: %s coefficient %zu expected %.6f, got %.6f\n", label, i, expected[i], actual[i]);
      return false;
    }
  }
  return true;
}

// These calculations need no renderer. In particular, a uniform dimming ramp
// must retain its brightness instead of normalizing the adapted white to Y=1.
static bool test_hdr_white_point_math(void) {
  const float neutral[3] = {1.0f, 1.0f, 1.0f};
  const float identity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float matrix[9];
  color_transform_sdr_white_point_matrix(matrix, neutral);
  bool ok = check_matrix_near(matrix, identity, "neutral HDR white");

  const float warm[3] = {1.0f, 0.8f, 0.5f};
  // Bradford adaptation in linear sRGB from D65 to this warm white. The
  // off-diagonal coefficients distinguish adaptation from RGB channel gains.
  const float expected[9] = {
      0.78192648f, 0.19335100f,  0.02472252f,  -0.00933757f, 0.60599629f,
      0.00716862f, -0.01052950f, -0.03506980f, 0.25964044f,
  };
  color_transform_sdr_white_point_matrix(matrix, warm);
  ok = check_matrix_near(matrix, expected, "warm Bradford adaptation") && ok;
  const float reference[3] = {1.0f, 1.0f, 1.0f};
  const float highlight[3] = {8.0f, 8.0f, 8.0f};
  float adapted_reference[3], adapted_highlight[3];
  apply_matrix(adapted_reference, matrix, reference);
  apply_matrix(adapted_highlight, matrix, highlight);
  for (size_t channel = 0; channel < 3; channel++) {
    ok = check(
             fabsf(adapted_reference[channel] - srgb_to_linear(warm[channel])) < 0.0003f,
             "adapted reference white decodes the SDR endpoint in linear light"
         )
        && ok;
    ok = check(
             fabsf(adapted_highlight[channel] - 8.0f * adapted_reference[channel]) < 0.0003f,
             "HDR highlights retain their ratio to reference white"
         )
        && ok;
  }
  ok = check(adapted_highlight[0] > 1.0f, "HDR white adaptation retains highlight headroom") && ok;

  const float dim[3] = {0.5f, 0.5f, 0.5f};
  const float dim_level = srgb_to_linear(0.5f);
  const float dim_matrix[9] = {dim_level, 0.0f, 0.0f, 0.0f, dim_level, 0.0f, 0.0f, 0.0f, dim_level};
  color_transform_sdr_white_point_matrix(matrix, dim);
  ok = check_matrix_near(matrix, dim_matrix, "uniform SDR dimming endpoint") && ok;

  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float zero[9] = {0};
  color_transform_sdr_white_point_matrix(matrix, black);
  return check_matrix_near(matrix, zero, "black SDR endpoint") && ok;
}

static float pq_to_linear(float value) {
  const float c1 = 0.8359375f;
  const float c2 = 18.8515625f;
  const float c3 = 18.6875f;
  const float m = 78.84375f;
  const float n = 0.1593017578125f;
  const float powered = powf(value, 1.0f / m);
  return powf(fmaxf(powered - c1, 0.0f) / (c2 - c3 * powered), 1.0f / n);
}

static bool render_white_point_scene(
    struct fixture* fixture, struct wlr_scene_output* scene_output, bool hdr, uint8_t display[static 2][4],
    uint8_t capture[static 2][4]
) {
  const int cursor_locks = fixture->output->software_cursor_locks;
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  const struct wlr_output_image_description description = {
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
  };
  bool ok = check(wlr_output_state_set_image_description(&state, hdr ? &description : NULL), "stage test output mode");
  const struct wlr_scene_output_state_options options = {.capture_sdr = true};
  ok = ok && check(wlr_scene_output_build_state(scene_output, &state, &options), "render white-point scene");
  ok = check(fixture->output->software_cursor_locks == cursor_locks, "pending HDR frame restores live cursor policy")
      && ok;
  if (ok) {
    ok = check(state.buffer != NULL, "white-point render produces a buffer");
  }
  for (size_t sample = 0; ok && sample < 2; sample++) {
    const int x = sample == 0 ? TEST_WIDTH / 4 : TEST_WIDTH * 3 / 4;
    ok = fixture_read_display_pixel(fixture, state.buffer, x, TEST_HEIGHT / 2, display[sample])
        && fixture_read_pixel(fixture, state.buffer, x, TEST_HEIGHT / 2, capture[sample]);
  }
  if (ok) {
    // Headless outputs do not advertise HDR. Present the rendered buffer while
    // leaving the test description pending, as in test_scene_unset_primaries.
    // This consumes the frame's damage so the next setter must schedule it.
    struct wlr_output_state present = state;
    present.committed &= ~WLR_OUTPUT_STATE_IMAGE_DESCRIPTION;
    ok = check(wlr_output_commit_state(fixture->output, &present), "present white-point test buffer");
  }
  wlr_output_state_finish(&state);
  return ok;
}

static bool check_white_point_pixels(
    const uint8_t display[static 2][4], const float white[static 3], const uint8_t source_codes[static 2]
) {
  // Linear sRGB to BT.2020, independently rounded from the standard primary
  // conversion. A neutral source becomes this target white before PQ encoding.
  const float to_bt2020[9] = {
      0.627404f, 0.329283f, 0.043313f, 0.069097f, 0.919540f, 0.011362f, 0.016392f, 0.088013f, 0.895595f,
  };
  const float linear_white[3] = {
      srgb_to_linear(white[0]),
      srgb_to_linear(white[1]),
      srgb_to_linear(white[2]),
  };
  float target_white[3];
  apply_matrix(target_white, to_bt2020, linear_white);
  bool ok = true;
  for (size_t sample = 0; sample < 2; sample++) {
    const float absolute_light = pq_to_linear(source_codes[sample] / 255.0f);
    for (size_t channel = 0; channel < 3; channel++) {
      const int expected = to_u8(linear_to_pq(target_white[channel] * absolute_light));
      const int actual = display[sample][2 - channel];
      if (abs(actual - expected) > 2) {
        fprintf(
            stderr, "FAIL: PQ sample %zu channel %zu expected %d from linear adaptation, got %d\n", sample, channel,
            expected, actual
        );
        ok = false;
      }
    }
  }
  return ok;
}

// Exercise the scene's output transform with tagged PQ content, unchanged
// capture values, and white-point changes while the source stays still.
static bool test_hdr_white_point(struct fixture* fixture) {
  bool ok = true;
  struct wlr_scene* scene = NULL;
  struct wlr_buffer* source = NULL;
  struct wlr_output_state enable;
  wlr_output_state_init(&enable);
  wlr_output_state_set_enabled(&enable, true);
  wlr_output_state_set_custom_mode(&enable, TEST_WIDTH, TEST_HEIGHT, 0);
  const bool enabled = wlr_output_commit_state(fixture->output, &enable);
  wlr_output_state_finish(&enable);
  if (!check(enabled, "enable white-point scene output")) {
    return false;
  }

  source = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(source != NULL, "allocate tagged PQ scene source")) {
    return false;
  }
  const uint8_t source_codes[2] = {to_u8(linear_to_pq(203.0f / 10000.0f)), to_u8(linear_to_pq(1000.0f / 10000.0f))};
  struct wlr_render_pass* paint = wlr_renderer_begin_buffer_pass(fixture->renderer, source, NULL);
  if (!check(paint != NULL, "begin tagged PQ source paint")) {
    ok = false;
    goto out;
  }
  for (size_t sample = 0; sample < 2; sample++) {
    const float code = source_codes[sample] / 255.0f;
    wlr_render_pass_add_rect(
        paint,
        &(struct wlr_render_rect_options){
            .box = {.x = sample * (TEST_WIDTH / 2), .width = TEST_WIDTH / 2, .height = TEST_HEIGHT},
            .color = {.r = code, .g = code, .b = code, .a = 1.0f},
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        }
    );
  }
  if (!check(wlr_render_pass_submit(paint), "paint PQ reference white and highlight")) {
    ok = false;
    goto out;
  }

  scene = wlr_scene_create();
  struct wlr_scene_output* scene_output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  struct wlr_scene_buffer* content = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  if (!check(scene_output != NULL && content != NULL, "create tagged PQ scene")) {
    ok = false;
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(scene_output, false);
  wlr_scene_output_set_sdr_white_level(scene_output, 203.0f);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
  wlr_scene_buffer_set_primaries(content, WLR_COLOR_NAMED_PRIMARIES_BT2020);
  uint8_t baseline[2][4], baseline_capture[2][4], shown[2][4], captured[2][4], initial_sdr[2][4];
  const float neutral[3] = {1.0f, 1.0f, 1.0f};
  // Allocate the headless output's swapchain in its supported SDR mode before
  // rendering pending HDR descriptions through that same swapchain.
  if (!render_white_point_scene(fixture, scene_output, false, initial_sdr, captured)) {
    ok = false;
    goto out;
  }
  if (!render_white_point_scene(fixture, scene_output, true, baseline, baseline_capture)) {
    ok = false;
    goto out;
  }
  ok = check_white_point_pixels(baseline, neutral, source_codes) && ok;
  ok = check(!wlr_scene_output_needs_frame(scene_output), "presented static scene has no pending frame") && ok;

  const float warm[3] = {1.0f, 0.8f, 0.5f};
  wlr_scene_output_set_hdr_white_point(scene_output, warm);
  ok = check(wlr_scene_output_needs_frame(scene_output), "warming a static scene schedules a new frame") && ok;
  // Reject the target format after the pending HDR cursor policy is selected.
  // A failed transition must leave the running SDR output's lock count intact.
  const int cursor_locks = fixture->output->software_cursor_locks;
  struct wlr_output_state rejected;
  wlr_output_state_init(&rejected);
  const struct wlr_output_image_description hdr_description = {
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
  };
  wlr_output_state_set_image_description(&rejected, &hdr_description);
  wlr_output_state_set_render_format(&rejected, DRM_FORMAT_INVALID);
  ok = check(!wlr_scene_output_build_state(scene_output, &rejected, NULL), "invalid target rejects an HDR frame") && ok;
  ok = check(
           fixture->output->software_cursor_locks == cursor_locks,
           "failed HDR frame restores the running output's cursor locks"
       )
      && ok;
  wlr_output_state_finish(&rejected);
  if (!render_white_point_scene(fixture, scene_output, true, shown, captured)) {
    ok = false;
    goto out;
  }
  ok = check_white_point_pixels(shown, warm, source_codes) && ok;
  for (size_t sample = 0; sample < 2; sample++) {
    ok = check(codes_close(captured[sample], baseline_capture[sample], 1), "warming leaves SDR capture neutral") && ok;
  }
  ok = check(shown[1][2] > shown[0][2] + 20, "warm HDR highlight remains brighter than reference white") && ok;

  const float warmer[3] = {1.0f, 0.65f, 0.3f};
  wlr_scene_output_set_hdr_white_point(scene_output, warmer);
  if (!render_white_point_scene(fixture, scene_output, true, shown, captured)) {
    ok = false;
    goto out;
  }
  ok = check_white_point_pixels(shown, warmer, source_codes) && ok;

  wlr_scene_output_set_hdr_white_point(scene_output, neutral);
  ok = check(wlr_scene_output_needs_frame(scene_output), "resetting warmth schedules a new frame") && ok;
  if (!render_white_point_scene(fixture, scene_output, true, shown, captured)) {
    ok = false;
    goto out;
  }
  for (size_t sample = 0; sample < 2; sample++) {
    ok = check(codes_close(shown[sample], baseline[sample], 1), "reset restores the original HDR display values") && ok;
  }

  if (!render_white_point_scene(fixture, scene_output, false, baseline, baseline_capture)) {
    ok = false;
    goto out;
  }
  for (size_t sample = 0; sample < 2; sample++) {
    ok = check(
             codes_close(baseline[sample], initial_sdr[sample], 1),
             "returning from HDR restores the original SDR highlight mapping"
         )
        && ok;
  }
  for (size_t channel = 0; channel < 3; channel++) {
    ok = check(
             baseline[0][channel] < baseline[1][channel] && baseline[1][channel] < 255,
             "returning from HDR preserves distinct SDR highlights without clipping"
         )
        && ok;
  }
  wlr_scene_output_set_hdr_white_point(scene_output, warm);
  if (!render_white_point_scene(fixture, scene_output, false, shown, captured)) {
    ok = false;
    goto out;
  }
  for (size_t sample = 0; sample < 2; sample++) {
    ok = check(codes_close(shown[sample], baseline[sample], 1), "HDR white point leaves SDR rendering unchanged") && ok;
  }

out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static bool render_sdr_alpha_scene(
    struct fixture* fixture, struct wlr_scene_output* scene_output, int expected, const char* label
) {
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  bool ok = check(wlr_scene_output_build_state(scene_output, &state, NULL), "render SDR alpha scene");
  ok = check(state.buffer != NULL, "SDR alpha render produces a buffer") && ok;
  uint8_t shown[4] = {0};
  if (ok) {
    ok = fixture_read_display_pixel(fixture, state.buffer, TEST_WIDTH / 2, TEST_HEIGHT / 2, shown);
  }
  if (ok) {
    ok = check(wlr_output_commit_state(fixture->output, &state), "present SDR alpha scene");
    for (size_t channel = 0; channel < 3; channel++) {
      if (abs((int)shown[channel] - expected) > 1) {
        fprintf(stderr, "FAIL: %s channel %zu expected %d, got %u\n", label, channel, expected, shown[channel]);
        ok = false;
      }
    }
  }
  wlr_output_state_finish(&state);
  return ok;
}

// HDR white-point state must not switch an SDR desktop to linear blending.
// Exercise actual translucent scene pixels before and after setting warmth,
// then ensure a real shader gamma ramp can be applied and removed independently.
static bool test_sdr_white_point_alpha(struct fixture* fixture) {
  bool ok = true;
  struct wlr_scene* scene = NULL;
  struct wlr_buffer* source = NULL;
  struct wlr_output_state enable;
  wlr_output_state_init(&enable);
  wlr_output_state_set_enabled(&enable, true);
  wlr_output_state_set_custom_mode(&enable, TEST_WIDTH, TEST_HEIGHT, 0);
  const bool enabled = wlr_output_commit_state(fixture->output, &enable);
  wlr_output_state_finish(&enable);
  if (!check(enabled, "enable SDR alpha output")) {
    return false;
  }
  if (!check(wlr_output_get_gamma_size(fixture->output) == 0, "headless output uses shader gamma ramps")) {
    return false;
  }

  source = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(source != NULL, "allocate translucent SDR source")) {
    return false;
  }
  struct wlr_render_pass* paint = wlr_renderer_begin_buffer_pass(fixture->renderer, source, NULL);
  if (!check(paint != NULL, "begin translucent SDR source paint")) {
    ok = false;
    goto out;
  }
  const float alpha = 247.0f / 255.0f;
  wlr_render_pass_add_rect(
      paint,
      &(struct wlr_render_rect_options){
          .box = {.width = TEST_WIDTH, .height = TEST_HEIGHT},
          .color = {.a = alpha},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  if (!check(wlr_render_pass_submit(paint), "paint translucent SDR source")) {
    ok = false;
    goto out;
  }

  scene = wlr_scene_create();
  struct wlr_scene_output* scene_output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  struct wlr_scene_rect* backdrop =
      scene != NULL ? wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, white) : NULL;
  struct wlr_scene_buffer* content = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  if (!check(scene_output != NULL && backdrop != NULL && content != NULL, "create translucent SDR scene")) {
    ok = false;
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(scene_output, false);
  const int encoded = to_u8(1.0f - alpha);
  ok = render_sdr_alpha_scene(fixture, scene_output, encoded, "initial encoded SDR blending") && ok;
  ok = check(!wlr_scene_output_needs_frame(scene_output), "static SDR alpha scene is idle") && ok;

  const float warm[3] = {1.0f, 0.8f, 0.5f};
  wlr_scene_output_set_hdr_white_point(scene_output, warm);
  ok = check(wlr_scene_output_needs_frame(scene_output), "white-point change schedules the SDR test frame") && ok;
  ok = render_sdr_alpha_scene(fixture, scene_output, encoded, "HDR warmth leaves SDR alpha unchanged") && ok;
  wlr_scene_output_set_hdr_white_point(scene_output, white);
  ok = render_sdr_alpha_scene(fixture, scene_output, encoded, "resetting HDR warmth preserves SDR alpha") && ok;

  const uint16_t ramp[2] = {0, 32768};
  scene_output->gamma_lut_color_transform = wlr_color_transform_init_lut_3x1d(2, ramp, ramp, ramp);
  if (!check(scene_output->gamma_lut_color_transform != NULL, "create shader gamma dimming ramp")) {
    ok = false;
    goto out;
  }
  const int dimmed_linear = to_u8(linear_to_srgb(1.0f - alpha) * 0.5f);
  ok = render_sdr_alpha_scene(fixture, scene_output, dimmed_linear, "shader gamma ramp remains effective") && ok;
  wlr_color_transform_unref(scene_output->gamma_lut_color_transform);
  scene_output->gamma_lut_color_transform = NULL;
  ok = render_sdr_alpha_scene(fixture, scene_output, encoded, "removing shader gamma restores encoded SDR alpha") && ok;
  ok = check(!wlr_scene_output_needs_frame(scene_output), "restored SDR alpha scene is idle") && ok;

out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static struct wlr_color_transform*
create_output_transform(enum wlr_color_named_primaries primaries, enum wlr_color_transfer_function tf, float scale) {
  struct wlr_color_primaries srgb;
  struct wlr_color_primaries target;
  wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  wlr_color_primaries_from_named(&target, primaries);
  float matrix[9];
  wlr_color_primaries_transform_absolute_colorimetric(&srgb, &target, matrix);
  for (size_t i = 0; i < 9; i++) {
    matrix[i] *= scale;
  }
  struct wlr_color_transform* matrix_transform = wlr_color_transform_init_matrix(matrix);
  struct wlr_color_transform* eotf_transform = wlr_color_transform_init_linear_to_inverse_eotf(tf);
  if (matrix_transform == NULL || eotf_transform == NULL) {
    wlr_color_transform_unref(matrix_transform);
    wlr_color_transform_unref(eotf_transform);
    return NULL;
  }
  struct wlr_color_transform* parts[] = {
      matrix_transform,
      eotf_transform,
  };
  struct wlr_color_transform* pipeline = wlr_color_transform_init_pipeline(parts, 2);
  wlr_color_transform_unref(matrix_transform);
  wlr_color_transform_unref(eotf_transform);
  return pipeline;
}

static bool codes_close(const uint8_t actual[static 4], const uint8_t expected[static 4], int tolerance) {
  for (size_t i = 0; i < 4; i++) {
    if (abs((int)actual[i] - expected[i]) > tolerance) {
      fprintf(stderr, "FAIL: channel %zu expected %u, got %u\n", i, expected[i], actual[i]);
      return false;
    }
  }
  return true;
}

// Renders an SDR texture tagged `texture_tf` (0 for untagged) over the left
// half and a rect of the same color over the right half of an output pass
// encoding `tf` (PQ in BT.2020 at 203 cd/m² SDR white, otherwise sRGB
// primaries), with an SDR capture pending.
static bool render_sdr_on(
    struct fixture* fixture, struct wlr_buffer* target, struct wlr_texture* fill, const struct wlr_render_color* color,
    enum wlr_color_transfer_function texture_tf, enum wlr_color_transfer_function tf, bool effect_capture
) {
  const bool pq = tf == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
  struct wlr_color_transform* transform = create_output_transform(
      pq ? WLR_COLOR_NAMED_PRIMARIES_BT2020 : WLR_COLOR_NAMED_PRIMARIES_SRGB, tf, pq ? 203.0f / 10000.0f : 1.0f
  );
  if (transform == NULL) {
    return false;
  }
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(
      fixture->output, target, &(struct wlr_buffer_pass_options){.color_transform = transform}
  );
  wlr_color_transform_unref(transform);
  if (pass == NULL) {
    return false;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  fx_pass->output_buffer->capture_sdr = true;
  wlr_render_pass_add_texture(
      pass,
      &(struct wlr_render_texture_options){
          .texture = fill,
          .dst_box = {.width = TEST_WIDTH / 2, .height = TEST_HEIGHT},
          .filter_mode = WLR_SCALE_FILTER_NEAREST,
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
          .transfer_function = texture_tf,
      }
  );
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = {.x = TEST_WIDTH / 2, .width = TEST_WIDTH - TEST_WIDTH / 2, .height = TEST_HEIGHT},
          .color = *color,
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  bool saved = !effect_capture || fx_render_pass_save_effect_capture(fx_pass);
  return wlr_render_pass_submit(pass) && saved;
}

// Untagged SDR and compositor colors use the output's gamma 2.2 policy on PQ
// and gamma 2.2 outputs. Explicit SDR metadata keeps its own decoding curve.
// Both SDR capture paths encode the resulting linear values with gamma 2.2.
static bool test_sdr_curve_consistency(struct fixture* fixture) {
  bool ok = true;
  const uint8_t level = 16;
  const uint8_t grey[4] = {level, level, level, 255};
  const struct wlr_render_color color = {.r = level / 255.0f, .g = level / 255.0f, .b = level / 255.0f, .a = 1.0f};
  struct wlr_texture* fill = wlr_texture_from_pixels(fixture->renderer, DRM_FORMAT_ABGR8888, 4, 1, 1, grey);
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(fill != NULL && target != NULL, "allocate SDR curve target")) {
    ok = false;
    goto out;
  }

  const float gamma_linear = powf(level / 255.0f, 2.2f);
  const float srgb_linear = srgb_to_linear(level / 255.0f);
  const int pq_shown = to_u8(linear_to_pq(gamma_linear * 203.0f / 10000.0f));
  const int pq_piecewise = to_u8(linear_to_pq(srgb_linear * 203.0f / 10000.0f));
  const int srgb_captured = to_u8(powf(srgb_linear, 1.0f / 2.2f));
  ok = check(pq_piecewise - pq_shown >= 3, "test level separates gamma 2.2 from piecewise sRGB on PQ") && ok;
  ok = check(srgb_captured - level >= 3, "capture separates explicit sRGB from inherited gamma 2.2") && ok;
  const enum wlr_color_transfer_function pq = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
  const enum wlr_color_transfer_function gamma22 = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
  const struct {
    const char* name;
    enum wlr_color_transfer_function texture_tf, output_tf;
    bool effect_capture;
  } cases[] = {
      {"SDR capture on PQ", 0, pq, false},
      {"effect capture on PQ", 0, pq, true},
      {"SDR capture of sRGB-tagged content on PQ", WLR_COLOR_TRANSFER_FUNCTION_SRGB, pq, false},
      {"effect capture of sRGB-tagged content on PQ", WLR_COLOR_TRANSFER_FUNCTION_SRGB, pq, true},
      {"SDR capture after unsetting sRGB on PQ", 0, pq, false},
      {"effect capture after unsetting sRGB on PQ", 0, pq, true},
      {"SDR capture of gamma 2.2 content on PQ", gamma22, pq, false},
      {"effect capture of gamma 2.2 content on PQ", gamma22, pq, true},
      {"SDR capture on gamma 2.2", 0, gamma22, false},
      {"effect capture on gamma 2.2", 0, gamma22, true},
      {"SDR capture of sRGB-tagged content on gamma 2.2", WLR_COLOR_TRANSFER_FUNCTION_SRGB, gamma22, false},
      {"effect capture of sRGB-tagged content on gamma 2.2", WLR_COLOR_TRANSFER_FUNCTION_SRGB, gamma22, true},
      {"SDR capture of gamma 2.2 content on gamma 2.2", gamma22, gamma22, false},
      {"effect capture of gamma 2.2 content on gamma 2.2", gamma22, gamma22, true},
  };
  const char* sides[] = {"texture", "rect"};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char message[128];
    snprintf(message, sizeof(message), "render SDR for %s", cases[i].name);
    if (!check(
            render_sdr_on(
                fixture, target, fill, &color, cases[i].texture_tf, cases[i].output_tf, cases[i].effect_capture
            ),
            message
        )) {
      ok = false;
      continue;
    }
    // A capture read falls back to the SDR view when no effect capture is
    // valid, which encodes the same way, so make sure the intended one is read.
    struct fx_framebuffer* framebuffer = fx_framebuffer_get_or_create(fx_get_renderer(fixture->renderer), target);
    snprintf(message, sizeof(message), "%s reads the intended capture", cases[i].name);
    if (!check(framebuffer != NULL && framebuffer->effect_capture_valid == cases[i].effect_capture, message)) {
      ok = false;
      continue;
    }
    uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4] = {0};
    snprintf(message, sizeof(message), "read %s", cases[i].name);
    if (!check(read_buffer(fixture, target, DRM_FORMAT_ABGR8888, TEST_WIDTH * 4, pixels), message)) {
      ok = false;
      continue;
    }
    for (size_t side = 0; side < 2; side++) {
      const int x = side == 0 ? TEST_WIDTH / 4 : TEST_WIDTH * 3 / 4, y = TEST_HEIGHT / 2;
      uint8_t shown[4];
      if (!fixture_read_display_pixel(fixture, target, x, y, shown)) {
        ok = false;
        continue;
      }
      const bool explicit_srgb = side == 0 && cases[i].texture_tf == WLR_COLOR_TRANSFER_FUNCTION_SRGB;
      const int expected_capture = explicit_srgb ? srgb_captured : level;
      const int expected = cases[i].output_tf == pq ? (explicit_srgb ? pq_piecewise : pq_shown) : expected_capture;
      if (abs((int)shown[2] - expected) > 1) {
        fprintf(stderr, "%s for %s: shows %u, expected %d\n", sides[side], cases[i].name, shown[2], expected);
        ok = false;
      }
      const uint8_t captured = pixels[(y * TEST_WIDTH + x) * 4];
      if (abs((int)captured - expected_capture) > 1) {
        fprintf(
            stderr, "%s for %s: captured %u, expected %d\n", sides[side], cases[i].name, captured, expected_capture
        );
        ok = false;
      }
    }
  }

out:
  if (target != NULL) {
    wlr_buffer_drop(target);
  }
  if (fill != NULL) {
    wlr_texture_destroy(fill);
  }
  return ok;
}

static bool test_implicit_srgb_target(struct fixture* fixture) {
  const uint8_t input[4] = {17, 17, 45, 255};
  uint8_t implicit_output[4] = {0};
  uint8_t explicit_output[4] = {0};
  struct wlr_color_primaries srgb;
  wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
  bool ok = check(transform != NULL, "create sRGB output transform")
      && render_texture(
                fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, input, WLR_COLOR_TRANSFER_FUNCTION_SRGB,
                &srgb, NULL, NULL, NULL, false, DRM_FORMAT_ABGR8888, 4, implicit_output
      )
      && codes_close(implicit_output, input, 0)
      && render_texture(
                fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, input, WLR_COLOR_TRANSFER_FUNCTION_SRGB,
                &srgb, NULL, transform, NULL, false, DRM_FORMAT_ABGR8888, 4, explicit_output
      )
      && codes_close(explicit_output, implicit_output, 1);
  wlr_color_transform_unref(transform);
  return ok;
}

static bool test_gamma22_to_srgb(struct fixture* fixture) {
  const uint8_t dark_input[4] = {24, 24, 49, 255};
  const uint8_t dark_expected[4] = {17, 17, 45, 255};
  uint8_t dark_output[4] = {0};
  const uint8_t yellow_input[4] = {255, 245, 154, 255};
  const uint8_t yellow_expected[4] = {255, 245, 155, 255};
  uint8_t yellow_output[4] = {0};
  struct wlr_color_primaries srgb;
  wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  return render_texture(
             fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, dark_input,
             WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, &srgb, NULL, NULL, NULL, false, DRM_FORMAT_ABGR8888, 4, dark_output
         )
      && codes_close(dark_output, dark_expected, 1)
      && render_texture(
             fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, yellow_input,
             WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, &srgb, NULL, NULL, NULL, false, DRM_FORMAT_ABGR8888, 4, yellow_output
      )
      && codes_close(yellow_output, yellow_expected, 1);
}

static bool test_pq_roundtrip(struct fixture* fixture) {
  const uint8_t input[4] = {96, 144, 208, 255};
  uint8_t output[4] = {0};
  struct wlr_color_primaries bt2020;
  wlr_color_primaries_from_named(&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);
  const float input_luminance = 10000.0f / 203.0f;
  struct wlr_color_transform* transform = create_output_transform(
      WLR_COLOR_NAMED_PRIMARIES_BT2020, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f
  );
  bool ok =
      check(transform != NULL, "create PQ output transform")
      && render_texture(
          fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, input, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
          &bt2020, &input_luminance, transform, NULL, false, DRM_FORMAT_ABGR8888, 4, output
      )
      && codes_close(output, input, 1);
  wlr_color_transform_unref(transform);
  return ok;
}

static bool test_bt2020_to_srgb(struct fixture* fixture) {
  const uint8_t input[4] = {156, 132, 104, 255};
  uint8_t output[4] = {0};
  struct wlr_color_primaries srgb;
  struct wlr_color_primaries bt2020;
  wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  wlr_color_primaries_from_named(&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);
  float matrix[9];
  wlr_color_primaries_transform_absolute_colorimetric(&bt2020, &srgb, matrix);
  float linear[3] = {
      srgb_to_linear((float)input[0] / 255.0f),
      srgb_to_linear((float)input[1] / 255.0f),
      srgb_to_linear((float)input[2] / 255.0f),
  };
  uint8_t expected[4] = {0};
  for (size_t row = 0; row < 3; row++) {
    float value = matrix[row * 3] * linear[0] + matrix[row * 3 + 1] * linear[1] + matrix[row * 3 + 2] * linear[2];
    expected[row] = to_u8(linear_to_srgb(fmaxf(value, 0.0f)));
  }
  expected[3] = 255;
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
  bool ok = check(transform != NULL, "create sRGB output transform")
      && render_texture(
                fixture, 1, 1, DRM_FORMAT_ABGR8888, DRM_FORMAT_ABGR8888, 4, input, WLR_COLOR_TRANSFER_FUNCTION_SRGB,
                &bt2020, NULL, transform, NULL, false, DRM_FORMAT_ABGR8888, 4, output
      )
      && codes_close(output, expected, 1);
  wlr_color_transform_unref(transform);
  return ok;
}

static uint32_t select_10bit_format(struct fixture* fixture) {
  if (get_render_format(fixture, DRM_FORMAT_ABGR2101010) != NULL) {
    return DRM_FORMAT_ABGR2101010;
  }
  if (get_render_format(fixture, DRM_FORMAT_XBGR2101010) != NULL) {
    return DRM_FORMAT_XBGR2101010;
  }
  return DRM_FORMAT_INVALID;
}

// A real pixel buffer imported by the renderer, without first painting its
// extended-range values through another compositor render pass.
struct linear_test_buffer {
  struct wlr_buffer base;
  uint16_t pixels[2][4];
};

static void linear_buffer_destroy(struct wlr_buffer* base) {
  struct linear_test_buffer* buffer = wl_container_of(base, buffer, base);
  free(buffer);
}

static bool
linear_buffer_begin_access(struct wlr_buffer* base, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
  if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
    return false;
  }
  struct linear_test_buffer* buffer = wl_container_of(base, buffer, base);
  *data = buffer->pixels;
  *format = DRM_FORMAT_ABGR16161616F;
  *stride = base->width * sizeof(buffer->pixels[0]);
  return true;
}

static void linear_buffer_end_access(struct wlr_buffer* base) { (void)base; }

static const struct wlr_buffer_impl linear_buffer_impl = {
    .destroy = linear_buffer_destroy,
    .begin_data_ptr_access = linear_buffer_begin_access,
    .end_data_ptr_access = linear_buffer_end_access,
};

static struct wlr_buffer* create_linear_buffer(const float pixels[static 2][4]) {
  struct linear_test_buffer* buffer = calloc(1, sizeof(*buffer));
  if (buffer == NULL) {
    return NULL;
  }
  for (size_t sample = 0; sample < 2; sample++) {
    for (size_t channel = 0; channel < 4; channel++) {
      buffer->pixels[sample][channel] = float_to_half(pixels[sample][channel]);
    }
  }
  wlr_buffer_init(&buffer->base, &linear_buffer_impl, 2, 1);
  return &buffer->base;
}

static bool enable_color_scene_output(struct fixture* fixture) {
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  wlr_output_state_set_custom_mode(&state, TEST_WIDTH, TEST_HEIGHT, 0);
  bool ok = wlr_output_commit_state(fixture->output, &state);
  wlr_output_state_finish(&state);
  return check(ok, "enable color scene output");
}

// Present each frame to consume damage. Metadata setters must then damage a
// static scene themselves instead of relying on source-buffer repaint damage.
static bool render_metadata_scene(
    struct fixture* fixture, struct wlr_scene_output* scene_output, bool hdr, uint8_t pixels[static 2][4]
) {
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  const struct wlr_output_image_description description = {
      .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
      .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
  };
  bool ok = wlr_output_state_set_image_description(&state, hdr ? &description : NULL)
      && check(wlr_scene_output_build_state(scene_output, &state, NULL), "render color metadata scene")
      && check(state.buffer != NULL, "color metadata scene produced a buffer");
  for (size_t sample = 0; ok && sample < 2; sample++) {
    ok = fixture_read_display_pixel(
        fixture, state.buffer, (2 * sample + 1) * TEST_WIDTH / 4, TEST_HEIGHT / 2, pixels[sample]
    );
  }
  if (ok) {
    // The headless backend supports SDR presentation; exercise pending HDR
    // rendering without asking it to validate a physical HDR output mode.
    struct wlr_output_state present = state;
    present.committed &= ~WLR_OUTPUT_STATE_IMAGE_DESCRIPTION;
    ok = check(wlr_output_commit_state(fixture->output, &present), "present color metadata scene");
  }
  wlr_output_state_finish(&state);
  return ok;
}

static bool check_pq_scene_pixel(const uint8_t pixel[static 4], const float nits[static 3], const char* label) {
  bool ok = true;
  for (size_t channel = 0; channel < 3; channel++) {
    int expected = to_u8(linear_to_pq(nits[channel] / 10000.0f));
    int actual = pixel[2 - channel];
    if (abs(actual - expected) > 2) {
      fprintf(stderr, "FAIL: %s channel %zu: PQ code %d, expected %d\n", label, channel, actual, expected);
      ok = false;
    }
  }
  return ok;
}

static bool test_scene_custom_color(struct fixture* fixture) {
  if (!enable_color_scene_output(fixture)) {
    return false;
  }
  const float source_pixels[2][4] = {{0.25f, 0.5f, 0.125f, 1.0f}, {0.25f, 0.5f, 0.125f, 1.0f}};
  struct wlr_buffer* source = create_linear_buffer(source_pixels);
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  struct wlr_scene_buffer* original = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  struct wlr_scene_buffer* copy = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  bool ok = check(source != NULL && output != NULL && original != NULL && copy != NULL, "create custom color scene");
  if (!ok) {
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(output, false);
  wlr_scene_output_set_sdr_white_level(output, 203.0f);
  wlr_scene_buffer_set_dest_size(original, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_dest_size(copy, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_filter_mode(original, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_node_set_enabled(&copy->node, false);
  wlr_scene_buffer_set_transfer_function(original, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
  wlr_scene_buffer_set_primaries(original, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  uint8_t shown[2][4] = {0};
  // Initialize the backend's SDR swapchain before using pending HDR states.
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  // Independently rounded sRGB to BT.2020 primary conversion.
  const float baseline[3] = {
      (0.627404f * 0.25f + 0.329283f * 0.5f + 0.043313f * 0.125f) * 203.0f,
      (0.069097f * 0.25f + 0.919540f * 0.5f + 0.011362f * 0.125f) * 203.0f,
      (0.016392f * 0.25f + 0.088013f * 0.5f + 0.895595f * 0.125f) * 203.0f,
  };
  ok = check_pq_scene_pixel(shown[0], baseline, "named sRGB baseline") && ok;
  ok = check(!wlr_scene_output_needs_frame(output), "presented color scene has no pending frame") && ok;

  struct wlr_color_primaries bt2020;
  wlr_color_primaries_from_named(&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);
  wlr_scene_buffer_set_custom_primaries(original, &bt2020);
  ok = check(wlr_scene_output_needs_frame(output), "custom primary change damages a static scene") && ok;
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  const float custom_primaries_light[3] = {0.25f * 203.0f, 0.5f * 203.0f, 0.125f * 203.0f};
  ok = check_pq_scene_pixel(shown[0], custom_primaries_light, "custom BT.2020 overrides named sRGB") && ok;
  const struct wlr_color_luminances luminances = {.min = 0.0f, .max = 1000.0f, .reference = 203.0f};
  wlr_scene_buffer_set_luminances(original, &luminances, true);
  ok = check(wlr_scene_output_needs_frame(output), "custom luminance change damages a static scene") && ok;
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  const float custom_light[3] = {250.0f, 500.0f, 125.0f};
  ok = check_pq_scene_pixel(shown[0], custom_light, "custom luminances change light output") && ok;

  wlr_scene_buffer_set_luminance_multiplier(original, 0.75f);
  wlr_scene_buffer_copy_color(copy, original);
  wlr_scene_node_set_enabled(&original->node, false);
  wlr_scene_node_set_enabled(&copy->node, true);
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  const float copied_light[3] = {187.5f, 375.0f, 93.75f};
  ok = check_pq_scene_pixel(shown[0], copied_light, "copied primary, transfer, luminance and multiplier state") && ok;

  // A copied preview/snapshot owns its metadata independently of the source.
  wlr_scene_buffer_set_custom_primaries(original, NULL);
  wlr_scene_buffer_set_luminances(original, NULL, false);
  wlr_scene_buffer_set_luminance_multiplier(original, 1.0f);
  wlr_scene_output_set_sdr_white_level(output, 500.0f);
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  ok = check_pq_scene_pixel(shown[0], copied_light, "copy retains independent absolute luminance state") && ok;

  wlr_scene_node_set_enabled(&original->node, true);
  wlr_scene_node_set_enabled(&copy->node, false);
  wlr_scene_output_set_sdr_white_level(output, 203.0f);
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  ok = check_pq_scene_pixel(shown[0], baseline, "unsetting custom metadata restores named defaults") && ok;
  wlr_scene_buffer_copy_color(copy, original);
  wlr_scene_node_set_enabled(&original->node, false);
  wlr_scene_node_set_enabled(&copy->node, true);
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  ok = check_pq_scene_pixel(shown[0], baseline, "copying default state clears previous custom metadata") && ok;
out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static bool test_scene_scrgb_absolute_luminance(struct fixture* fixture) {
  if (!enable_color_scene_output(fixture)) {
    return false;
  }
  const float source_pixels[2][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {10.0f, 10.0f, 10.0f, 1.0f}};
  struct wlr_buffer* source = create_linear_buffer(source_pixels);
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  struct wlr_scene_buffer* content = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  bool ok = check(source != NULL && output != NULL && content != NULL, "create absolute scRGB scene");
  if (!ok) {
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(output, false);
  wlr_scene_buffer_set_dest_size(content, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_filter_mode(content, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
  wlr_scene_buffer_set_primaries(content, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  wlr_scene_buffer_set_luminance_multiplier(content, 80.0f / 203.0f);
  wlr_scene_buffer_set_luminances(content, NULL, true);
  uint8_t shown[2][4] = {0};
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  const float white_levels[] = {203.0f, 500.0f, 203.0f};
  for (size_t round = 0; round < sizeof(white_levels) / sizeof(white_levels[0]); round++) {
    wlr_scene_output_set_sdr_white_level(output, white_levels[round]);
    ok = render_metadata_scene(fixture, output, true, shown) && ok;
    const float reference[3] = {80.0f, 80.0f, 80.0f};
    const float highlight[3] = {800.0f, 800.0f, 800.0f};
    ok = check_pq_scene_pixel(shown[0], reference, "scRGB 1.0 remains 80 nits across output white transitions") && ok;
    ok = check_pq_scene_pixel(shown[1], highlight, "scRGB highlight remains 800 nits across output white transitions")
        && ok;
  }
out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static bool test_scene_hdr_sdr_fallback(struct fixture* fixture) {
  if (!enable_color_scene_output(fixture)) {
    return false;
  }
  const float scrgb_pixels[2][4] = {{4.0f, 4.0f, 4.0f, 1.0f}, {16.0f, 16.0f, 16.0f, 1.0f}};
  const float sdr_pixels[2][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}};
  const float pq_pixels[2][4] = {
      {linear_to_pq(320.0f / 10000.0f), linear_to_pq(320.0f / 10000.0f), linear_to_pq(320.0f / 10000.0f), 1.0f},
      {linear_to_pq(1280.0f / 10000.0f), linear_to_pq(1280.0f / 10000.0f), linear_to_pq(1280.0f / 10000.0f), 1.0f},
  };
  const float alpha_pixels[2][4] = {{2.0f, 1.0f, 0.5f, 0.5f}, {4.0f, 2.0f, 1.0f, 1.0f}};
  struct wlr_buffer* sources[] = {
      create_linear_buffer(scrgb_pixels),
      create_linear_buffer(sdr_pixels),
      create_linear_buffer(pq_pixels),
      create_linear_buffer(alpha_pixels),
  };
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  struct wlr_scene_rect* background =
      scene != NULL ? wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black) : NULL;
  struct wlr_scene_buffer* content =
      scene != NULL && sources[0] != NULL ? wlr_scene_buffer_create(&scene->tree, sources[0]) : NULL;
  bool ok = check(
      output != NULL
          && background != NULL
          && content != NULL
          && sources[1] != NULL
          && sources[2] != NULL
          && sources[3] != NULL,
      "create HDR to SDR fallback scene"
  );
  if (!ok) {
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(output, false);
  wlr_scene_output_set_sdr_white_level(output, 203.0f);
  wlr_scene_buffer_set_dest_size(content, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_filter_mode(content, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
  wlr_scene_buffer_set_primaries(content, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  wlr_scene_buffer_set_luminance_multiplier(content, 80.0f / 203.0f);
  wlr_scene_buffer_set_luminances(content, NULL, true);

  uint8_t fallback[2][4] = {0};
  ok = render_metadata_scene(fixture, output, false, fallback) && ok;
  for (size_t channel = 0; channel < 3; channel++) {
    ok = check(
             fallback[0][channel] >= 220 && fallback[0][channel] < fallback[1][channel] && fallback[1][channel] < 255,
             "SDR fallback retains distinct scRGB highlights instead of clipping both to white"
         )
        && ok;
  }

  // A pending HDR output state must bypass the SDR shoulder immediately, then
  // returning to SDR must restore the same mapping without a source repaint.
  uint8_t shown[2][4] = {0};
  ok = render_metadata_scene(fixture, output, true, shown) && ok;
  const float moderate[3] = {320.0f, 320.0f, 320.0f};
  const float brighter[3] = {1280.0f, 1280.0f, 1280.0f};
  ok = check_pq_scene_pixel(shown[0], moderate, "scRGB SDR to HDR transition retains absolute brightness") && ok;
  ok = check_pq_scene_pixel(shown[1], brighter, "HDR highlights bypass SDR compression") && ok;
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  ok = check(
           codes_close(shown[0], fallback[0], 1) && codes_close(shown[1], fallback[1], 1),
           "returning from HDR restores SDR highlight mapping"
       )
      && ok;

  // Explicit linear SDR remains unchanged, including peak white. Ordinary SDR
  // is not passed through an HDR shoulder merely because it uses linear data.
  wlr_scene_buffer_set_buffer(content, sources[1]);
  wlr_scene_buffer_set_luminance_multiplier(content, 1.0f);
  wlr_scene_buffer_set_luminances(content, NULL, false);
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  const uint8_t white[4] = {255, 255, 255, 255};
  const uint8_t grey[4] = {188, 188, 188, 255};
  ok = check(
           codes_close(shown[0], white, 1) && codes_close(shown[1], grey, 1),
           "linear SDR peak white and midtones are unchanged"
       )
      && ok;

  const struct wlr_color_luminances hdr_gamma = {.min = 0.0f, .max = 160.0f, .reference = 80.0f};
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  wlr_scene_buffer_set_luminances(content, &hdr_gamma, false);
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  ok = check(
           shown[0][0] > 235 && shown[0][0] < 255 && abs((int)shown[1][0] - 177) <= 1,
           "custom gamma HDR compresses highlights while preserving midtones below the knee"
       )
      && ok;

  wlr_scene_buffer_set_buffer(content, sources[2]);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
  wlr_scene_buffer_set_luminances(content, NULL, false);
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  ok = check(
           codes_close(shown[0], fallback[0], 1) && codes_close(shown[1], fallback[1], 1),
           "equivalent PQ and scRGB light receives the same SDR mapping"
       )
      && ok;

  wlr_scene_buffer_set_buffer(content, sources[3]);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
  wlr_scene_buffer_set_luminance_multiplier(content, 80.0f / 203.0f);
  wlr_scene_buffer_set_luminances(content, NULL, true);
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  for (size_t channel = 0; channel < 3; channel++) {
    ok = check(
             abs(2 * (int)shown[0][channel] - (int)shown[1][channel]) <= 3,
             "HDR mapping unpremultiplies before compression and restores alpha before blending"
         )
        && ok;
  }
out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
    if (sources[i] != NULL) {
      wlr_buffer_drop(sources[i]);
    }
  }
  return ok;
}

static bool test_scene_custom_white(struct fixture* fixture) {
  if (!enable_color_scene_output(fixture)) {
    return false;
  }
  const float source_pixels[2][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {0.25f, 0.25f, 0.25f, 1.0f}};
  struct wlr_buffer* source = create_linear_buffer(source_pixels);
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  struct wlr_scene_buffer* content = scene != NULL ? wlr_scene_buffer_create(&scene->tree, source) : NULL;
  bool ok = check(source != NULL && output != NULL && content != NULL, "create custom white scene");
  if (!ok) {
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(output, false);
  wlr_scene_output_set_sdr_white_level(output, 203.0f);
  wlr_scene_buffer_set_dest_size(content, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_filter_mode(content, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
  wlr_scene_buffer_set_primaries(content, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  uint8_t shown[2][4] = {0};
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  struct wlr_color_primaries primaries;
  wlr_color_primaries_from_named(&primaries, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  for (size_t round = 0; round < 3; round++) {
    if (round == 1) {
      primaries.white.x = 0.3457f;
      primaries.white.y = 0.3585f;
      wlr_scene_buffer_set_custom_primaries(content, &primaries);
      ok = check(wlr_scene_output_needs_frame(output), "custom D50 white damages the scene") && ok;
    } else if (round == 2) {
      wlr_scene_buffer_set_custom_primaries(content, NULL);
      ok = check(wlr_scene_output_needs_frame(output), "restoring D65 white damages the scene") && ok;
    }
    ok = render_metadata_scene(fixture, output, true, shown) && ok;
    const float white[3] = {203.0f, 203.0f, 203.0f};
    const float grey[3] = {50.75f, 50.75f, 50.75f};
    ok = check_pq_scene_pixel(shown[0], white, "custom white adapts reference white to neutral D65") && ok;
    ok = check_pq_scene_pixel(shown[1], grey, "custom white adapts grey to neutral D65") && ok;
  }
out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  if (source != NULL) {
    wlr_buffer_drop(source);
  }
  return ok;
}

static bool test_scene_custom_bt1886(struct fixture* fixture) {
  if (!enable_color_scene_output(fixture)) {
    return false;
  }
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* output = scene != NULL ? wlr_scene_output_create(scene, fixture->output) : NULL;
  struct wlr_scene_buffer* content = scene != NULL ? wlr_scene_buffer_create(&scene->tree, NULL) : NULL;
  bool ok = check(output != NULL && content != NULL, "create custom BT.1886 scene");
  if (!ok) {
    goto out;
  }
  wlr_scene_output_set_direct_scanout_enabled(output, false);
  wlr_scene_output_set_sdr_white_level(output, 203.0f);
  wlr_scene_buffer_set_dest_size(content, TEST_WIDTH, TEST_HEIGHT);
  wlr_scene_buffer_set_filter_mode(content, WLR_SCALE_FILTER_NEAREST);
  wlr_scene_buffer_set_transfer_function(content, WLR_COLOR_TRANSFER_FUNCTION_BT1886);
  wlr_scene_buffer_set_primaries(content, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  uint8_t shown[2][4] = {0};
  ok = render_metadata_scene(fixture, output, false, shown) && ok;
  const struct wlr_color_luminances luminances = {.min = 1.0f, .max = 100.0f, .reference = 80.0f};
  const float encoded[2][2] = {{0.0f, 0.5f}, {0.895941f, 1.0f}};
  // BT.1886 with 1-nit black, 100-nit peak and 80-nit reference, after
  // black-point compensation. The reference lies below the encoded peak.
  const float custom_light[2][2] = {{0.0f, 0.3205019744f}, {1.0f, 1.25316455696f}};
  for (size_t pair = 0; pair < 2; pair++) {
    float pixels[2][4];
    for (size_t sample = 0; sample < 2; sample++) {
      for (size_t channel = 0; channel < 3; channel++) {
        pixels[sample][channel] = encoded[pair][sample];
      }
      pixels[sample][3] = 1.0f;
    }
    struct wlr_buffer* source = create_linear_buffer(pixels);
    if (!check(source != NULL, "allocate BT.1886 source pixels")) {
      ok = false;
      break;
    }
    wlr_scene_buffer_set_buffer(content, source);
    wlr_buffer_drop(source);
    wlr_scene_buffer_set_luminances(content, NULL, false);
    ok = render_metadata_scene(fixture, output, true, shown) && ok;
    uint8_t defaults[2][4];
    memcpy(defaults, shown, sizeof(defaults));
    wlr_scene_buffer_set_luminances(content, &luminances, false);
    ok = check(wlr_scene_output_needs_frame(output), "custom BT.1886 luminances damage a static scene") && ok;
    ok = render_metadata_scene(fixture, output, true, shown) && ok;
    for (size_t sample = 0; sample < 2; sample++) {
      const float light = custom_light[pair][sample] * 203.0f;
      const float nits[3] = {light, light, light};
      ok = check_pq_scene_pixel(shown[sample], nits, "custom BT.1886 curve and reference after black compensation")
          && ok;
    }
    wlr_scene_buffer_set_luminances(content, NULL, false);
    ok = check(wlr_scene_output_needs_frame(output), "reset BT.1886 luminances damage a static scene") && ok;
    ok = render_metadata_scene(fixture, output, true, shown) && ok;
    ok = check(memcmp(defaults, shown, sizeof(defaults)) == 0, "clearing custom BT.1886 restores the default curve")
        && ok;
  }
out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  return ok;
}

static bool test_scrgb_negative_channels(struct fixture* fixture) {
  uint32_t format = select_10bit_format(fixture);
  if (!check(format != DRM_FORMAT_INVALID, "find scRGB 10-bit output format")) {
    return false;
  }
  struct wlr_color_primaries srgb;
  wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
  struct wlr_color_transform* transform = create_output_transform(
      WLR_COLOR_NAMED_PRIMARIES_BT2020, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f
  );
  if (!check(transform != NULL, "create scRGB output transform")) {
    return false;
  }
  bool ok = true;
  const float multiplier = 80.0f / 203.0f;
  const float levels[] = {1.0f, 10.0f};
  for (size_t sample = 0; sample < sizeof(levels) / sizeof(levels[0]); sample++) {
    // Independently rounded BT.2020 green in linear sRGB. Both negative
    // channels and the green channel above one must survive until conversion.
    const uint16_t input[4] = {
        float_to_half(-0.587641f * levels[sample]),
        float_to_half(1.132900f * levels[sample]),
        float_to_half(-0.100579f * levels[sample]),
        float_to_half(1.0f),
    };
    uint32_t pixel = 0;
    ok = render_texture(
             fixture, 1, 1, format, DRM_FORMAT_ABGR16161616F, sizeof(input), input,
             WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, &srgb, &multiplier, transform, NULL, false, format, sizeof(pixel),
             &pixel
         )
        && ok;
    const int red = pixel & 1023;
    const int green = (pixel >> 10) & 1023;
    const int blue = (pixel >> 20) & 1023;
    const int expected = lroundf(linear_to_pq(levels[sample] * 80.0f / 10000.0f) * 1023.0f);
    // FP16 quantization leaves tiny residual primary components. Pre-clamping
    // the negative input instead produces hundreds of PQ codes in red/blue.
    if (red > 45 || blue > 45 || abs(green - expected) > 3) {
      fprintf(
          stderr, "FAIL: scRGB green %.0f: PQ RGB %d %d %d, expected 0 %d 0\n", levels[sample], red, green, blue,
          expected
      );
      ok = false;
    }
  }
  wlr_color_transform_unref(transform);
  return ok;
}

static bool test_fp16(struct fixture* fixture, bool blur) {
  uint32_t format = select_10bit_format(fixture);
  if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
    return false;
  }
  const uint16_t input[4] = {
      float_to_half(1.5f),
      float_to_half(1.5f),
      float_to_half(1.5f),
      float_to_half(0.5f),
  };
  uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
  struct wlr_color_transform* transform =
      create_output_transform(WLR_COLOR_NAMED_PRIMARIES_SRGB, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
  struct blur_data blur_options = {
      .num_passes = 1,
      .radius = 1.0f,
      .brightness = 1.0f,
      .contrast = 1.0f,
      .saturation = 1.0f,
  };
  bool ok = check(transform != NULL, "create FP16 output transform")
      && render_texture(
                fixture, TEST_WIDTH, TEST_HEIGHT, format, DRM_FORMAT_ABGR16161616F, sizeof(input), input,
                WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL, transform, blur ? &blur_options : NULL, false,
                format, TEST_WIDTH * sizeof(uint32_t), output
      );
  wlr_color_transform_unref(transform);
  if (!ok) {
    return false;
  }

  uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
  int actual = pixel & 0x3FF;
  int expected = lroundf(linear_to_pq(1.5f * 203.0f / 10000.0f) * 1023.0f);
  int sdr_white = lroundf(linear_to_pq(203.0f / 10000.0f) * 1023.0f);
  int tolerance = blur ? 6 : 2;
  return check(
             abs(actual - expected) <= tolerance,
             blur ? "blur preserves FP16 luminance" : "FP16 blend preserves luminance"
         )
      && check(actual > sdr_white + 2, "value remains above SDR white");
}

static bool test_fp16_blur_effects(struct fixture* fixture) {
  uint32_t format = select_10bit_format(fixture);
  if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
    return false;
  }
  const float input_linear = 0.05f;
  const uint16_t input[4] = {
      float_to_half(input_linear),
      float_to_half(input_linear),
      float_to_half(input_linear),
      float_to_half(1.0f),
  };
  uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
  struct wlr_color_transform* transform =
      create_output_transform(WLR_COLOR_NAMED_PRIMARIES_SRGB, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
  struct blur_data blur_options = {
      .num_passes = 1,
      .radius = 1.0f,
      .brightness = 0.9f,
      .contrast = 0.9f,
      .saturation = 1.1f,
  };
  bool ok = check(transform != NULL, "create FP16 blur-effects output transform")
      && render_texture(
                fixture, TEST_WIDTH, TEST_HEIGHT, format, DRM_FORMAT_ABGR16161616F, sizeof(input), input,
                WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL, transform, &blur_options, false, format,
                TEST_WIDTH * sizeof(uint32_t), output
      );
  wlr_color_transform_unref(transform);
  if (!ok) {
    return false;
  }

  // Blur controls act on values encoded with the pass SDR curve, gamma 2.2 on
  // a PQ target, so they look the same as over the SDR content on SDR.
  float encoded = powf(input_linear, 1.0f / 2.2f);
  encoded = (encoded * 0.9f + 0.05f) - 0.1f;
  float expected_linear = powf(encoded, 2.2f);
  int expected = lroundf(linear_to_pq(expected_linear * 203.0f / 10000.0f) * 1023.0f);
  uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
  int actual = pixel & 0x3FF;
  return check(abs(actual - expected) <= 6, "FP16 blur effects preserve gamma-space brightness");
}

// The blur-artifact compensation copies the pass target into a scratch buffer
// and back. In two-pass mode the pass target holds linear light, so the copy
// must be lossless: any transfer-function conversion darkens the padding
// region around every blur node.
static bool test_fp16_save_restore(struct fixture* fixture) {
  uint32_t format = select_10bit_format(fixture);
  if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
    return false;
  }
  const float input_linear = 0.2f;
  const uint16_t input[4] = {
      float_to_half(input_linear),
      float_to_half(input_linear),
      float_to_half(input_linear),
      float_to_half(1.0f),
  };
  uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
  struct wlr_color_transform* transform =
      create_output_transform(WLR_COLOR_NAMED_PRIMARIES_SRGB, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
  bool ok = check(transform != NULL, "create FP16 save/restore output transform")
      && render_texture(
                fixture, TEST_WIDTH, TEST_HEIGHT, format, DRM_FORMAT_ABGR16161616F, sizeof(input), input,
                WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL, transform, NULL, true, format,
                TEST_WIDTH * sizeof(uint32_t), output
      );
  wlr_color_transform_unref(transform);
  if (!ok) {
    return false;
  }

  int expected = lroundf(linear_to_pq(input_linear * 203.0f / 10000.0f) * 1023.0f);
  uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
  int actual = pixel & 0x3FF;
  return check(abs(actual - expected) <= 2, "FP16 save/restore round trip preserves luminance");
}

// An untransformed 10-bit pass must still use FP16 effect buffers for blur when
// half-float textures can be linearly filtered. Pixel averages alone cannot
// distinguish an 8-bit intermediate from the intended FP16 intermediate.
static bool test_sdr10_blur_buffer_format(struct fixture* fixture) {
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_XRGB2101010, TEST_WIDTH, TEST_HEIGHT);
  // Some GBM drivers advertise XR30 but cannot allocate it; XBGR2101010
  // exercises the same untransformed 10-bit effect-buffer path.
  if (target == NULL) {
    target = create_output_buffer(fixture, DRM_FORMAT_XBGR2101010, TEST_WIDTH, TEST_HEIGHT);
  }
  if (!check(target != NULL, "allocate 10-bit blur target")) {
    return false;
  }

  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, NULL);
  if (!check(pass != NULL, "begin untransformed 10-bit blur pass")) {
    wlr_buffer_drop(target);
    return false;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct wlr_box box = {.width = TEST_WIDTH, .height = TEST_HEIGHT};
  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, TEST_WIDTH, TEST_HEIGHT);
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = box,
          .color = {.r = 0.4f, .g = 0.25f, .b = 0.5f, .a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );

  bool ok = check(!fx_pass->has_color_transform, "10-bit blur pass has no color transform")
      && check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "initialize 10-bit blur buffers");
  if (ok) {
    const float opacity = 1.0f;
    struct blur_data blur_data = {
        .num_passes = 1,
        .radius = 1.0f,
        .brightness = 1.0f,
        .contrast = 1.0f,
        .saturation = 1.0f,
    };
    struct fx_render_blur_pass_options blur_options = {
        .tex_options =
            {
                .base =
                    {
                        .dst_box = box,
                        .clip = &region,
                        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                        .alpha = &opacity,
                    },
                .clip_box = &box,
            },
        .blur_data = &blur_data,
        .blur_strength = 1.0f,
    };
    fx_render_pass_add_blur(fx_pass, &blur_options);

    struct fx_offscreen_buffers* fbos = fx_pass->fx_offscreen_buffers;
    uint32_t expected =
        fx_get_renderer(fixture->renderer)->exts.half_float_linear ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR8888;
    ok = check(
             fbos != NULL && fbos->output.effects_buffer != NULL && fbos->output.effects_buffer_swapped != NULL,
             "10-bit blur allocates both effect buffers"
         )
        && ok;
    if (fbos != NULL && fbos->output.effects_buffer != NULL && fbos->output.effects_buffer_swapped != NULL) {
      ok = check(
               fbos->output.effects_buffer->drm_format == expected
                   && fbos->output.effects_buffer_swapped->drm_format == expected,
               "untransformed 10-bit blur uses the expected effect format"
           )
          && ok;
    }
  }
  pixman_region32_fini(&region);
  ok = check(wlr_render_pass_submit(pass), "submit 10-bit blur pass") && ok;
  wlr_buffer_drop(target);
  return ok;
}

static bool test_shared_output_buffers(struct fixture* fixture) {
  bool ok = true;
  struct wlr_buffer* target_a = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_b = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  if (!check(target_a != NULL && target_b != NULL, "allocate shared-output targets")
      || !check(transform != NULL, "create shared-output transform")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options options = {
      .color_transform = transform,
  };
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &options);
  if (!check(pass != NULL, "begin first shared-output pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct fx_offscreen_buffers* output_buffers = fx_pass->output_buffers;
  struct fx_framebuffer* shared_blend = fx_pass->buffer;
  ok = check(output_buffers != NULL, "first output pass has output-local buffers") && ok;
  ok = check(fx_pass->needs_full_damage, "first shared blend pass requests full damage") && ok;
  if (output_buffers != NULL) {
    ok = check(output_buffers->blend_buffer == shared_blend, "first output pass targets the shared blend buffer") && ok;
  }
  bool submitted = submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f);
  ok = check(submitted, "submit first shared-output pass") && ok;
  if (!submitted) {
    goto out;
  }
  if (output_buffers != NULL) {
    ok = check(output_buffers->blend_valid, "first full frame validates the shared blend buffer") && ok;
  }

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_b, &options);
  if (!check(pass != NULL, "begin second shared-output pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  ok = check(fx_pass->output_buffers == output_buffers, "second target reuses the output-local buffer set") && ok;
  ok = check(fx_pass->buffer == shared_blend, "second target reuses the shared FP16 blend buffer") && ok;
  ok = check(!fx_pass->needs_full_damage, "valid shared blend skips repeated full damage") && ok;
  submitted = submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.4f);
  ok = check(submitted, "submit second shared-output pass") && ok;
  if (!submitted) {
    goto out;
  }

  pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target_a, &options);
  if (!check(pass != NULL, "begin generic transformed pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  ok = check(fx_pass->output_buffers == NULL, "generic pass has no output-local buffers") && ok;
  ok = check(!fx_pass->needs_full_damage, "generic pass does not request output damage") && ok;
  ok = check(fx_pass->buffer != shared_blend, "generic pass does not borrow the output blend buffer") && ok;
  ok = check(
           fx_pass->output_buffer->blend_buffer == fx_pass->buffer, "generic pass keeps its blend buffer on the target"
       )
      && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.6f), "submit generic transformed pass") && ok;

out:
  wlr_color_transform_unref(transform);
  if (target_a != NULL) {
    wlr_buffer_drop(target_a);
  }
  if (target_b != NULL) {
    wlr_buffer_drop(target_b);
  }
  return ok;
}

static bool test_output_lut_cache(struct fixture* fixture) {
  bool ok = true;
  const uint16_t ramp_a[2] = {0, UINT16_MAX};
  const uint16_t ramp_b[2] = {UINT16_MAX / 4, UINT16_MAX * 3 / 4};
  struct wlr_color_transform* transform_a = wlr_color_transform_init_lut_3x1d(2, ramp_a, ramp_a, ramp_a);
  struct wlr_color_transform* transform_b = wlr_color_transform_init_lut_3x1d(2, ramp_b, ramp_b, ramp_b);
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(transform_a != NULL && transform_b != NULL, "create cached LUT transforms")
      || !check(target != NULL, "allocate LUT target")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options options_a = {
      .color_transform = transform_a,
  };
  const struct wlr_buffer_pass_options options_b = {
      .color_transform = transform_b,
  };
  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, &options_a);
  if (!check(pass != NULL, "begin LUT A pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  GLuint texture_a = fx_pass->output_lut;
  ok = check(texture_a != 0, "LUT A uploads a texture") && ok;
  ok = check(fx_pass->color_transform == transform_a, "LUT A pass retains its transform") && ok;
  ok = check(transform_a->ref_count == 2, "LUT A transform is referenced for the pass lifetime") && ok;
  ok = check(output_lut_count(fixture) == 1, "renderer caches one LUT after LUT A begin") && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f), "submit LUT A pass") && ok;
  ok = check(transform_a->ref_count == 1, "LUT A pass releases its transform reference") && ok;

  pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, &options_b);
  if (!check(pass != NULL, "begin LUT B pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  GLuint texture_b = fx_pass->output_lut;
  ok = check(texture_b != 0, "LUT B uploads a texture") && ok;
  ok = check(texture_b != texture_a, "LUT A and LUT B keep distinct live texture names") && ok;
  ok = check(fx_pass->color_transform == transform_b, "LUT B pass retains its transform") && ok;
  ok = check(transform_b->ref_count == 2, "LUT B transform is referenced for the pass lifetime") && ok;
  ok = check(output_lut_count(fixture) == 2, "renderer caches both LUT textures") && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.4f), "submit LUT B pass") && ok;
  ok = check(transform_b->ref_count == 1, "LUT B pass releases its transform reference") && ok;

  pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, &options_a);
  if (!check(pass != NULL, "begin second LUT A pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  ok = check(fx_pass->output_lut == texture_a, "A/B/A returns to the original LUT A texture") && ok;
  ok = check(fx_pass->output_lut != texture_b, "A/B/A does not reuse LUT B for LUT A") && ok;
  ok = check(output_lut_count(fixture) == 2, "LUT A reuse does not allocate another cache entry") && ok;
  ok = check(transform_a->ref_count == 2, "reused LUT A transform is referenced for the pass lifetime") && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.6f), "submit reused LUT A pass") && ok;
  ok = check(transform_a->ref_count == 1, "reused LUT A pass releases its transform reference") && ok;

  wlr_color_transform_unref(transform_a);
  transform_a = NULL;
  ok = check(output_lut_count(fixture) == 1, "destroying LUT A removes only its cache entry") && ok;
  wlr_color_transform_unref(transform_b);
  transform_b = NULL;
  ok = check(output_lut_count(fixture) == 0, "destroying LUT B empties the renderer LUT cache") && ok;

out:
  wlr_color_transform_unref(transform_a);
  wlr_color_transform_unref(transform_b);
  if (target != NULL) {
    wlr_buffer_drop(target);
  }
  return ok;
}

static bool test_shared_sdr_capture(struct fixture* fixture) {
  bool ok = true;
  struct wlr_buffer* target_a = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_b = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  if (!check(target_a != NULL && target_b != NULL, "allocate SDR capture targets")
      || !check(transform != NULL, "create SDR capture transform")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options options = {
      .color_transform = transform,
  };
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &options);
  if (!check(pass != NULL, "begin first SDR capture pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct fx_offscreen_buffers* output_buffers = fx_pass->output_buffers;
  struct fx_framebuffer* target_a_fb = fx_pass->output_buffer;
  ok = check(output_buffers != NULL, "first capture pass has output-local buffers") && ok;
  fx_pass->output_buffer->capture_sdr = true;
  bool submitted = submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f);
  ok = check(submitted, "submit first SDR capture pass") && ok;
  if (!submitted || output_buffers == NULL) {
    goto out;
  }

  struct wlr_texture* capture_texture = wlr_texture_from_buffer(fixture->renderer, target_a);
  ok = check(capture_texture != NULL, "first target exposes an SDR capture texture") && ok;
  if (capture_texture != NULL) {
    wlr_texture_destroy(capture_texture);
  }
  struct fx_framebuffer* shared_capture = output_buffers->sdr_capture_buffer;
  uint64_t first_generation = output_buffers->sdr_capture_generation;
  ok = check(shared_capture != NULL, "first capture allocates the output SDR sidecar") && ok;
  ok = check(
           first_generation != 0 && first_generation == output_buffers->blend_generation,
           "first capture records the current blend generation"
       )
      && ok;
  ok = check(target_a_fb->sdr_capture_buffer == NULL, "first target does not own a private capture sidecar") && ok;
  if (shared_capture != NULL) {
    ok = check(
             shared_capture->drm_format == target_a_fb->drm_format, "shared capture sidecar preserves the output format"
         )
        && ok;
  }

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_b, &options);
  if (!check(pass != NULL, "begin second SDR capture pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  struct fx_framebuffer* target_b_fb = fx_pass->output_buffer;
  ok = check(fx_pass->output_buffers == output_buffers, "second capture target reuses the output buffer set") && ok;
  fx_pass->output_buffer->capture_sdr = true;
  submitted = submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.6f);
  ok = check(submitted, "submit second SDR capture pass") && ok;
  if (!submitted) {
    goto out;
  }
  ok = check(
           output_buffers->blend_generation != first_generation, "second rendered target advances the blend generation"
       )
      && ok;
  ok = check(
           output_buffers->sdr_capture_generation == first_generation,
           "old SDR sidecar remains tagged with its previous generation"
       )
      && ok;

  capture_texture = wlr_texture_from_buffer(fixture->renderer, target_b);
  ok = check(capture_texture != NULL, "second target refreshes the SDR capture texture") && ok;
  if (capture_texture != NULL) {
    wlr_texture_destroy(capture_texture);
  }
  ok =
      check(output_buffers->sdr_capture_buffer == shared_capture, "second target reuses the shared SDR capture sidecar")
      && ok;
  ok = check(
           output_buffers->sdr_capture_generation == output_buffers->blend_generation,
           "refreshed sidecar records the new blend generation"
       )
      && ok;
  ok = check(target_b_fb->sdr_capture_buffer == NULL, "second target does not own a private capture sidecar") && ok;

out:
  wlr_color_transform_unref(transform);
  if (target_a != NULL) {
    wlr_buffer_drop(target_a);
  }
  if (target_b != NULL) {
    wlr_buffer_drop(target_b);
  }
  return ok;
}

static bool test_shared_output_transform_transition(struct fixture* fixture) {
  bool ok = true;
  struct wlr_buffer* target_a = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_b = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  if (!check(target_a != NULL && target_b != NULL, "allocate transform-transition targets")
      || !check(transform != NULL, "create transform-transition output transform")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options transformed_options = {
      .color_transform = transform,
  };
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &transformed_options);
  if (!check(pass != NULL, "begin initial transformed output pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct fx_offscreen_buffers* output_buffers = fx_pass->output_buffers;
  ok = check(output_buffers != NULL, "initial transformed pass has output-local buffers") && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f), "submit initial transformed output pass") && ok;
  if (output_buffers == NULL) {
    goto out;
  }
  ok = check(output_buffers->blend_valid, "initial transformed pass validates the shared blend") && ok;

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_b, NULL);
  if (!check(pass != NULL, "begin intervening untransformed output pass")) {
    ok = false;
    goto out;
  }
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.6f), "submit intervening untransformed output pass")
      && ok;
  ok = check(!output_buffers->blend_valid, "untransformed output pass invalidates the shared blend") && ok;

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &transformed_options);
  if (!check(pass != NULL, "begin transformed output pass after SDR")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  ok = check(fx_pass->output_buffers == output_buffers, "transformed output pass returns to the same output buffers")
      && ok;
  ok = check(fx_pass->needs_full_damage, "first transformed pass after SDR requests full damage") && ok;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.4f), "submit transformed output pass after SDR") && ok;

out:
  wlr_color_transform_unref(transform);
  if (target_a != NULL) {
    wlr_buffer_drop(target_a);
  }
  if (target_b != NULL) {
    wlr_buffer_drop(target_b);
  }
  return ok;
}

static bool test_shared_sdr_capture_lock(struct fixture* fixture) {
  bool ok = true;
  struct wlr_buffer* target_a = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_b = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  struct wlr_texture* held_capture = NULL;
  struct wlr_texture* blocked_capture = NULL;
  struct wlr_texture* refreshed_capture = NULL;
  if (!check(target_a != NULL && target_b != NULL, "allocate capture-lock targets")
      || !check(transform != NULL, "create capture-lock output transform")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options options = {
      .color_transform = transform,
  };
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &options);
  if (!check(pass != NULL, "begin first capture-lock pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct fx_offscreen_buffers* output_buffers = fx_pass->output_buffers;
  fx_pass->output_buffer->capture_sdr = true;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f), "submit first capture-lock pass") && ok;
  if (output_buffers == NULL) {
    ok = check(false, "capture-lock pass has output-local buffers") && ok;
    goto out;
  }

  held_capture = wlr_texture_from_buffer(fixture->renderer, target_a);
  ok = check(held_capture != NULL, "create held SDR capture texture") && ok;
  struct fx_framebuffer* shared_capture = output_buffers->sdr_capture_buffer;
  if (held_capture == NULL || shared_capture == NULL) {
    ok = check(false, "held texture has a shared capture sidecar") && ok;
    goto out;
  }
  ok = check(shared_capture->buffer->n_locks > 0, "held capture texture locks the shared sidecar") && ok;

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_b, &options);
  if (!check(pass != NULL, "begin second capture-lock pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  fx_pass->output_buffer->capture_sdr = true;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.6f), "submit second capture-lock pass") && ok;

  blocked_capture = wlr_texture_from_buffer(fixture->renderer, target_b);
  ok = check(blocked_capture == NULL, "locked SDR sidecar is not overwritten for a newer generation") && ok;
  if (blocked_capture != NULL) {
    wlr_texture_destroy(blocked_capture);
    blocked_capture = NULL;
  }
  wlr_texture_destroy(held_capture);
  held_capture = NULL;

  refreshed_capture = wlr_texture_from_buffer(fixture->renderer, target_b);
  ok = check(refreshed_capture != NULL, "SDR capture refreshes after the prior consumer unlocks") && ok;
  ok = check(output_buffers->sdr_capture_buffer == shared_capture, "capture refresh reuses the unlocked shared sidecar")
      && ok;

out:
  if (refreshed_capture != NULL) {
    wlr_texture_destroy(refreshed_capture);
  }
  if (blocked_capture != NULL) {
    wlr_texture_destroy(blocked_capture);
  }
  if (held_capture != NULL) {
    wlr_texture_destroy(held_capture);
  }
  wlr_color_transform_unref(transform);
  if (target_a != NULL) {
    wlr_buffer_drop(target_a);
  }
  if (target_b != NULL) {
    wlr_buffer_drop(target_b);
  }
  return ok;
}

static bool test_shared_sdr_capture_storage_change(struct fixture* fixture) {
  bool ok = true;
  uint32_t alternate_format = select_10bit_format(fixture);
  struct wlr_buffer* target_a = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_b = alternate_format == DRM_FORMAT_INVALID
      ? NULL
      : create_output_buffer(fixture, alternate_format, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* target_resized = alternate_format == DRM_FORMAT_INVALID
      ? NULL
      : create_output_buffer(fixture, alternate_format, TEST_WIDTH + 1, TEST_HEIGHT + 1);
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
  struct wlr_texture* capture_texture = NULL;
  if (!check(alternate_format != DRM_FORMAT_INVALID, "find alternate capture format")
      || !check(target_a != NULL && target_b != NULL && target_resized != NULL, "allocate capture-storage targets")
      || !check(transform != NULL, "create capture-storage output transform")) {
    ok = false;
    goto out;
  }

  const struct wlr_buffer_pass_options options = {
      .color_transform = transform,
  };
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_a, &options);
  if (!check(pass != NULL, "begin initial capture-storage pass")) {
    ok = false;
    goto out;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct fx_offscreen_buffers* output_buffers = fx_pass->output_buffers;
  fx_pass->output_buffer->capture_sdr = true;
  ok = check(submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 0.2f), "submit initial capture-storage pass") && ok;
  if (output_buffers == NULL) {
    ok = check(false, "capture-storage pass has output-local buffers") && ok;
    goto out;
  }
  capture_texture = wlr_texture_from_buffer(fixture->renderer, target_a);
  ok = check(capture_texture != NULL, "create initial capture-storage texture") && ok;
  if (capture_texture == NULL) {
    goto out;
  }
  wlr_texture_destroy(capture_texture);
  capture_texture = NULL;
  uint64_t unchanged_generation = output_buffers->blend_generation;

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_b, &options);
  if (!check(pass != NULL, "begin alternate-format output pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  fx_pass->output_buffer->capture_sdr = true;
  ok = check(wlr_render_pass_submit(pass), "submit unchanged alternate-format output pass") && ok;
  ok = check(output_buffers->blend_generation == unchanged_generation, "unchanged frame preserves the blend generation")
      && ok;

  capture_texture = wlr_texture_from_buffer(fixture->renderer, target_b);
  ok = check(capture_texture != NULL, "create alternate-format capture texture") && ok;
  if (capture_texture != NULL) {
    struct fx_framebuffer* capture_buffer = output_buffers->sdr_capture_buffer;
    ok = check(
             capture_buffer != NULL && capture_buffer->drm_format == alternate_format,
             "generation fast path validates the capture format"
         )
        && ok;
    ok = check(
             capture_buffer != NULL
                 && capture_buffer->buffer->width == TEST_WIDTH
                 && capture_buffer->buffer->height == TEST_HEIGHT,
             "generation fast path validates the capture dimensions"
         )
        && ok;
    wlr_texture_destroy(capture_texture);
    capture_texture = NULL;
  }

  pass = fx_renderer_begin_output_buffer_pass(fixture->output, target_resized, &options);
  if (!check(pass != NULL, "begin resized capture-storage pass")) {
    ok = false;
    goto out;
  }
  fx_pass = fx_get_render_pass(pass);
  ok = check(fx_pass->needs_full_damage, "resizing shared blend requests full damage") && ok;
  fx_pass->output_buffer->capture_sdr = true;
  ok = check(submit_solid_frame(pass, TEST_WIDTH + 1, TEST_HEIGHT + 1, 0.4f), "submit resized capture-storage pass")
      && ok;
  capture_texture = wlr_texture_from_buffer(fixture->renderer, target_resized);
  ok = check(capture_texture != NULL, "create resized capture texture") && ok;
  if (capture_texture != NULL) {
    struct fx_framebuffer* capture_buffer = output_buffers->sdr_capture_buffer;
    ok = check(
             capture_buffer != NULL
                 && capture_buffer->drm_format == alternate_format
                 && capture_buffer->buffer->width == TEST_WIDTH + 1
                 && capture_buffer->buffer->height == TEST_HEIGHT + 1,
             "resized capture sidecar matches target storage"
         )
        && ok;
  }

out:
  if (capture_texture != NULL) {
    wlr_texture_destroy(capture_texture);
  }
  wlr_color_transform_unref(transform);
  if (target_a != NULL) {
    wlr_buffer_drop(target_a);
  }
  if (target_b != NULL) {
    wlr_buffer_drop(target_b);
  }
  if (target_resized != NULL) {
    wlr_buffer_drop(target_resized);
  }
  return ok;
}

// Capture publishes the texture's preferred read format as its only SHM format.
// Every 8-bit target and packed 10-bit target must publish an 8-bit format in
// the BGRA byte order when the driver can read it. The published format must
// also read back in its own byte order.
static bool test_capture_read_format(struct fixture* fixture) {
  const uint32_t targets[] = {
      DRM_FORMAT_XRGB8888,    DRM_FORMAT_ARGB8888,    DRM_FORMAT_XBGR8888,    DRM_FORMAT_ABGR8888,
      DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010,
  };
  bool bgra = fx_get_renderer(fixture->renderer)->exts.EXT_read_format_bgra;
  bool ok = true;
  bool tested_10bit = false;
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    char message[128];
    struct wlr_buffer* target = create_output_buffer(fixture, targets[i], TEST_WIDTH, TEST_HEIGHT);
    if (target == NULL) {
      snprintf(message, sizeof(message), "allocate 0x%08X target", targets[i]);
      ok = check(targets[i] != DRM_FORMAT_XRGB8888, message) && ok;
      continue;
    }
    if (targets[i] == DRM_FORMAT_XRGB2101010
        || targets[i] == DRM_FORMAT_ARGB2101010
        || targets[i] == DRM_FORMAT_XBGR2101010
        || targets[i] == DRM_FORMAT_ABGR2101010) {
      tested_10bit = true;
    }

    struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, NULL);
    struct wlr_texture* texture = NULL;
    if (pass == NULL
        || !submit_solid_frame(pass, TEST_WIDTH, TEST_HEIGHT, 1.0f)
        || (texture = wlr_texture_from_buffer(fixture->renderer, target)) == NULL) {
      snprintf(message, sizeof(message), "render and wrap 0x%08X target", targets[i]);
      ok = check(false, message) && ok;
      wlr_buffer_drop(target);
      continue;
    }

    bool alpha = targets[i] == DRM_FORMAT_ARGB8888
        || targets[i] == DRM_FORMAT_ABGR8888
        || targets[i] == DRM_FORMAT_ARGB2101010
        || targets[i] == DRM_FORMAT_ABGR2101010;
    uint32_t expected = bgra ? (alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888)
                             : (alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888);
    uint32_t published = wlr_texture_preferred_read_format(texture);
    snprintf(
        message, sizeof(message), "0x%08X target publishes 0x%08X, expected 0x%08X", targets[i], published, expected
    );
    ok = check(published == expected, message) && ok;

    uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4] = {0};
    bool read = wlr_texture_read_pixels(
        texture,
        &(struct wlr_texture_read_pixels_options){
            .data = pixels,
            .format = expected,
            .stride = TEST_WIDTH * 4,
        }
    );
    const uint8_t bgra_red[4] = {128, 64, 255, 255};
    const uint8_t rgba_red[4] = {255, 64, 128, 255};
    bool bgra_order = expected == DRM_FORMAT_XRGB8888 || expected == DRM_FORMAT_ARGB8888;
    uint8_t actual[4] = {pixels[0], pixels[1], pixels[2], 255};
    snprintf(message, sizeof(message), "0x%08X target reads back as 0x%08X in its byte order", targets[i], expected);
    ok = check(read && codes_close(actual, bgra_order ? bgra_red : rgba_red, 1), message) && ok;

    wlr_texture_destroy(texture);
    wlr_buffer_drop(target);
  }
  ok = check(tested_10bit, "allocate packed 10-bit capture target") && ok;
  return ok;
}

// Populates the output's optimized-blur cache with a solid backdrop by running
// an optimized-blur pass into an 8-bit SDR target. The cache buffers are
// output-local, so they survive into a later pass on the same output. Returns
// the DRM format the cache buffers were allocated with, or DRM_FORMAT_INVALID.
static uint32_t prime_optimized_blur_cache(struct fixture* fixture, float backdrop_red, struct blur_data* blur_data) {
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_XBGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(target != NULL, "allocate SDR8 optimized-blur target")) {
    return DRM_FORMAT_INVALID;
  }

  // Create the stale 8-bit format we want cached.
  struct wlr_render_pass* pass =
      wlr_renderer_begin_buffer_pass(fixture->renderer, target, &(struct wlr_buffer_pass_options){0});
  if (!check(pass != NULL, "begin optimized-blur producer pass")) {
    wlr_buffer_drop(target);
    return DRM_FORMAT_INVALID;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);

  struct wlr_box box = {.width = TEST_WIDTH, .height = TEST_HEIGHT};
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = box,
          .color = {.r = backdrop_red, .g = 0.25f, .b = 0.5f, .a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );

  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, TEST_WIDTH, TEST_HEIGHT);

  uint32_t cache_format = DRM_FORMAT_INVALID;
  bool ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "init producer offscreen buffers");
  if (ok) {
    struct fx_render_blur_pass_options blur_options = {
        .tex_options =
            {
                .base =
                    {
                        .dst_box = box,
                        .clip = &region,
                        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                    },
                .clip_box = &box,
            },
        .blur_data = blur_data,
        .blur_strength = 1.0f,
    };
    ok = check(fx_render_pass_add_optimized_blur(fx_pass, &blur_options), "populate optimized-blur cache");
    if (ok) {
      struct fx_offscreen_buffers* fbos = fx_pass->fx_offscreen_buffers;
      ok = check(fbos != NULL && fbos->optimized_blur_buffer != NULL, "optimized-blur cache exists");
      if (ok) {
        cache_format = fbos->optimized_blur_buffer->drm_format;
      }
    }
  }
  pixman_region32_fini(&region);
  ok = wlr_render_pass_submit(pass) && ok;
  wlr_buffer_drop(target);
  return ok ? cache_format : DRM_FORMAT_INVALID;
}

// Renders one output frame of a 16/255 grey backdrop under a full-output blur
// that requests the optimized cache. With `render_cache` the frame first
// renders the optimized blur, as an optimized-blur node does. A zero `tf`
// renders without a color transform. `stale` reports whether the cache was
// stale for this pass before that.
static bool render_cached_blur_frame(
    struct fixture* fixture, struct wlr_buffer* target, enum wlr_color_transfer_function tf, bool render_cache,
    bool* stale
) {
  const bool pq = tf == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
  struct wlr_color_transform* transform = NULL;
  if (tf != 0) {
    transform = create_output_transform(
        pq ? WLR_COLOR_NAMED_PRIMARIES_BT2020 : WLR_COLOR_NAMED_PRIMARIES_SRGB, tf, pq ? 203.0f / 10000.0f : 1.0f
    );
    if (transform == NULL) {
      return false;
    }
  }
  struct wlr_render_pass* pass = fx_renderer_begin_output_buffer_pass(
      fixture->output, target, &(struct wlr_buffer_pass_options){.color_transform = transform}
  );
  wlr_color_transform_unref(transform);
  if (pass == NULL) {
    return false;
  }
  struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
  struct wlr_box box = {.width = target->width, .height = target->height};
  const float grey = 16.0f / 255.0f;
  wlr_render_pass_add_rect(
      pass,
      &(struct wlr_render_rect_options){
          .box = box,
          .color = {.r = grey, .g = grey, .b = grey, .a = 1.0f},
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  bool ok = fx_render_pass_init_offscreen_buffers(pass, fixture->output);
  *stale = ok && fx_render_pass_optimized_blur_stale(fx_pass);
  struct blur_data blur_data = {
      .num_passes = 1,
      .radius = 1.0f,
      .brightness = 1.0f,
      .contrast = 1.0f,
      .saturation = 1.0f,
  };
  pixman_region32_t region;
  pixman_region32_init_rect(&region, 0, 0, target->width, target->height);
  const float opacity = 1.0f;
  struct fx_render_blur_pass_options blur_options = {
      .tex_options =
          {
              .base =
                  {
                      .dst_box = box,
                      .clip = &region,
                      .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                      .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                      .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                      .alpha = &opacity,
                  },
              .clip_box = &box,
          },
      .blur_data = &blur_data,
      .blur_strength = 1.0f,
  };
  if (ok && render_cache) {
    ok = fx_render_pass_add_optimized_blur(fx_pass, &blur_options);
  }
  if (ok) {
    blur_options.use_optimized_blur = true;
    fx_render_pass_add_blur(fx_pass, &blur_options);
  }
  pixman_region32_fini(&region);
  return wlr_render_pass_submit(pass) && ok;
}

static bool check_sdr_transfer_pixels(
    struct fixture* fixture, struct wlr_buffer* target, enum wlr_color_transfer_function policy,
    enum wlr_color_transfer_function source_tf, enum wlr_color_transfer_function output_tf, const char* label
) {
  const float encoded = 16.0f / 255.0f;
  const float inherited = policy == WLR_COLOR_TRANSFER_FUNCTION_SRGB ? srgb_to_linear(encoded) : powf(encoded, 2.2f);
  const float source = source_tf == WLR_COLOR_TRANSFER_FUNCTION_SRGB ? srgb_to_linear(encoded)
      : source_tf == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22             ? powf(encoded, 2.2f)
                                                                     : inherited;
  uint8_t captured[TEST_WIDTH * TEST_HEIGHT * 4] = {0};
  bool ok = check(read_buffer(fixture, target, DRM_FORMAT_ABGR8888, TEST_WIDTH * 4, captured), label);
  for (size_t side = 0; side < 2; side++) {
    const int x = side == 0 ? TEST_WIDTH / 4 : TEST_WIDTH * 3 / 4, y = TEST_HEIGHT / 2;
    const float linear = side == 0 ? source : inherited;
    const int display_expected = output_tf == WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ
        ? to_u8(linear_to_pq(linear * 203.0f / 10000.0f))
        : output_tf == WLR_COLOR_TRANSFER_FUNCTION_SRGB ? to_u8(linear_to_srgb(linear))
                                                        : to_u8(powf(linear, 1.0f / 2.2f));
    const int capture_expected =
        policy == WLR_COLOR_TRANSFER_FUNCTION_SRGB ? to_u8(linear_to_srgb(linear)) : to_u8(powf(linear, 1.0f / 2.2f));
    uint8_t shown[4] = {0};
    ok = fixture_read_display_pixel(fixture, target, x, y, shown) && ok;
    const uint8_t capture_value = captured[(y * TEST_WIDTH + x) * 4];
    if (abs((int)shown[2] - display_expected) > 1 || abs((int)capture_value - capture_expected) > 1) {
      fprintf(
          stderr, "%s %s: display %u expected %d, capture %u expected %d\n", label, side == 0 ? "texture" : "rect",
          shown[2], display_expected, capture_value, capture_expected
      );
      ok = false;
    }
  }
  return ok;
}

static void acknowledge_scene_damage(struct wlr_scene_output* output) {
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_damage(&state, &output->pending_commit_damage);
  wlr_scene_output_acknowledge_damage_for_test(output, &state);
  wlr_output_state_finish(&state);
}

static bool test_sdr_transfer_policy(struct fixture* fixture) {
  bool ok = true;
  const uint8_t grey[4] = {16, 16, 16, 255};
  const struct wlr_render_color color = {.r = 16.0f / 255.0f, .g = 16.0f / 255.0f, .b = 16.0f / 255.0f, .a = 1};
  const enum wlr_color_transfer_function pq = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
  const enum wlr_color_transfer_function gamma22 = WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
  const enum wlr_color_transfer_function srgb = WLR_COLOR_TRANSFER_FUNCTION_SRGB;
  const enum wlr_color_transfer_function sources[] = {0, srgb, gamma22};
  struct fixture outputs[2] = {*fixture, *fixture};
  outputs[1].output = wlr_headless_add_output(fixture->backend, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_scene* scene = wlr_scene_create();
  struct wlr_scene_output* scene_outputs[2] = {0};
  struct wlr_scene_optimized_blur* optimized[2] = {0};
  struct wlr_buffer* targets[2] = {0};
  uint8_t hdr_pixels[2][2][4] = {0};
  struct wlr_texture* fill = wlr_texture_from_pixels(fixture->renderer, DRM_FORMAT_ABGR8888, 4, 1, 1, grey);
  if (!check(outputs[1].output != NULL && scene != NULL && fill != NULL, "create SDR transfer fixtures")
      || !check(
          wlr_output_init_render(outputs[1].output, fixture->allocator, fixture->renderer), "initialize second output"
      )) {
    ok = false;
    goto out;
  }
  for (size_t output = 0; output < 2; output++) {
    struct wlr_output_state enable;
    wlr_output_state_init(&enable);
    wlr_output_state_set_enabled(&enable, true);
    wlr_output_state_set_custom_mode(&enable, TEST_WIDTH, TEST_HEIGHT, 60000);
    bool enabled = wlr_output_commit_state(outputs[output].output, &enable);
    wlr_output_state_finish(&enable);
    if (!check(enabled, "enable SDR transfer output")) {
      ok = false;
      goto out;
    }
    scene_outputs[output] = wlr_scene_output_create(scene, outputs[output].output);
    targets[output] = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
    if (!check(scene_outputs[output] != NULL && targets[output] != NULL, "create output-local SDR transfer target")) {
      ok = false;
      goto out;
    }
    wlr_scene_output_set_position(scene_outputs[output], output * TEST_WIDTH, 0);
  }
  const float scene_color[4] = {color.r, color.g, color.b, color.a};
  if (!check(
          wlr_scene_rect_create(&scene->tree, 2 * TEST_WIDTH, TEST_HEIGHT, scene_color) != NULL, "create scene backdrop"
      )) {
    ok = false;
    goto out;
  }
  for (size_t output = 0; output < 2; output++) {
    optimized[output] = wlr_scene_optimized_blur_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT);
    if (!check(optimized[output] != NULL, "create output-local optimized blur")) {
      ok = false;
      goto out;
    }
    wlr_scene_node_set_position(&optimized[output]->node, output * TEST_WIDTH, 0);
  }
  struct wlr_scene_blur* blur = wlr_scene_blur_create(&scene->tree, 2 * TEST_WIDTH, TEST_HEIGHT);
  struct wlr_scene_border* border = wlr_scene_border_create(&scene->tree, scene_color, scene_color);
  if (!check(blur != NULL && border != NULL, "create scene blur and compositor border")) {
    ok = false;
    goto out;
  }
  wlr_scene_blur_set_should_only_blur_bottom_layer(blur, true);
  wlr_scene_border_set_geometry(
      border, 2 * TEST_WIDTH, TEST_HEIGHT, 2, 0,
      (struct clipped_region){.area = {2, 2, 2 * TEST_WIDTH - 4, TEST_HEIGHT - 4}}, (struct fx_corner_radii){0},
      (struct fx_corner_radii){0}
  );
  wlr_scene_set_blur_data(scene, 1, 1, 0, 1, 1, 1);
  ok = check(!wlr_scene_output_set_sdr_transfer_function(scene_outputs[0], pq), "reject unsupported SDR curve") && ok;

  for (size_t round = 0; round < 2; round++) {
    enum wlr_color_transfer_function policies[2] = {round == 0 ? gamma22 : srgb, round == 0 ? srgb : gamma22};
    for (size_t output = 0; output < 2; output++) {
      acknowledge_scene_damage(scene_outputs[output]);
      ok = check(
               wlr_scene_output_set_sdr_transfer_function(
                   scene_outputs[output], fx_output_get_sdr_transfer_function(outputs[output].output)
               ) && pixman_region32_empty(&scene_outputs[output]->pending_commit_damage),
               "setting the same curve adds no scene damage"
           )
          && ok;
    }
    for (size_t output = 0; output < 2; output++) {
      const struct wlr_box dot = {.width = 1, .height = 1};
      wlr_scene_output_damage_box(scene_outputs[output], &dot);
      if (fx_output_get_sdr_transfer_function(outputs[output].output) != policies[output]) {
        if (round != 0) {
          ok = check(!optimized[output]->dirty, "previous scene frame settled its optimized blur cache") && ok;
        }
        ok = check(
                 wlr_scene_output_set_sdr_transfer_function(scene_outputs[output], policies[output]),
                 "set output-local SDR transfer"
             )
            && ok;
        const pixman_box32_t whole = {.x2 = TEST_WIDTH, .y2 = TEST_HEIGHT};
        ok = check(
                 pixman_region32_contains_rectangle(&scene_outputs[output]->pending_commit_damage, &whole)
                     == PIXMAN_REGION_IN,
                 "curve change expands partial damage to the complete output"
             )
            && ok;
        ok = check(optimized[output]->dirty, "curve change marks the output's optimized blur for regeneration") && ok;
      }
      for (size_t source = 0; source < sizeof(sources) / sizeof(sources[0]); source++) {
        for (size_t capture = 0; capture < 2; capture++) {
          char label[160];
          snprintf(
              label, sizeof(label), "round %zu output %zu policy %u source %u capture %zu", round, output,
              policies[output], sources[source], capture
          );
          if (!check(
                  render_sdr_on(&outputs[output], targets[output], fill, &color, sources[source], pq, capture), label
              )) {
            ok = false;
            continue;
          }
          ok =
              check_sdr_transfer_pixels(&outputs[output], targets[output], policies[output], sources[source], pq, label)
              && ok;
        }
      }
      const enum wlr_color_transfer_function hdr_sources[] = {pq, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR};
      for (size_t source = 0; source < sizeof(hdr_sources) / sizeof(hdr_sources[0]); source++) {
        ok = check(
                 render_sdr_on(&outputs[output], targets[output], fill, &color, hdr_sources[source], pq, false),
                 "render explicitly tagged HDR content"
             )
            && ok;
        uint8_t shown[4] = {0};
        ok =
            fixture_read_display_pixel(&outputs[output], targets[output], TEST_WIDTH / 4, TEST_HEIGHT / 2, shown) && ok;
        if (round == 0) {
          memcpy(hdr_pixels[output][source], shown, sizeof(shown));
        } else {
          ok = check(
                   codes_close(shown, hdr_pixels[output][source], 1),
                   "explicit PQ and linear content remain unchanged after switching the SDR curve"
               )
              && ok;
        }
      }
      if (policies[output] == srgb) {
        ok =
            check(
                render_sdr_on(&outputs[output], targets[output], fill, &color, 0, gamma22, false), "render gamma target"
            )
            && ok;
        ok = check_sdr_transfer_pixels(&outputs[output], targets[output], gamma22, 0, gamma22, "non-PQ target") && ok;
        struct fx_offscreen_buffers* buffers = fx_offscreen_buffers_try_get(outputs[output].output);
        if (check(buffers != NULL, "find output scratch storage")) {
          fx_offscreen_buffers_destroy(buffers);
          ok = check(
                   fx_output_get_sdr_transfer_function(outputs[output].output) == srgb,
                   "curve policy survives renderer scratch-storage destruction"
               )
              && ok;
        } else {
          ok = false;
        }
        ok = check(
                 render_sdr_on(&outputs[output], targets[output], fill, &color, 0, pq, false),
                 "recreate PQ scratch storage"
             )
            && ok;
        ok = check_sdr_transfer_pixels(&outputs[output], targets[output], srgb, 0, pq, "recreated PQ storage") && ok;
      }
      ok = check(
               render_sdr_on(&outputs[output], targets[output], fill, &color, 0, srgb, false),
               "render unchanged SDR target"
           )
          && ok;
      ok = check_sdr_transfer_pixels(&outputs[output], targets[output], srgb, 0, srgb, "non-HDR target") && ok;
    }
    bool stale = false;
    for (size_t output = 0; output < 2; output++) {
      ok = check(
               render_cached_blur_frame(&outputs[output], targets[output], pq, true, &stale),
               "render selected blur curve"
           )
          && ok;
      uint8_t shown[4] = {0};
      ok = fixture_read_display_pixel(&outputs[output], targets[output], TEST_WIDTH / 2, TEST_HEIGHT / 2, shown) && ok;
      const float linear = policies[output] == srgb ? srgb_to_linear(16.0f / 255.0f) : powf(16.0f / 255.0f, 2.2f);
      const int expected = to_u8(linear_to_pq(linear * 203.0f / 10000.0f));
      ok = check(abs((int)shown[2] - expected) <= 1, "optimized blur uses its output's selected SDR curve") && ok;

      wlr_scene_output_set_sdr_white_level(scene_outputs[output], 203);
      struct wlr_output_state state;
      wlr_output_state_init(&state);
      wlr_output_state_set_image_description(
          &state,
          &(struct wlr_output_image_description){.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020, .transfer_function = pq}
      );
      const struct wlr_scene_output_state_options options = {.capture_sdr = true};
      bool built = wlr_scene_output_build_state(scene_outputs[output], &state, &options) && state.buffer != NULL;
      ok = check(built, "build scene after selecting SDR curve") && ok;
      if (built) {
        const int samples[] = {1, TEST_WIDTH / 2, TEST_WIDTH - 1};
        for (size_t sample = 0; sample < sizeof(samples) / sizeof(samples[0]); sample++) {
          ok =
              fixture_read_display_pixel(&outputs[output], state.buffer, samples[sample], TEST_HEIGHT / 2, shown) && ok;
          ok = check(
                   abs((int)shown[2] - expected) <= 1,
                   "border and cached blur redraw with the selected curve outside the original partial damage"
               )
              && ok;
        }
        uint8_t capture[TEST_WIDTH * TEST_HEIGHT * 4];
        ok = check(
                 read_buffer(&outputs[output], state.buffer, DRM_FORMAT_ABGR8888, TEST_WIDTH * 4, capture),
                 "read scene SDR capture"
             )
            && ok;
        ok = check(
                 abs((int)capture[((TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2) * 4] - 16) <= 1,
                 "scene capture retains implicit SDR codes after switching curves"
             )
            && ok;
      }
      wlr_output_state_finish(&state);
    }
  }

out:
  if (scene != NULL) {
    wlr_scene_node_destroy(&scene->tree.node);
  }
  for (size_t output = 0; output < 2; output++) {
    if (targets[output] != NULL) {
      wlr_buffer_drop(targets[output]);
    }
  }
  if (fill != NULL) {
    wlr_texture_destroy(fill);
  }
  if (outputs[1].output != NULL) {
    wlr_output_destroy(outputs[1].output);
  }
  return ok;
}

// The optimized blur holds linear light decoded with the SDR curve of the pass
// that rendered it. After an output switches from an sRGB transform to PQ,
// both keep FP16 caches, so only the curve tells the old cache apart: a blur
// must not sample it until fx_render_pass_add_optimized_blur, which an
// optimized-blur node calls, renders it again.
static bool test_optimized_blur_sdr_curve(struct fixture* fixture) {
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(target != NULL, "allocate optimized-blur target")) {
    return false;
  }
  bool stale = false;
  bool ok = check(
      render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale),
      "render optimized blur on an sRGB transform"
  );
  struct fx_offscreen_buffers* fbos = fx_offscreen_buffers_try_get(fixture->output);
  ok = check(
           fbos != NULL
               && fbos->optimized_blur_buffer != NULL
               && fbos->optimized_blur_buffer->drm_format == DRM_FORMAT_ABGR16161616F
               && fbos->optimized_no_blur_buffer != NULL
               && fbos->optimized_no_blur_buffer->drm_format == DRM_FORMAT_ABGR16161616F,
           "sRGB transform caches FP16 optimized blur"
       )
      && ok;

  const int gamma22 = to_u8(linear_to_pq(powf(16.0f / 255.0f, 2.2f) * 203.0f / 10000.0f));
  const int piecewise = to_u8(linear_to_pq(srgb_to_linear(16.0f / 255.0f) * 203.0f / 10000.0f));
  const char* frames[] = {"stale cache on PQ", "re-rendered cache on PQ"};
  for (size_t frame = 0; frame < 2 && ok; frame++) {
    ok = check(
        render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, frame == 1, &stale),
        "render blur on PQ"
    );
    ok = check(stale, "sRGB-curve cache is stale on PQ until rendered again") && ok;
    uint8_t shown[4];
    if (ok && !fixture_read_display_pixel(fixture, target, TEST_WIDTH / 2, TEST_HEIGHT / 2, shown)) {
      ok = false;
    } else if (ok && abs((int)shown[2] - gamma22) > 1) {
      fprintf(
          stderr, "%s: blur shows %u, gamma 2.2 expects %d, the sRGB-curve cache gives %d\n", frames[frame], shown[2],
          gamma22, piecewise
      );
      ok = false;
    }
  }
  ok = check(
           fbos != NULL && fbos->optimized_blur_sdr_tf == WLR_COLOR_TRANSFER_FUNCTION_GAMMA22,
           "re-rendered cache records the PQ curve"
       )
      && ok;
  wlr_buffer_drop(target);
  return ok;
}

// A 10-bit SDR output without a color transform keeps its optimized blur in
// FP16 too, but holding encoded values. A linear-light pass with the same
// sRGB curve and format must render it again rather than read encoded values
// as linear light.
static bool test_optimized_blur_encoded_cache(struct fixture* fixture) {
  // An opaque 10-bit target, as a 10-bit SDR output scans out.
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_XBGR2101010, TEST_WIDTH, TEST_HEIGHT);
  if (!check(target != NULL, "allocate 10-bit optimized-blur target")) {
    return false;
  }
  bool stale = false;
  bool ok = check(
      render_cached_blur_frame(fixture, target, 0, true, &stale), "render optimized blur without a color transform"
  );
  struct fx_offscreen_buffers* fbos = fx_offscreen_buffers_try_get(fixture->output);
  ok = check(fbos != NULL && fbos->optimized_blur_buffer != NULL, "encoded optimized-blur cache exists") && ok;
  if (!ok || fbos->optimized_blur_buffer->drm_format != DRM_FORMAT_ABGR16161616F) {
    if (ok) {
      // Without half-float linear filtering the cache is 8-bit and its format
      // alone keeps it from a linear-light pass.
      fprintf(stderr, "SKIP: a 10-bit SDR output keeps an 8-bit optimized-blur cache here\n");
    }
    wlr_buffer_drop(target);
    return ok;
  }

  ok = check(
      render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, false, &stale),
      "render blur on an sRGB transform"
  );
  ok = check(stale, "encoded blur cache is stale for a linear-light pass") && ok;
  uint8_t shown[4];
  if (ok && !fixture_read_display_pixel(fixture, target, TEST_WIDTH / 2, TEST_HEIGHT / 2, shown)) {
    ok = false;
  } else if (ok && abs((int)shown[2] - 16) > 1) {
    fprintf(stderr, "blur over an encoded cache shows %u, expected 16\n", shown[2]);
    ok = false;
  }
  wlr_buffer_drop(target);
  return ok;
}

// An optimized blur from a pass of another pixel size, as after a scale change
// at the same logical size, is stale until rendered again at the new size.
static bool test_optimized_blur_size_change(struct fixture* fixture) {
  struct wlr_buffer* full = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct wlr_buffer* half = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH / 2, TEST_HEIGHT / 2);
  bool ok = check(full != NULL && half != NULL, "allocate optimized-blur targets");
  bool stale = false;
  ok = ok
      && check(
           render_cached_blur_frame(fixture, full, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale),
           "render optimized blur at full size"
      );
  // `stale` is sampled before this frame renders the cache again.
  ok = ok
      && check(
           render_cached_blur_frame(fixture, half, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale),
           "render optimized blur at half size"
      );
  ok = ok && check(stale, "full-size blur cache is stale for a half-size pass");
  ok = ok
      && check(
           render_cached_blur_frame(fixture, half, WLR_COLOR_TRANSFER_FUNCTION_SRGB, false, &stale),
           "blur again at half size"
      );
  ok = ok && check(!stale, "re-rendered blur cache fits the half-size pass");
  if (full != NULL) {
    wlr_buffer_drop(full);
  }
  if (half != NULL) {
    wlr_buffer_drop(half);
  }
  return ok;
}

// A failed copy into the optimized blur leaves it stale instead of half
// written and valid, and the next successful render makes it current again.
static bool test_optimized_blur_copy_failure(struct fixture* fixture) {
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  if (!check(target != NULL, "allocate optimized-blur target")) {
    return false;
  }
  bool stale = false;
  bool ok = check(
      render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale), "render optimized blur"
  );
  fx_renderer_fail_target_copies_for_test(fixture->renderer, true);
  bool rendered = render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale);
  fx_renderer_fail_target_copies_for_test(fixture->renderer, false);
  ok = check(!rendered, "optimized blur reports failed copies") && ok;
  ok = check(
           render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, true, &stale),
           "render optimized blur again"
       )
      && ok;
  ok = check(stale, "failed copies leave the blur cache stale") && ok;
  ok = check(
           render_cached_blur_frame(fixture, target, WLR_COLOR_TRANSFER_FUNCTION_SRGB, false, &stale),
           "blur from the re-rendered cache"
       )
      && ok;
  ok = check(!stale, "re-rendered blur cache is current") && ok;
  wlr_buffer_drop(target);
  return ok;
}

// A stale-format optimized-blur cache must not be reused after the output's
// render format changes. The offscreen effect buffers are output-local, so a
// cache primed while the output was 8-bit SDR can survive into a 10-bit pass
// whose effect buffers are FP16.
static bool test_optimized_blur_format_cache(struct fixture* fixture) {
  uint32_t format = select_10bit_format(fixture);
  if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
    return false;
  }

  struct blur_data blur_data = {
      .num_passes = 1,
      .radius = 1.0f,
      .brightness = 1.0f,
      .contrast = 1.0f,
      .saturation = 1.0f,
  };

  // Prime the cache with a bright-red backdrop while the output is 8-bit.
  const float stale_red = 0.9f;
  uint32_t cache_format = prime_optimized_blur_cache(fixture, stale_red, &blur_data);
  if (!check(cache_format != DRM_FORMAT_INVALID, "prime optimized-blur cache")) {
    return false;
  }
  if (!check(cache_format == DRM_FORMAT_XBGR8888, "cache primed at 8-bit SDR format")) {
    return false;
  }

  // Now render a consumer blur pass into a 10-bit target with a color
  // transform, so its effect buffers want FP16. The live backdrop is a
  // dark red, different from the cached bright red.
  const float live_red = 0.1f;
  struct wlr_buffer* target = create_output_buffer(fixture, format, TEST_WIDTH, TEST_HEIGHT);
  if (!check(target != NULL, "allocate 10-bit consumer target")) {
    return false;
  }
  struct wlr_color_transform* transform =
      wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
  if (!check(transform != NULL, "create consumer output transform")) {
    wlr_buffer_drop(target);
    return false;
  }

  uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(
      fixture->renderer, target,
      &(struct wlr_buffer_pass_options){
          .color_transform = transform,
      }
  );
  bool ok = check(pass != NULL, "begin consumer blur pass");
  if (ok) {
    struct fx_gles_render_pass* fx_pass = fx_get_render_pass(pass);
    struct wlr_box box = {.width = TEST_WIDTH, .height = TEST_HEIGHT};
    pixman_region32_t region;
    pixman_region32_init_rect(&region, 0, 0, TEST_WIDTH, TEST_HEIGHT);

    wlr_render_pass_add_rect(
        pass,
        &(struct wlr_render_rect_options){
            .box = box,
            .color = {.r = live_red, .g = 0.25f, .b = 0.5f, .a = 1.0f},
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        }
    );

    ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "init consumer offscreen buffers") && ok;
    // The cache from the producer pass should still be present and stale.
    struct fx_offscreen_buffers* fbos = fx_pass->fx_offscreen_buffers;
    ok = check(
             fbos != NULL
                 && fbos->optimized_blur_buffer != NULL
                 && fbos->optimized_blur_buffer->drm_format == DRM_FORMAT_XBGR8888,
             "consumer sees the stale 8-bit optimized-blur cache"
         )
        && ok;

    const float opacity = 1.0f;
    struct fx_render_blur_pass_options blur_options = {
        .tex_options =
            {
                .base =
                    {
                        .dst_box = box,
                        .clip = &region,
                        .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
                        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                        .alpha = &opacity,
                    },
                .clip_box = &box,
            },
        // Request the optimized path.
        .use_optimized_blur = true,
        .blur_data = &blur_data,
        .blur_strength = 1.0f,
    };
    fx_render_pass_add_blur(fx_pass, &blur_options);
    pixman_region32_fini(&region);

    ok = check(wlr_render_pass_submit(pass), "submit consumer blur pass") && ok;
  }

  if (ok) {
    ok = check(read_buffer(fixture, target, format, TEST_WIDTH * sizeof(uint32_t), output), "read consumer output");
  }

  wlr_color_transform_unref(transform);
  wlr_buffer_drop(target);
  if (!ok) {
    return false;
  }

  uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
  int actual = pixel & 0x3FF;
  int expected = lroundf(live_red * 1023.0f);
  int stale = lroundf(stale_red * 1023.0f);
  fprintf(stderr, "INFO: red actual=%d expected(live)=%d stale(cache)=%d\n", actual, expected, stale);
  return check(abs(actual - expected) <= 24, "format-mismatched optimized-blur cache is not reused");
}

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
static size_t image_target_texture_calls;

static void GL_APIENTRY count_image_target_texture(GLenum target, GLeglImageOES image) {
  image_target_texture_calls++;
  image_target_texture(target, image);
}

static bool sample_framebuffer_view(
    struct fixture* fixture, struct fx_framebuffer* source, struct wlr_buffer* target, float red,
    size_t expected_bindings, const char* binding_label
) {
  struct wlr_render_pass* paint = wlr_renderer_begin_buffer_pass(fixture->renderer, source->buffer, NULL);
  if (!check(paint != NULL, "begin framebuffer view update")
      || !check(
          submit_solid_frame(paint, source->buffer->width, source->buffer->height, red), "update framebuffer pixels"
      )) {
    return false;
  }

  struct wlr_texture* texture = fx_texture_from_buffer(fixture->renderer, source->buffer);
  if (!check(texture != NULL, "import framebuffer view")) {
    return false;
  }
  if (image_target_texture_calls != expected_bindings) {
    fprintf(
        stderr, "%s: got %zu image bindings, expected %zu after rendering red %.1f\n", binding_label,
        image_target_texture_calls, expected_bindings, red
    );
  }
  bool ok = check(image_target_texture_calls == expected_bindings, binding_label);
  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, NULL);
  if (!check(pass != NULL, "begin framebuffer view sampling")) {
    wlr_texture_destroy(texture);
    return false;
  }
  wlr_render_pass_add_texture(
      pass,
      &(struct wlr_render_texture_options){
          .texture = texture,
          .dst_box = {.width = TEST_WIDTH, .height = TEST_HEIGHT},
          .filter_mode = WLR_SCALE_FILTER_NEAREST,
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
      }
  );
  ok = check(wlr_render_pass_submit(pass), "sample framebuffer view") && ok;
  wlr_texture_destroy(texture);

  uint8_t pixel[4] = {0};
  const uint8_t expected[4] = {to_u8(0.5f), to_u8(0.25f), to_u8(red), 255};
  ok = fixture_read_display_pixel(fixture, target, TEST_WIDTH / 2, TEST_HEIGHT / 2, pixel) && ok;
  return check(codes_close(pixel, expected, 1), "framebuffer view samples the latest rendered pixels") && ok;
}

// Count real EGLImage attachment calls while sampling pixels, including a
// renderbuffer update after the internal texture view has been established.
static bool test_framebuffer_texture_binding(struct fixture* fixture) {
  struct fx_renderer* renderer = fx_get_renderer(fixture->renderer);
  struct wlr_egl_context previous;
  if (!check(wlr_egl_make_current(renderer->egl, &previous), "activate framebuffer view context")) {
    return false;
  }
  image_target_texture = renderer->procs.glEGLImageTargetTexture2DOES;
  image_target_texture_calls = 0;
  renderer->procs.glEGLImageTargetTexture2DOES = count_image_target_texture;

  bool ok = true;
  bool failed = false;
  struct fx_framebuffer* owned = NULL;
  struct wlr_buffer* external = NULL;
  struct wlr_buffer* target = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
  fx_framebuffer_get_or_create_custom(
      renderer, fixture->allocator, TEST_WIDTH, TEST_HEIGHT, DRM_FORMAT_ABGR16161616F, &owned, &failed
  );
  if (!check(target != NULL && !failed && owned != NULL && owned->owned, "create internal framebuffer view")) {
    ok = false;
    goto out;
  }
  ok = sample_framebuffer_view(fixture, owned, target, 0.2f, 1, "first owned import attaches one EGLImage") && ok;
  ok = sample_framebuffer_view(fixture, owned, target, 0.8f, 1, "owned framebuffer update reuses its EGLImage binding")
      && ok;

  fx_framebuffer_get_or_create_custom(
      renderer, fixture->allocator, TEST_WIDTH / 2, TEST_HEIGHT, DRM_FORMAT_ABGR8888, &owned, &failed
  );
  if (!check(
          !failed && owned != NULL && owned->buffer->width == TEST_WIDTH / 2, "replace internal framebuffer storage"
      )) {
    ok = false;
    goto out;
  }
  ok = sample_framebuffer_view(fixture, owned, target, 0.4f, 2, "replaced storage attaches a new EGLImage") && ok;
  ok = sample_framebuffer_view(fixture, owned, target, 0.6f, 2, "replacement update reuses its EGLImage binding") && ok;

  external = create_output_buffer(fixture, DRM_FORMAT_ABGR8888, TEST_WIDTH, TEST_HEIGHT);
  struct fx_framebuffer* imported = external != NULL ? fx_framebuffer_get_or_create(renderer, external) : NULL;
  if (!check(imported != NULL && !imported->owned && !imported->external_only, "create external framebuffer view")) {
    ok = false;
    goto out;
  }
  ok = sample_framebuffer_view(fixture, imported, target, 0.3f, 3, "first external import attaches its EGLImage") && ok;
  ok = sample_framebuffer_view(fixture, imported, target, 0.7f, 4, "external update reattaches its EGLImage") && ok;

out:
  renderer->procs.glEGLImageTargetTexture2DOES = image_target_texture;
  image_target_texture = NULL;
  if (external != NULL) {
    wlr_buffer_drop(external);
  }
  if (owned != NULL) {
    wlr_buffer_drop(owned->buffer);
  }
  if (target != NULL) {
    wlr_buffer_drop(target);
  }
  wlr_egl_restore_context(&previous);
  return ok;
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s CASE\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "hdr-white-point-math") == 0) {
    return test_hdr_white_point_math() ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  struct fixture fixture;
  if (!fixture_init(&fixture)) {
    fprintf(stderr, "SKIP: no FP16-capable DRM render node\n");
    fixture_finish(&fixture);
    return 77;
  }

  bool ok;
  if (strcmp(argv[1], "implicit-srgb-target") == 0) {
    ok = test_implicit_srgb_target(&fixture);
  } else if (strcmp(argv[1], "scene-unset-primaries") == 0) {
    ok = test_scene_unset_primaries(&fixture);
  } else if (strcmp(argv[1], "scene-custom-color") == 0) {
    ok = test_scene_custom_color(&fixture);
  } else if (strcmp(argv[1], "scene-custom-white") == 0) {
    ok = test_scene_custom_white(&fixture);
  } else if (strcmp(argv[1], "scene-custom-bt1886") == 0) {
    ok = test_scene_custom_bt1886(&fixture);
  } else if (strcmp(argv[1], "scene-scrgb-absolute-luminance") == 0) {
    ok = test_scene_scrgb_absolute_luminance(&fixture);
  } else if (strcmp(argv[1], "scene-hdr-sdr-fallback") == 0) {
    ok = test_scene_hdr_sdr_fallback(&fixture);
  } else if (strcmp(argv[1], "hdr-white-point") == 0) {
    ok = test_hdr_white_point(&fixture);
  } else if (strcmp(argv[1], "sdr-white-point-alpha") == 0) {
    ok = test_sdr_white_point_alpha(&fixture);
  } else if (strcmp(argv[1], "sdr-curve-consistency") == 0) {
    ok = test_sdr_curve_consistency(&fixture);
  } else if (strcmp(argv[1], "sdr-transfer-policy") == 0) {
    ok = test_sdr_transfer_policy(&fixture);
  } else if (strcmp(argv[1], "optimized-blur-sdr-curve") == 0) {
    ok = test_optimized_blur_sdr_curve(&fixture);
  } else if (strcmp(argv[1], "optimized-blur-encoded-cache") == 0) {
    ok = test_optimized_blur_encoded_cache(&fixture);
  } else if (strcmp(argv[1], "optimized-blur-size-change") == 0) {
    ok = test_optimized_blur_size_change(&fixture);
  } else if (strcmp(argv[1], "optimized-blur-copy-failure") == 0) {
    ok = test_optimized_blur_copy_failure(&fixture);
  } else if (strcmp(argv[1], "gamma22-to-srgb") == 0) {
    ok = test_gamma22_to_srgb(&fixture);
  } else if (strcmp(argv[1], "pq-roundtrip") == 0) {
    ok = test_pq_roundtrip(&fixture);
  } else if (strcmp(argv[1], "bt2020-to-srgb") == 0) {
    ok = test_bt2020_to_srgb(&fixture);
  } else if (strcmp(argv[1], "scrgb-negative-channels") == 0) {
    ok = test_scrgb_negative_channels(&fixture);
  } else if (strcmp(argv[1], "fp16-blending") == 0) {
    ok = test_fp16(&fixture, false);
  } else if (strcmp(argv[1], "fp16-blur") == 0) {
    ok = test_fp16(&fixture, true);
  } else if (strcmp(argv[1], "fp16-blur-effects") == 0) {
    ok = test_fp16_blur_effects(&fixture);
  } else if (strcmp(argv[1], "fp16-save-restore") == 0) {
    ok = test_fp16_save_restore(&fixture);
  } else if (strcmp(argv[1], "sdr10-blur-buffer-format") == 0) {
    ok = test_sdr10_blur_buffer_format(&fixture);
  } else if (strcmp(argv[1], "optimized-blur-format-cache") == 0) {
    ok = test_optimized_blur_format_cache(&fixture);
  } else if (strcmp(argv[1], "shared-output-buffers") == 0) {
    ok = test_shared_output_buffers(&fixture);
  } else if (strcmp(argv[1], "output-lut-cache") == 0) {
    ok = test_output_lut_cache(&fixture);
  } else if (strcmp(argv[1], "shared-sdr-capture") == 0) {
    ok = test_shared_sdr_capture(&fixture);
  } else if (strcmp(argv[1], "shared-output-transform-transition") == 0) {
    ok = test_shared_output_transform_transition(&fixture);
  } else if (strcmp(argv[1], "shared-sdr-capture-lock") == 0) {
    ok = test_shared_sdr_capture_lock(&fixture);
  } else if (strcmp(argv[1], "shared-sdr-capture-storage-change") == 0) {
    ok = test_shared_sdr_capture_storage_change(&fixture);
  } else if (strcmp(argv[1], "capture-read-format") == 0) {
    ok = test_capture_read_format(&fixture);
  } else if (strcmp(argv[1], "framebuffer-texture-binding") == 0) {
    ok = test_framebuffer_texture_binding(&fixture);
  } else {
    fprintf(stderr, "unknown case: %s\n", argv[1]);
    ok = false;
  }

  fixture_finish(&fixture);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
