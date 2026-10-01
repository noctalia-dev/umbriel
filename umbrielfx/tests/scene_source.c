// Frozen workspace capture, role replacement, color precision and ownership.
#include "render_fixture.h"
#include "types/wlr_scene.h"
#include "types/scene_source.h"
#include "render/fx_renderer/scene_program.h"
#include "umbrielfx/render/effect.h"

static struct fx_scene_source_view native_view(struct wlr_scene_output *output,
    struct wlr_scene_node *first, struct wlr_scene_node *last) {
  struct fx_scene_source_view view = {.first = first, .last = last,
      .scale = output->output->scale, .extent = {.x = output->x, .y = output->y}};
  wlr_output_effective_resolution(output->output, &view.extent.width, &view.extent.height);
  return view;
}

static uint64_t native_pair_bytes(struct wlr_scene_output *output,
    struct wlr_scene_node *first, struct wlr_scene_node *last) {
  struct fx_scene_source_view view = native_view(output, first, last);
  return fx_scene_source_frozen_pair_bytes(output, &view);
}

static bool native_pair_capture(struct wlr_scene_output *output,
    struct wlr_scene_node *first, struct wlr_scene_node *last,
    uint64_t bytes, struct fx_scene_source_pair_for_test *pair) {
  struct fx_scene_source_view view = native_view(output, first, last);
  return fx_scene_source_pair_capture_for_test(output, &view, bytes, pair);
}

static uint64_t output_pair_bytes(struct wlr_scene_output *output) {
  if (wl_list_empty(&output->scene->tree.children)) return 0;
  struct wlr_scene_node *first = wl_container_of(output->scene->tree.children.next, first, link);
  struct wlr_scene_node *last = wl_container_of(output->scene->tree.children.prev, last, link);
  return native_pair_bytes(output, first, last);
}

struct sample_counter {
	struct wl_listener listener;
	unsigned count;
};

static void sampled(struct wl_listener *listener, void *data) {
	struct sample_counter *counter = wl_container_of(listener, counter, listener);
	(void)data;
	counter->count++;
}

// FP16 sources are working-space images: ordinary sRGB nodes are decoded,
// authored extended values survive, and encoding happens only at the landing.
static float source_half(uint16_t h) {
	int exponent = (h >> 10) & 31;
	float value = exponent == 0 ? ldexpf(h & 1023, -24)
		: ldexpf(1.0f + (h & 1023) / 1024.0f, exponent - 15);
	return h & 0x8000 ? -value : value;
}

// GLES does not guarantee packed ten-bit readback. Sampling into a gamma-valued
// FP16 attachment retains every ten-bit step without an eight-bit conversion.
static bool read_ten_bit(struct fixture *fixture, struct wlr_buffer *buffer, uint16_t *pixels) {
	struct wlr_buffer *readback = create_output_buffer(fixture, DRM_FORMAT_ABGR16161616F, 16, 16);
	struct wlr_texture *texture = buffer ? wlr_texture_from_buffer(fixture->renderer, buffer) : NULL;
	struct wlr_render_pass *pass = readback ? wlr_renderer_begin_buffer_pass(fixture->renderer, readback, NULL) : NULL;
	bool ok = texture && pass;
	if (ok) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){.texture = texture,
			.dst_box = {.width = 16, .height = 16}, .blend_mode = WLR_RENDER_BLEND_MODE_NONE});
		ok = wlr_render_pass_submit(pass) && read_buffer(fixture, readback, DRM_FORMAT_ABGR16161616F, 16*8, pixels);
	}
	if (texture) wlr_texture_destroy(texture);
	if (readback) wlr_buffer_drop(readback);
	return ok;
}

static bool test_unmanaged_ten_bit(struct fixture *fixture) {
	bool ok = true;
	unsigned tested = 0;
	const uint32_t formats[] = {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010};
	for (unsigned f = 0; f < 2; f++) {
		struct wlr_buffer *probe = create_output_buffer(fixture, formats[f], 16, 16);
		if (!probe) { fprintf(stderr, "ten-bit native allocator unavailable for format %08x\n", formats[f]); continue; }
		wlr_buffer_drop(probe);
		tested++;
		struct wlr_output_state setup;
		wlr_output_state_init(&setup);
		wlr_output_state_set_render_format(&setup, formats[f]);
		wlr_output_state_set_transform(&setup, WL_OUTPUT_TRANSFORM_NORMAL);
		wlr_output_state_set_scale(&setup, 1);
		ok &= check(wlr_output_commit_state(fixture->output, &setup), "select unmanaged ten-bit output");
		wlr_output_state_finish(&setup);
		struct wlr_scene *scene = wlr_scene_create();
		struct wlr_scene_output *output = wlr_scene_output_create(scene, fixture->output);
		const float grey[] = {0.5f, 0.5f, 0.5f, 1};
		struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 16, 16, grey);
		struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
			"vec4 window(vec2 uv) { return vec4((512.0 + floor(uv.x*16.0))/1023.0, umbriel_sample(uv).g, 0.25, 1.0); }", "ten-bit-source");
		struct fx_animation_parameters parameters = {.progress = 1, .linear_progress = 1};
		wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOW, shader, &parameters);
		struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, 16, 16, get_render_format(fixture, formats[f]));
		struct wlr_output_state native;
		wlr_output_state_init(&native);
		ok &= check(shader && swapchain && wlr_scene_output_build_state(output, &native,
			&(struct wlr_scene_output_state_options){.swapchain = swapchain}) && native.buffer, "native ten-bit reference");
		struct fx_scene_source_view view = {.first = &rect->node, .last = &rect->node,
			.extent = {0, 0, 16, 16}, .scale = 1};
		struct fx_scene_source_view_plan plan;
		ok &= check(fx_scene_source_view_plan_for_test(output, &view, &plan) && plan.floating_point && !plan.working_space
			&& fx_scene_source_floating_point(output) && !fx_scene_source_working_space(output), "ten-bit precision is independent of value encoding");
		struct fx_scene_source_pair_for_test pair = {0};
		ok &= check(fx_scene_source_view_pair_capture_for_test(output, &view, plan.total_bytes, &pair)
			&& pair.floating_point && !pair.working_space, "capture encoded FP16 ten-bit source pair");
		uint16_t half[16 * 16 * 4];
		if (pair.display) {
			ok &= check(read_buffer(fixture, pair.display, DRM_FORMAT_ABGR16161616F, 16 * 8, half), "read encoded FP16 source");
			for (unsigned x = 0; x < 16; x++) {
				ok &= check(fabsf(source_half(half[x*4]) - (512.0f+x)/1023.0f) < 0.0005f
					&& fabsf(source_half(half[x*4+1]) - 0.5f) < 0.0005f, "source retains ten-bit steps without decoding");
			}
		}
		struct wlr_buffer *mixed = fx_scene_buffer_create(fixture->renderer, fixture->allocator, 16, 16, true);
		struct fx_scene_target *target = fx_scene_target_create_with_color(fixture->renderer, mixed, false);
		struct wlr_texture *texture = pair.display ? wlr_texture_from_buffer(fixture->renderer, pair.display) : NULL;
		struct fx_scene_input input = {.texture = texture, .sample_matrix = (float[]){1,0,0,0,1,0,0,0,1}};
		struct fx_scene_sources stages = {.fragment = "vec4 transition(vec2 uv) { return mix(umbriel_sample_from(uv), umbriel_sample_to(uv), umbriel_progress); }"};
        struct fx_scene_program *program = fx_scene_program_create(fixture->renderer, &stages, NULL, 0);
        struct fx_scene_frame frame = {.output_size = {16, 16}, .scale = 1, .progress = 0.5f, .scene_count = 2};
        struct fx_scene_input inputs[2] = {input, input};
        ok &= check(target && texture && program && fx_scene_program_render(program, target, &frame, inputs), "encoded FP16 pair composition");
        fx_scene_program_unref(program);
		struct wlr_buffer *landing = create_output_buffer(fixture, formats[f], 16, 16);
		struct wlr_texture *composed = mixed ? wlr_texture_from_buffer(fixture->renderer, mixed) : NULL;
		struct wlr_render_pass *pass = landing ? wlr_renderer_begin_buffer_pass(fixture->renderer, landing, NULL) : NULL;
		if (composed && pass) {
			wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){.texture = composed,
				.dst_box = {.width = 16, .height = 16}, .blend_mode = WLR_RENDER_BLEND_MODE_NONE});
			ok &= check(wlr_render_pass_submit(pass), "present encoded FP16 into native ten-bit output");
			uint16_t reference[16*16*4], result[16*16*4];
			ok &= check(read_ten_bit(fixture, native.buffer, reference)
				&& read_ten_bit(fixture, landing, result), "read native and retained ten-bit pixels");
			for (unsigned x = 0; x < 16; x++) {
				float expected = (512.0f+x)/1023.0f;
				ok &= check(fabsf(source_half(reference[x*4])-expected) <= 1.1f/1023
					&& fabsf(source_half(result[x*4])-expected) <= 1.1f/1023,
					"native and retained ten-bit pixels match independent ramp");
			}

		} else ok = false;
		if (composed) wlr_texture_destroy(composed);
		if (texture) wlr_texture_destroy(texture);
		fx_scene_target_destroy(target);
		if (mixed) wlr_buffer_drop(mixed);
		if (landing) wlr_buffer_drop(landing);
		fx_scene_source_pair_finish_for_test(&pair);
		wlr_output_state_finish(&native);
		if (swapchain) wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(shader);
		wlr_scene_node_destroy(&scene->tree.node);
	}
	ok &= check(tested > 0, "at least one native ten-bit format exercised");
	struct wlr_output_state restore;
	wlr_output_state_init(&restore);
	wlr_output_state_set_render_format(&restore, DRM_FORMAT_XRGB8888);
	ok &= wlr_output_commit_state(fixture->output, &restore);
	wlr_output_state_finish(&restore);
	return ok;
}

static bool test_mirrored_replacement(struct fixture *fixture) {
	struct wlr_output *other = wlr_headless_add_output(fixture->backend, 16, 16);
	bool ok = other && wlr_output_init_render(other, fixture->allocator, fixture->renderer);
	if (!ok) return false;
	struct wlr_output_state enabled;
	wlr_output_state_init(&enabled);
	wlr_output_state_set_enabled(&enabled, true);
	ok &= wlr_output_commit_state(other, &enabled);
	wlr_output_state_finish(&enabled);
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *owner = wlr_scene_output_create(scene, fixture->output);
	struct wlr_scene_output *peer = wlr_scene_output_create(scene, other);
	struct wlr_scene_tree *desktop = wlr_scene_tree_create(&scene->tree);
	const float blue[] = {0, 0, 1, 1};
	struct wlr_scene_rect *client = wlr_scene_rect_create(desktop, 16, 16, blue);
	struct wlr_buffer *green = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, 16, 16);
	struct wlr_buffer *clean = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, 16, 16);
	struct wlr_buffer *native_source = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, 16, 16);
	ok &= green && clean && native_source;
	if (!ok) return false;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(fixture->renderer, green, NULL);
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){.box = {0, 0, 16, 16}, .color = {0, 1, 0, 1}});
	ok &= wlr_render_pass_submit(pass);
	struct fx_scene_source_view view = native_view(owner, &desktop->node, &desktop->node);
	ok &= fx_scene_capture_view_for_test(owner, &view, clean, false, fx_scene_source_view_bytes_for_test(owner, &view));
	pixman_region32_t visible;
	pixman_region32_init(&visible);
	pixman_region32_copy(&visible, &client->node.visible);
	struct wlr_scene_tree *presentation = wlr_scene_tree_create(&scene->tree);
	// The full output picture and its nonopaque-declared backing are one output
	// presentation subtree. Neither may cover a mirrored native output.
	wlr_scene_buffer_create(presentation, green);
	struct wlr_scene_buffer *picture = wlr_scene_buffer_create(presentation, green);
	ok &= check(fx_scene_output_replace_range_for_test(owner, &desktop->node, &desktop->node)
		&& fx_scene_output_bind_replacement_roles_for_test(owner, picture, clean), "bind mirrored output-local presentation");
	ok &= check(pixman_region32_equal(&visible, &client->node.visible), "presentation preserves native global visibility");
	struct sample_counter samples = {.listener.notify = sampled};
	wl_signal_add(&picture->events.output_sample, &samples.listener);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, 16, 16, get_render_format(fixture, DRM_FORMAT_ARGB8888));
	for (unsigned index = 0; ok && index < 2; index++) {
		struct wlr_scene_output *output = index ? peer : owner;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_scene_output_damage_whole_for_test(output);
		ok &= wlr_scene_output_build_state(output, &state, &(struct wlr_scene_output_state_options){.swapchain = swapchain});
		uint8_t pixel[4];
		ok &= state.buffer && fixture_read_display_pixel(fixture, state.buffer, 8, 8, pixel);
		ok &= check(index ? pixel[0] > 253 && pixel[1] < 2 : pixel[1] > 253 && pixel[0] < 2,
			"mirrored peer renders native blue while owning output presents green");
		wlr_output_state_finish(&state);
	}
	ok &= check(samples.count == 1, "foreign output emits no sample callback for owned picture");
	pixman_region32_clear(&peer->pending_commit_damage);
	wlr_scene_buffer_set_buffer(picture, clean);
	wlr_scene_node_set_position(&presentation->node, 1, 0);
	ok &= check(!pixman_region32_not_empty(&peer->pending_commit_damage), "owned picture buffer and geometry updates do not damage mirrored peer");
	view = native_view(peer, &desktop->node, &presentation->node);
	ok &= check(fx_scene_capture_view_for_test(peer, &view, native_source, false, fx_scene_source_view_bytes_for_test(peer, &view)),
		"independent mirrored source excludes presentation subtree");
	uint8_t source_pixel[4];
	ok &= fixture_read_pixel(fixture, native_source, 8, 8, source_pixel)
		&& check(source_pixel[0] > 253 && source_pixel[1] < 2, "source contains native content instead of recursive presentation");
	wl_list_remove(&samples.listener.link);
	pixman_region32_fini(&visible);
	wlr_swapchain_destroy(swapchain);
	wlr_scene_node_destroy(&presentation->node);
	ok &= check(!pixman_region32_not_empty(&peer->pending_commit_damage), "owned subtree destruction does not damage mirrored peer");
	ok &= check(scene->source_replacement_count_for_test == 0 && owner->source_replacement_for_test == NULL,
		"presentation subtree destruction clears output owner safely");
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_buffer_drop(green);
	wlr_buffer_drop(clean);
	wlr_buffer_drop(native_source);
	wlr_output_destroy(other);
	return ok;
}

static bool test_replacement_roles(struct fixture *fixture, bool managed) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *output = wlr_scene_output_create(scene, fixture->output);
	struct wlr_scene_tree *desktop = wlr_scene_tree_create(&scene->tree);
	const float grey[] = {0.5f, 0.5f, 0.5f, 1};
	struct wlr_scene_rect *client = wlr_scene_rect_create(desktop, 16, 16, grey);
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		managed ? "vec4 window(vec2 uv) { return vec4(2.0, 0.5, 0.125, 1.0); }"
		: "vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }", "replacement-roles");
	struct fx_animation_parameters parameters = {.progress = 1, .linear_progress = 1, .direction = 1};
	wlr_scene_node_set_animation(&client->node, FX_SLOT_WINDOW, shader, &parameters);
	struct wlr_color_transform *encoding = managed ? wlr_color_transform_init_linear_to_inverse_eotf(
		WLR_COLOR_TRANSFER_FUNCTION_SRGB) : NULL;
	if (managed) output->combined_color_transform = wlr_color_transform_ref(encoding);
	struct fx_scene_source_pair_for_test pair = {0};
	uint64_t bytes = output_pair_bytes(output);
	bool ok = check(bytes && !native_pair_capture(output, &desktop->node, &desktop->node, bytes - 1, &pair)
		&& !pair.display && !pair.unfiltered, "insufficient reservation publishes no frozen images");
	ok &= check(shader && (!managed || encoding) && bytes && native_pair_capture(output,
		&desktop->node, &desktop->node, bytes, &pair) && pair.working_space == managed,
		"capture replacement pair in output color space");
	if (managed && pair.display && pair.unfiltered) {
		uint16_t shown[16 * 16 * 4], plain[16 * 16 * 4];
		ok &= read_buffer(fixture, pair.display, DRM_FORMAT_ABGR16161616F, 16 * 8, shown)
			&& read_buffer(fixture, pair.unfiltered, DRM_FORMAT_ABGR16161616F, 16 * 8, plain);
		ok &= check(fabsf(source_half(shown[0]) - 2.0f) < 0.002f
			&& fabsf(source_half(plain[0]) - 0.214041f) < 0.002f,
			"retained roles preserve extended shader values and linearized client pixels");
	}
	struct wlr_scene_buffer *picture = pair.display ? wlr_scene_buffer_create(&scene->tree, pair.display) : NULL;
	if (picture && managed) {
		wlr_scene_buffer_set_transfer_function(picture, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR);
		wlr_scene_buffer_set_primaries(picture, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	}
	ok &= check(picture && fx_scene_output_replace_range_for_test(output, &desktop->node, &desktop->node)
		&& fx_scene_output_bind_replacement_roles_for_test(output, picture, pair.unfiltered),
		"bind retained scene-buffer roles");
	wlr_scene_output_set_effect_capture_policy(output, false);
	wlr_scene_node_destroy(&client->node);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, 16, 16,
		get_render_format(fixture, DRM_FORMAT_ARGB8888));
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_scene_output_damage_whole_for_test(output);
	ok &= check(swapchain && wlr_scene_output_build_state(output, &state,
		&(struct wlr_scene_output_state_options){.swapchain = swapchain, .effect_capture_pending = true,
			.color_transform = encoding}) && state.buffer, "present retained roles after client destruction");
	if (state.buffer) {
		uint8_t display[4], capture[4];
		ok &= fixture_read_display_pixel(fixture, state.buffer, 8, 8, display)
			&& fixture_read_pixel(fixture, state.buffer, 8, 8, capture);
		const int expected[2][3] = {{0, 255, 0}, {99, 188, 255}};
		for (unsigned c = 0; c < 3; c++) {
			ok &= check(abs(display[c] - expected[managed][c]) <= 1, "display keeps shader color with exactly one encoding");
			ok &= check(abs(capture[c] - 128) <= 1, "unfiltered capture keeps client color with exactly one encoding");
		}
	}
	wlr_output_state_finish(&state);
	if (swapchain) wlr_swapchain_destroy(swapchain);
	fx_scene_output_replace_range_for_test(output, NULL, NULL);
	fx_scene_source_pair_finish_for_test(&pair);
	wlr_scene_node_destroy(&scene->tree.node);
	fx_effect_shader_unref(shader);
	wlr_color_transform_unref(encoding);
	return ok;
}

static bool test_frozen_feedback_light(struct fixture *fixture, bool split) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *output = wlr_scene_output_create(scene, fixture->output);
	const float grey[] = {0.15f, 0.2f, 0.25f, 1}, white[] = {1, 1, 1, 1};
	struct wlr_scene_rect *background = wlr_scene_rect_create(&scene->tree, 16, 16, grey);
	struct wlr_scene_border *border = wlr_scene_border_create(&scene->tree, white, white);
	wlr_scene_border_set_geometry(border, 8, 8, 1, 0, (struct clipped_region){.area = {1, 1, 6, 6}},
		(struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	wlr_scene_node_set_position(&border->node, 4, 4);
	struct wlr_scene_tree *lights = wlr_scene_tree_create(&scene->tree);
	wlr_scene_set_effect_light_layer(scene, lights);
	struct fx_effect_shader *feedback = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv){vec4 p=umbriel_sample_previous(uv);return vec4(p.r+0.25,0.3,0.1,1.0);}",
		"frozen-feedback-light");
	struct fx_animation_parameters parameters = {.progress = 1, .linear_progress = 1, .direction = 1,
		.light = {.enabled = true, .spread = 2, .intensity = 2, .threshold = 0.05f}};
	struct fx_effect_shader *identity = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv){return umbriel_sample(uv);}", "feedback-light-role-trigger");
	wlr_scene_node_set_animation(&background->node, FX_SLOT_WINDOW, split ? identity : NULL, &parameters);
	wlr_scene_node_set_animation(&border->node, FX_SLOT_BORDER_EFFECT, feedback, &parameters);
	wlr_scene_output_set_effect_capture_policy(output, false);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, 16, 16,
		get_render_format(fixture, DRM_FORMAT_ARGB8888));
	bool ok = check(feedback && identity && swapchain, "feedback light resources");
	ok &= check(output_pair_bytes(output) == 0,
		"feedback light without represented emission explicitly declines freeze");
	for (unsigned frame = 0; ok && frame < 2; frame++) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_scene_output_damage_whole_for_test(output);
		ok &= check(wlr_scene_output_build_state(output, &state,
			&(struct wlr_scene_output_state_options){.swapchain = swapchain, .effect_capture_pending = true})
			&& state.buffer, "present native feedback border and its emission");
		ok &= check(output_pair_bytes(output) == 0,
			"uncommitted feedback emission cannot be acquired as a displayed freeze");
		ok &= check(wlr_output_commit_state(fixture->output, &state), "commit feedback emission before freeze admission");
		struct fx_scene_source_view live = native_view(output, &background->node, &lights->node);
		uint64_t history = fx_scene_source_view_history_bytes(output, &live);
		live.session = fx_scene_source_view_session_create(output, &live, history);
		struct fx_scene_source_pair_for_test independent = {0};
		ok &= check(live.session && fx_scene_source_session_begin_frame_for_test(live.session)
			&& fx_scene_source_view_pair_capture_for_test(output, &live,
				fx_scene_source_view_bytes_for_test(output, &live), &independent),
			"independent live feedback and emission cannot overwrite committed native caches");
		fx_scene_source_pair_finish_for_test(&independent);
		fx_scene_source_session_finish_frame_for_test(live.session, false);
		fx_scene_source_session_destroy_for_test(live.session);
		struct fx_scene_source_pair_for_test pair = {0};
		uint64_t bytes = output_pair_bytes(output);
		ok &= check(bytes && native_pair_capture(output,
			&background->node, &lights->node, bytes, &pair), "freeze completed border history and exact emission");
		ok &= check((pair.display == pair.unfiltered) == !split,
			"border-only native capture aliases displayed history; excluded stages retain independent roles");
		if (state.buffer && pair.display && pair.unfiltered) {
			for (int y = 0; ok && y < 16; y++) {
				for (int x = 0; ok && x < 16; x++) {
					uint8_t native[4], captured[4], frozen[4], plain[4];
					ok &= fixture_read_display_pixel(fixture, state.buffer, x, y, native)
						&& fixture_read_pixel(fixture, state.buffer, x, y, captured)
						&& fixture_read_pixel(fixture, pair.display, x, y, frozen)
						&& fixture_read_pixel(fixture, pair.unfiltered, x, y, plain);
					for (unsigned c = 0; c < 4; c++) {
						if (abs(native[c] - captured[c]) > 1 || abs(native[c] - frozen[c]) > 1 || abs(captured[c] - plain[c]) > 1) {
							fprintf(stderr, "feedback light frame%u (%d,%d) channel%u display%u/%u capture%u/%u\n",
								frame, x, y, c, native[c], frozen[c], captured[c], plain[c]);
							ok = false;
						}
					}
				}
			}
		}
		fx_scene_source_pair_finish_for_test(&pair);
		wlr_output_state_finish(&state);
	}
	if (swapchain) wlr_swapchain_destroy(swapchain);
	fx_effect_shader_unref(feedback);
	fx_effect_shader_unref(identity);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

static bool test_working_history(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *output = wlr_scene_output_create(scene, fixture->output);
	const float blue[] = {0, 0, 1, 1};
	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 16, 16, blue);
	struct fx_effect_shader *history = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { vec4 p=umbriel_sample_previous(uv); return vec4(p.r+2.0,0.0,1.0,1.0); }",
		"source-working-history");
	struct fx_effect_shader *identity = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(uv); }", "source-working-identity");
	struct fx_animation_parameters parameters = {.progress = 1, .linear_progress = 1, .direction = 1};
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOWS_IN, history, &parameters);
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOW, identity, &parameters);
	wlr_scene_output_set_effect_capture_policy(output, false);
	struct wlr_color_transform *encoding = wlr_color_transform_init_linear_to_inverse_eotf(
		WLR_COLOR_TRANSFER_FUNCTION_SRGB);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, 16, 16,
		get_render_format(fixture, DRM_FORMAT_ARGB8888));
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	bool ok = check(history && identity && encoding && swapchain && wlr_scene_output_build_state(output,
		&state, &(struct wlr_scene_output_state_options){.swapchain = swapchain,
			.color_transform = encoding, .effect_capture_pending = true}), "seed managed role histories");
	wlr_output_state_finish(&state);
	struct fx_scene_source_pair_for_test pair = {0};
	uint64_t bytes = output_pair_bytes(output);
	ok &= check(bytes > 0 && native_pair_capture(output, &rect->node, &rect->node, bytes, &pair),
		"freeze FP16 feedback roles");
	uint16_t pixels[16 * 16 * 4];
	if (pair.display && pair.unfiltered) {
		ok &= check(pair.working_space && read_buffer(fixture, pair.display, DRM_FORMAT_ABGR16161616F, 16 * 8, pixels)
			&& fabsf(source_half(pixels[0]) - 2) < 0.002f, "frozen feedback copy preserves extended FP16 value");
		ok &= check(read_buffer(fixture, pair.unfiltered, DRM_FORMAT_ABGR16161616F, 16 * 8, pixels)
			&& fabsf(source_half(pixels[0]) - 2) < 0.002f, "frozen capture feedback retains own FP16 value");
	}
	fx_scene_source_pair_finish_for_test(&pair);
  // A native slide offset must not leak into a frozen feedback endpoint.
  wlr_scene_node_set_position(&rect->node, 8, 0);
  struct fx_scene_source_root_override root = {.root = &rect->node, .offset_x = -8};
  struct fx_scene_source_view view = native_view(output, &rect->node, &rect->node);
  view.roots = &root;
  view.root_count = 1;
  bytes = fx_scene_source_frozen_pair_bytes(output, &view);
  ok &= check(bytes && fx_scene_source_pair_capture_for_test(output, &view, bytes, &pair),
      "freeze feedback with resting workspace coordinates");
  if (pair.display) {
    ok &= check(read_buffer(fixture, pair.display, DRM_FORMAT_ABGR16161616F, 16 * 8, pixels)
        && fabsf(source_half(pixels[0]) - 2) < 0.002f,
        "frozen feedback ignores native slide offset");
  }
  fx_scene_source_pair_finish_for_test(&pair);
  wlr_scene_node_set_position(&rect->node, 0, 0);
	view.roots = NULL;
  view.root_count = 0;
  struct fx_scene_source_session *session = fx_scene_source_view_session_create(output, &view, fx_scene_source_view_history_bytes(output, &view));
  view.session = session;
	struct wlr_buffer *target = create_output_buffer(fixture, DRM_FORMAT_ABGR16161616F, 16, 16);
	ok &= check(session && target && fx_scene_source_session_begin_frame_for_test(session)
		&& fx_scene_capture_view_for_test(output, &view, target, false, fx_scene_source_view_bytes_for_test(output, &view)), "advance isolated FP16 feedback source");
	if (target) {
		ok &= check(read_buffer(fixture, target, DRM_FORMAT_ABGR16161616F, 16 * 8, pixels)
			&& fabsf(source_half(pixels[0]) - 2) < 0.002f, "live FP16 history starts independently in working space");
	}
	fx_scene_source_session_finish_frame_for_test(session, false);
	fx_scene_source_session_destroy_for_test(session);
	if (target) wlr_buffer_drop(target);
	if (swapchain) wlr_swapchain_destroy(swapchain);
	wlr_color_transform_unref(encoding);
	fx_effect_shader_unref(history);
	fx_effect_shader_unref(identity);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

static bool test_frozen_output_locality(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *output = wlr_scene_output_create(scene, fixture->output);
	const float blue[] = {0,0,1,1}, white[] = {1,1,1,1};
	struct wlr_scene_rect *local = wlr_scene_rect_create(&scene->tree,16,16,blue);
	struct wlr_scene_tree *foreign = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&foreign->node,1000,0);
	struct wlr_scene_border *border = wlr_scene_border_create(foreign,white,white);
	wlr_scene_border_set_geometry(border,16,16,2,0,(struct clipped_region){0},
		(struct fx_corner_radii){0},(struct fx_corner_radii){0});
	struct wlr_scene_tree *lights = wlr_scene_tree_create(&scene->tree);
	wlr_scene_set_effect_light_layer(scene,lights);
	struct fx_effect_shader *feedback = fx_effect_shader_create(fixture->renderer,FX_EFFECT_BORDER,
		"vec4 border(vec2 uv){return umbriel_sample_previous(uv)*0.5;}","foreign-frozen-feedback");
	struct fx_animation_parameters parameters = {.progress=1,.linear_progress=1,.direction=1,
		.light={.enabled=true,.spread=2,.intensity=1,.threshold=0}};
	wlr_scene_node_set_animation(&border->node,FX_SLOT_BORDER_EFFECT,feedback,&parameters);
	uint64_t bytes = native_pair_bytes(output,&local->node,&lights->node);
	struct fx_scene_source_pair_for_test pair={0};
	bool ok = check(feedback && bytes && native_pair_capture(output,&local->node,&lights->node,bytes,&pair),
		"foreign uncommitted feedback light does not reject local native freeze");
	if(pair.display) {
		uint8_t pixel[4]; ok &= fixture_read_pixel(fixture,pair.display,8,8,pixel);
		ok &= check(pixel[0]>253 && pixel[1]<2 && pixel[2]<2,"local frozen pixels exclude foreign output stage");
	}
	fx_scene_source_pair_finish_for_test(&pair);
	wlr_scene_node_set_position(&foreign->node,0,0);
	ok &= check(native_pair_bytes(output,&local->node,&lights->node)==0,
		"same uncommitted feedback light reaching this output correctly rejects freeze");
	wlr_scene_node_destroy(&scene->tree.node);
	fx_effect_shader_unref(feedback);
	return ok;
}

int main(void) {
	struct fixture fixture;
	if (!fixture_init(&fixture)) {
		fixture_finish(&fixture);
		return 77;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, 16, 16, 60000);
	bool ok = check(wlr_output_commit_state(fixture.output, &state), "enable capture fixture output");
	wlr_output_state_finish(&state);
	ok &= test_frozen_output_locality(&fixture);
	ok &= test_unmanaged_ten_bit(&fixture);
	ok &= test_working_history(&fixture);
	ok &= test_replacement_roles(&fixture, false);
	ok &= test_replacement_roles(&fixture, true);
	ok &= test_frozen_feedback_light(&fixture, false);
	ok &= test_frozen_feedback_light(&fixture, true);
	ok &= test_mirrored_replacement(&fixture);
	fixture_finish(&fixture);
	return ok ? 0 : 1;
}
