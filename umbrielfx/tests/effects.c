// Effect program kinds, generic uniforms, and the composition slots, rendered
// on a headless output. Each case name is a meson test; the executable
// returns 77 without an FP16-capable render node.
#include "render_fixture.h"
#include "render/fx_renderer/effect.h"
#include "umbrielfx/render/effect.h"
#include "umbrielfx/render/pass.h"
#include <stdarg.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

static const char *const kSources[] = {
	[FX_EFFECT_ANIMATION] = "vec4 animation(vec2 uv) { return vec4(uv.x, umbriel_clamped_progress, 0.0, 1.0); }",
	[FX_EFFECT_BORDER] = "vec4 border(vec2 uv) { return vec4(umbriel_border_distance(uv) < 0.0 ? 1.0 : 0.0, 0.0, 0.0, 1.0); }",
	[FX_EFFECT_WINDOW] = "vec4 window(vec2 uv) { return umbriel_sample(uv).bgra; }",
	[FX_EFFECT_SCREEN] = "vec4 screen(vec2 uv) { return umbriel_sample(uv) * 0.5; }",
	[FX_EFFECT_CURSOR] = "vec4 cursor(vec2 uv) { return vec4(umbriel_pointer, 0.0, 1.0); }",
};

static bool test_kinds(struct fixture *fixture) {
	bool ok = true;
	for (int kind = FX_EFFECT_ANIMATION; kind <= FX_EFFECT_CURSOR; kind++) {
		struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, kind, kSources[kind], "kinds");
		ok &= check(shader != NULL, "every kind compiles its own entry point");
		ok &= check(shader == NULL || fx_effect_shader_kind(shader) == (enum fx_effect_kind)kind, "kind is retained");
		fx_effect_shader_unref(shader);
	}
	// A source for the wrong kind has no entry point and must be rejected, not silently accepted.
	struct fx_effect_shader *wrong = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER, kSources[FX_EFFECT_ANIMATION], "wrong-kind");
	ok &= check(wrong == NULL, "a border program without vec4 border(vec2) is rejected");
	fx_effect_shader_unref(wrong);
	return ok;
}

static bool test_reads(struct fixture *fixture) {
	struct fx_effect_shader *timed = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(uv) * (0.5 + 0.5 * sin(umbriel_time)); }", "reads-time");
	struct fx_effect_shader *still = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(uv); }", "reads-none");
	bool ok = check(timed != NULL && still != NULL, "both window programs compile");
	if (ok) {
		ok &= check(fx_effect_shader_reads(timed, "umbriel_time"), "umbriel_time is an active uniform");
		ok &= check(!fx_effect_shader_reads(still, "umbriel_time"), "an unused umbriel_time is eliminated");
		ok &= check(!fx_effect_shader_reads(still, "no_such_uniform"), "unknown names are not read");
	}
	fx_effect_shader_unref(timed);
	fx_effect_shader_unref(still);
	return ok;
}

// Renders `shader` over a 16x16 capture of a solid magenta rect and returns the
// centre pixel through `out` (B, G, R, A).
static bool render_animation(struct fixture *fixture, struct fx_effect_shader *shader,
		const struct fx_animation_parameters *parameters, int expand, uint8_t out[4]) {
	struct wlr_buffer *target = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
	if (!check(target != NULL, "target buffer")) {
		return false;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, NULL);
	if (!check(pass != NULL, "pass")) {
		wlr_buffer_drop(target);
		return false;
	}
	struct fx_gles_render_pass *fx_pass = fx_get_render_pass(pass);
	bool ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "offscreen buffers");
	ok &= check(fx_render_pass_begin_animation(fx_pass), "capture begins");
	const struct wlr_box box = { .x = 4, .y = 4, .width = 8, .height = 8 };
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
		.box = box, .color = { .r = 1, .g = 0, .b = 1, .a = 1 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
	fx_render_pass_end_animation(fx_pass, shader, parameters, &box, &box, WL_OUTPUT_TRANSFORM_NORMAL, NULL, expand);
	ok &= check(wlr_render_pass_submit(pass), "submit");
	ok &= check(fixture_read_pixel(fixture, target, 8, 8, out), "read centre");
	wlr_buffer_drop(target);
	return ok;
}

static int clamp_logs;

// Counts the binder's oversized-count message for `pal` on the oversized-count program.
static void count_clamp_logs(enum wlr_log_importance importance, const char *fmt, va_list args) {
	char message[512];
	vsnprintf(message, sizeof(message), fmt, args);
	if (strstr(message, "oversized-count") != NULL && strstr(message, "'pal'") != NULL) {
		clamp_logs++;
	}
	if (importance == WLR_ERROR) {
		fprintf(stderr, "%s\n", message);
	}
}

static int failure_logs;
static const char *failure_log_needle;

// Counts messages containing `failure_log_needle`.
static void count_failure_logs(enum wlr_log_importance importance, const char *fmt, va_list args) {
	char message[512];
	vsnprintf(message, sizeof(message), fmt, args);
	if (strstr(message, failure_log_needle) != NULL) {
		failure_logs++;
	}
}

static bool test_uniforms(struct fixture *fixture) {
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"uniform float gain; uniform vec3 tint; uniform int steps;\n"
		"vec4 animation(vec2 uv) { return vec4(tint * gain * float(steps), 1.0); }", "uniforms");
	if (!check(shader != NULL, "uniform program compiles")) {
		return false;
	}
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *gain = fx_parameters_add_uniform(&parameters, "gain", FX_UNIFORM_FLOAT, 1);
	struct fx_uniform *tint = fx_parameters_add_uniform(&parameters, "tint", FX_UNIFORM_VEC3, 1);
	struct fx_uniform *steps = fx_parameters_add_uniform(&parameters, "steps", FX_UNIFORM_INT, 1);
	bool ok = check(gain != NULL && tint != NULL && steps != NULL, "three uniforms fit");
	gain->floats[0] = 0.5f;
	tint->floats[0] = 1.0f; tint->floats[1] = 0.0f; tint->floats[2] = 0.0f;
	steps->ints[0] = 2;
	uint8_t pixel[4];
	ok &= render_animation(fixture, shader, &parameters, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[1] < 5 && pixel[0] < 5, "float, vec3 and int uniforms bind by name");

	// A type mismatch is skipped: the uniform keeps its previous value on this
	// program, so `gain` stays 0.5 from the draw above and only `tint` changes.
	struct fx_animation_parameters mismatch = parameters;
	mismatch.uniforms[0].type = FX_UNIFORM_INT;   // gain declared float
	mismatch.uniforms[0].ints[0] = 9;
	mismatch.uniforms[1].floats[0] = 0.0f; mismatch.uniforms[1].floats[2] = 1.0f;   // blue tint
	ok &= render_animation(fixture, shader, &mismatch, 0, pixel);
	ok &= check(pixel[0] > 250 && pixel[2] < 5, "a mismatched uniform is skipped while the others still bind");

	// Unknown names are ignored without failing the draw.
	struct fx_animation_parameters unknown = parameters;
	fx_parameters_add_uniform(&unknown, "missing", FX_UNIFORM_FLOAT, 1);
	ok &= render_animation(fixture, shader, &unknown, 0, pixel);
	ok &= check(pixel[2] > 250, "an unknown uniform name is ignored");
	fx_effect_shader_unref(shader);

	// The palette preamble: umbriel_palette_at wraps over the supplied entries and is transparent black without any.
	struct fx_effect_shader *palette = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return umbriel_palette_at(1.25); }", "palette");
	ok &= check(palette != NULL, "palette program compiles");
	struct fx_animation_parameters colours = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *entries = fx_parameters_add_uniform(&colours, "umbriel_palette", FX_UNIFORM_VEC4, 4);
	struct fx_uniform *count = fx_parameters_add_uniform(&colours, "umbriel_palette_count", FX_UNIFORM_INT, 1);
	ok &= check(entries != NULL && count != NULL, "palette uniforms fit");
	const float table[16] = { 1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 1,  1, 1, 0, 1 };
	memcpy(entries->floats, table, sizeof(table));
	count->ints[0] = 4;
	ok &= render_animation(fixture, palette, &colours, 0, pixel);
	// t = 1.25 wraps to 0.25 -> entry 1 (green).
	ok &= check(pixel[1] > 250 && pixel[2] < 5, "umbriel_palette_at wraps into the palette");
	struct fx_animation_parameters none = { .progress = 1, .linear_progress = 1, .direction = 1 };
	ok &= render_animation(fixture, palette, &none, 0, pixel);
	ok &= check(pixel[3] < 5, "without a palette the lookup is transparent black");
	fx_effect_shader_unref(palette);

	// A count above the program's active array size binds the active elements and is logged once per program and name.
	struct fx_effect_shader *oversized = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"uniform vec4 pal[2];\nvec4 animation(vec2 uv) { return vec4(pal[0].r, 0.0, pal[1].b, 1.0); }", "oversized-count");
	ok &= check(oversized != NULL, "oversized-count program compiles");
	struct fx_animation_parameters exact = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *pal_exact = fx_parameters_add_uniform(&exact, "pal", FX_UNIFORM_VEC4, 2);
	ok &= check(pal_exact != NULL, "a count matching the declared array size fits");
	if (pal_exact != NULL) {
		pal_exact->floats[0] = 1.0f; pal_exact->floats[3] = 1.0f; // red
	}
	ok &= render_animation(fixture, oversized, &exact, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[0] < 5, "pal[0] and pal[1] bind when count matches the declared array size");
	struct fx_animation_parameters over = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *pal_over = fx_parameters_add_uniform(&over, "pal", FX_UNIFORM_VEC4, 4);
	ok &= check(pal_over != NULL, "a count larger than the declared array size still fits fx_uniform storage");
	if (pal_over != NULL) {
		const float values[16] = { 1, 0, 0, 1,  0, 0, 1, 1,  0, 1, 0, 1,  0, 1, 0, 1 }; // red, blue, green, green
		memcpy(pal_over->floats, values, sizeof(values));
	}
	clamp_logs = 0;
	wlr_log_init(WLR_DEBUG, count_clamp_logs);
	ok &= render_animation(fixture, oversized, &over, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[0] > 250 && pixel[1] < 5, "an oversized count binds pal[0] and pal[1] from its first two values");
	ok &= render_animation(fixture, oversized, &over, 0, pixel);
	wlr_log_init(WLR_ERROR, NULL);
	ok &= check(clamp_logs == 1, "an oversized count is logged once per program and name");
	fx_effect_shader_unref(oversized);

	// A constant index folds the reads to pal[0], so drivers may report an active size of 1 for a declared pal[2].
	struct fx_effect_shader *folded = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"uniform vec4 pal[2];\nvec4 animation(vec2 uv) { return pal[0]; }", "folded-count");
	ok &= check(folded != NULL, "folded-count program compiles");
	struct fx_animation_parameters pair = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *pal_pair = fx_parameters_add_uniform(&pair, "pal", FX_UNIFORM_VEC4, 2);
	ok &= check(pal_pair != NULL, "a two-element palette fits");
	if (pal_pair != NULL) {
		pal_pair->floats[1] = 1.0f; pal_pair->floats[3] = 1.0f; // green
		pal_pair->floats[4] = 1.0f; pal_pair->floats[7] = 1.0f; // red
	}
	ok &= render_animation(fixture, folded, &pair, 0, pixel);
	ok &= check(pixel[1] > 250 && pixel[2] < 5, "count 2 binds pal[0] when only pal[0] is read");
	fx_effect_shader_unref(folded);

	// A name too long for the cache is skipped, never cached as a truncated alias of a real uniform.
	struct fx_effect_shader *long_name = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"uniform vec4 abcdefghijklmnopqrstuvwxyz0123456789ABCD;\n"
		"uniform float abcdefghijklmnopqrstuvwxyz01234;\n"
		"vec4 animation(vec2 uv) {\n"
		"  return vec4(abcdefghijklmnopqrstuvwxyz01234, 0.0, 0.0, 1.0) + abcdefghijklmnopqrstuvwxyz0123456789ABCD;\n"
		"}",
		"long-name");
	ok &= check(long_name != NULL, "long-uniform-name program compiles");
	ok &= check(!fx_effect_shader_reads(long_name, "abcdefghijklmnopqrstuvwxyz0123456789ABCD"),
		"a 40-character uniform name is not cached under a truncated alias");
	ok &= check(fx_effect_shader_reads(long_name, "abcdefghijklmnopqrstuvwxyz01234"),
		"the real 31-character uniform sharing that prefix is still cached under its own name");
	unsigned name_matches = 0;
	for (unsigned i = 0; i < long_name->uniform_count; i++) {
		if (strcmp(long_name->uniforms[i].name, "abcdefghijklmnopqrstuvwxyz01234") == 0) {
			name_matches++;
			ok &= check(long_name->uniforms[i].type == GL_FLOAT,
				"the cached entry for the 31-character name keeps its declared float type");
		}
	}
	ok &= check(name_matches == 1,
		"the 31-character name is cached exactly once, not aliased by the truncated 40-character name");
	struct fx_animation_parameters named = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *short_uniform = fx_parameters_add_uniform(&named, "abcdefghijklmnopqrstuvwxyz01234", FX_UNIFORM_FLOAT, 1);
	ok &= check(short_uniform != NULL, "the 31-character name fits fx_uniform storage");
	if (short_uniform != NULL) {
		short_uniform->floats[0] = 1.0f;
	}
	ok &= render_animation(fixture, long_name, &named, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[3] > 250, "binding the real short uniform by name is not blocked by the skipped long alias");
	fx_effect_shader_unref(long_name);

	// Hand-built entries whose count exceeds their own storage are rejected, leaving the previous binding.
	struct fx_effect_shader *bounded = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"uniform int steps[8]; uniform vec4 tints[9];\n"
		"vec4 animation(vec2 uv) { return vec4(tints[0].rgb * float(steps[0]), 1.0); }", "bounded-count");
	ok &= check(bounded != NULL, "bounded-count program compiles");
	struct fx_animation_parameters fitting = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *fitting_steps = fx_parameters_add_uniform(&fitting, "steps", FX_UNIFORM_INT, 1);
	struct fx_uniform *fitting_tints = fx_parameters_add_uniform(&fitting, "tints", FX_UNIFORM_VEC4, 1);
	ok &= check(fitting_steps != NULL && fitting_tints != NULL, "fitting entries fit");
	if (fitting_steps != NULL && fitting_tints != NULL) {
		fitting_steps->ints[0] = 1;
		fitting_tints->floats[0] = 1.0f; fitting_tints->floats[3] = 1.0f; // red
	}
	ok &= render_animation(fixture, bounded, &fitting, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[1] < 5, "fitting int and vec4 array entries bind");
	struct fx_animation_parameters too_many_ints = fitting;
	too_many_ints.uniforms[0].count = 5;   // ints[] holds 4
	too_many_ints.uniforms[0].ints[0] = 0;
	ok &= render_animation(fixture, bounded, &too_many_ints, 0, pixel);
	ok &= check(pixel[2] > 250, "an INT entry with count > 4 is rejected");
	struct fx_animation_parameters too_many_floats = fitting;
	too_many_floats.uniforms[1].count = 9;   // 36 floats; floats[] holds 32
	too_many_floats.uniforms[1].floats[0] = 0.0f; too_many_floats.uniforms[1].floats[1] = 1.0f; // green
	ok &= render_animation(fixture, bounded, &too_many_floats, 0, pixel);
	ok &= check(pixel[2] > 250 && pixel[1] < 5, "a float entry past FX_UNIFORM_FLOATS_MAX is rejected");
	fx_effect_shader_unref(bounded);
	return ok;
}

// Clears `target`, captures a green rect over `box` (buffer coordinates), and
// composites `shader` over it with `expand`.
static bool render_expand(struct fixture *fixture, struct wlr_buffer *target, struct fx_effect_shader *shader,
		const struct wlr_box *box, const struct wlr_box *logical_box, enum wl_output_transform transform, int expand) {
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(fixture->renderer, target, NULL);
	if (!check(pass != NULL, "pass")) {
		return false;
	}
	struct fx_gles_render_pass *fx_pass = fx_get_render_pass(pass);
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
		.box = { .width = TEST_WIDTH, .height = TEST_HEIGHT }, .color = { 0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
	bool ok = check(fx_render_pass_init_offscreen_buffers(pass, fixture->output), "offscreen buffers");
	ok &= check(fx_render_pass_begin_animation(fx_pass), "capture begins");
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
		.box = *box, .color = { .r = 0, .g = 1, .b = 0, .a = 1 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
	fx_render_pass_end_animation(fx_pass, shader, &parameters, box, logical_box, transform, NULL, expand);
	ok &= check(wlr_render_pass_submit(pass), "submit");
	return ok;
}

static bool painted(struct fixture *fixture, struct wlr_buffer *target, int x, int y) {
	uint8_t pixel[4];
	return fixture_read_pixel(fixture, target, x, y, pixel) && pixel[2] > 250 && pixel[3] > 250;
}

static bool blank(struct fixture *fixture, struct wlr_buffer *target, int x, int y) {
	uint8_t pixel[4];
	return fixture_read_pixel(fixture, target, x, y, pixel) && pixel[2] < 5 && pixel[3] < 5;
}

static bool test_expand(struct fixture *fixture) {
	// Solid red everywhere the program is drawn: with expand, red must reach past the node box.
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0) * umbriel_expand.x * 4.0 + umbriel_sample(uv) * 0.0; }", "expand");
	struct fx_effect_shader *scaled = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return vec4(umbriel_scale, 0.0, 0.0, 1.0) + umbriel_sample(uv) * 0.0; }", "expand-scale");
	struct wlr_buffer *target = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
	if (!check(shader != NULL && scaled != NULL && target != NULL, "expand programs compile and the target exists")) {
		fx_effect_shader_unref(shader);
		fx_effect_shader_unref(scaled);
		wlr_buffer_drop(target);
		return false;
	}
	// expand = 2 logical px on an unscaled target: the drawn box is 8x8 at (4,4), so umbriel_expand.x == 0.25.
	const struct wlr_box box = { .x = 6, .y = 6, .width = 4, .height = 4 };
	bool ok = render_expand(fixture, target, shader, &box, &box, WL_OUTPUT_TRANSFORM_NORMAL, 2);
	ok &= check(painted(fixture, target, 8, 8) && painted(fixture, target, 4, 4),
		"the program paints the node box and its expand margin");
	ok &= check(blank(fixture, target, 2, 2), "nothing is drawn past the expanded box");

	// An 8x4 node at (4,6) on a 90-degree output: its buffer box is 4x8, and the
	// 2 px margin holds on both buffer axes with umbriel_scale == 1.
	const struct wlr_box logical = { .x = 4, .y = 6, .width = 8, .height = 4 };
	struct wlr_box rotated;
	wlr_box_transform(&rotated, &logical, wlr_output_transform_invert(WL_OUTPUT_TRANSFORM_90), TEST_WIDTH, TEST_HEIGHT);
	ok &= render_expand(fixture, target, scaled, &rotated, &logical, WL_OUTPUT_TRANSFORM_90, 2);
	const int left = rotated.x - 2, right = rotated.x + rotated.width + 1;
	const int top = rotated.y - 2, bottom = rotated.y + rotated.height + 1;
	const int mid_x = rotated.x + rotated.width / 2, mid_y = rotated.y + rotated.height / 2;
	ok &= check(painted(fixture, target, mid_x, mid_y), "the rotated node box is painted with umbriel_scale 1");
	ok &= check(painted(fixture, target, left, mid_y) && painted(fixture, target, right, mid_y),
		"the rotated margin covers 2 px on the buffer x axis");
	ok &= check(painted(fixture, target, mid_x, top) && painted(fixture, target, mid_x, bottom),
		"the rotated margin covers 2 px on the buffer y axis");
	ok &= check(blank(fixture, target, left - 1, mid_y) && blank(fixture, target, right + 1, mid_y)
		&& blank(fixture, target, mid_x, top - 1) && blank(fixture, target, mid_x, bottom + 1),
		"nothing is drawn past the rotated expanded box");
	wlr_buffer_drop(target);
	fx_effect_shader_unref(shader);
	fx_effect_shader_unref(scaled);
	return ok;
}

static bool test_renderer_destroy(struct fixture *fixture) {
	int fd = fcntl(fixture->drm_fd, F_DUPFD_CLOEXEC, 0);
	struct wlr_renderer *renderer = fx_renderer_create_with_drm_fd(fd);
	if (!check(renderer != NULL, "second renderer")) {
		close(fd);
		return false;
	}
	struct fx_effect_shader *shader = fx_effect_shader_create(renderer, FX_EFFECT_WINDOW, kSources[FX_EFFECT_WINDOW], "destroy");
	bool ok = check(shader != NULL, "program on the second renderer");
	wlr_renderer_destroy(renderer);
	// The scene may still hold references after the context is gone; unref must not touch GL names.
	fx_effect_shader_unref(shader);
	return ok;
}

// A persistent slot on one node must not disturb an unrelated node's culling
// and must draw only inside its own bounds. Layout (16x16 output):
//   background: opaque blue rect covering everything
//   effect node: opaque rect 4x4 at (2,2) with a persistent (window slot) program returning green
//   bystander: opaque red rect 4x4 at (10,10), no effect
static bool test_persistent_scene(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	struct wlr_scene_rect *background = wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct wlr_scene_rect *effect = wlr_scene_rect_create(&scene->tree, 4, 4, white);
	wlr_scene_node_set_position(&effect->node, 2, 2);
	struct wlr_scene_rect *bystander = wlr_scene_rect_create(&scene->tree, 4, 4, red);
	wlr_scene_node_set_position(&bystander->node, 10, 10);
	struct fx_effect_shader *green = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }", "persistent-scene");
	bool ok = check(green != NULL, "window program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&effect->node, FX_SLOT_WINDOW, green, &parameters);
	// Only the effect's own subtree stops culling: the bystander still hides
	// the background beneath it.
	ok &= check(pixman_region32_contains_point(&background->node.visible, 3, 3, NULL),
		"the background stays visible under the effect node");
	ok &= check(!pixman_region32_contains_point(&background->node.visible, 12, 12, NULL),
		"the bystander still culls the background");

	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "scene renders with a persistent slot");
	if (rendered != NULL) {
		uint8_t at_effect[4], at_bystander[4], at_background[4];
		ok &= fixture_read_pixel(fixture, rendered, 3, 3, at_effect);
		ok &= fixture_read_pixel(fixture, rendered, 12, 12, at_bystander);
		ok &= fixture_read_pixel(fixture, rendered, 8, 2, at_background);
		ok &= check(at_effect[1] > 250 && at_effect[2] < 5, "the persistent program paints its node");
		ok &= check(at_bystander[2] > 250 && at_bystander[1] < 5, "an unrelated node is untouched");
		ok &= check(at_background[0] > 250, "the background outside the node is untouched");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	struct fx_offscreen_buffers *captured = fx_offscreen_buffers_try_get(fixture->output);
	ok &= check(captured != NULL && captured->output.in_place_source != NULL && captured->output.animation_buffers[0] == NULL,
		"the in-place window slot copies its target without a capture");

	// With the slot removed nothing keeps the scene's effect list: the next
	// render must not re-add offscreen buffers (a second render succeeds and the
	// output's fx_offscreen_buffers hold no effect buffers).
	wlr_scene_node_set_animation(&effect->node, FX_SLOT_WINDOW, NULL, NULL);
	struct wlr_output_state again;
	struct wlr_buffer *plain = fixture_render_scene(fixture, scene_output, &again);
	ok &= check(plain != NULL, "scene renders after the slot is removed");
	struct fx_offscreen_buffers *fbos = fx_offscreen_buffers_try_get(fixture->output);
	ok &= check(fbos == NULL || (fbos->output.animation_buffers[0] == NULL && fbos->output.in_place_source == NULL),
		"effect buffers are released without effects");
	if (plain != NULL) wlr_buffer_unlock(plain);
	wlr_output_state_finish(&again);

	// Scene teardown finishes the root's effect state before the nodes that
	// still carry slots.
	wlr_scene_node_set_animation(&effect->node, FX_SLOT_WINDOW, green, &parameters);
	fx_effect_shader_unref(green);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// A persistent window effect reads its input under an opaque node above it.
// Layout: blue background, a white 8x8 effect node at (4,4) whose window slot
// mirrors it horizontally, and an opaque red 8x8 rect at (8,4) over its right half.
static bool test_occlusion(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct wlr_scene_rect *effect = wlr_scene_rect_create(&scene->tree, 8, 8, white);
	wlr_scene_node_set_position(&effect->node, 4, 4);
	struct wlr_scene_rect *cover = wlr_scene_rect_create(&scene->tree, 8, 8, red);
	wlr_scene_node_set_position(&cover->node, 8, 4);
	struct fx_effect_shader *mirror = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(vec2(1.0 - uv.x, uv.y)); }", "occlusion");
	bool ok = check(mirror != NULL, "mirror program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&effect->node, FX_SLOT_WINDOW, mirror, &parameters);
	ok &= check(pixman_region32_contains_point(&effect->node.visible, 10, 8, NULL),
		"the covered half stays in the effect node's visible region");

	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "scene renders");
	if (rendered != NULL) {
		uint8_t mirrored[4], covered[4];
		ok &= fixture_read_pixel(fixture, rendered, 5, 8, mirrored);
		ok &= fixture_read_pixel(fixture, rendered, 10, 8, covered);
		ok &= check(mirrored[0] > 250 && mirrored[1] > 250 && mirrored[2] > 250,
			"the covered input is captured and mirrored into the left half");
		ok &= check(covered[2] > 250 && covered[0] < 5 && covered[1] < 5, "the red rect still covers the right half");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(mirror);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

static struct wlr_swapchain *create_swapchain(struct fixture *fixture) {
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	return format != NULL ? wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format) : NULL;
}

// Builds a frame on `swapchain`, which outlives the frame so buffer age limits
// render damage, and acknowledges its damage as a commit would. The caller
// unlocks the returned buffer and finishes `state`.
static struct wlr_buffer *render_frame_pending(struct wlr_scene_output *scene_output, struct wlr_swapchain *swapchain,
		bool capture_pending, struct wlr_output_state *state) {
	wlr_output_state_init(state);
	struct wlr_scene_output_state_options options = { .swapchain = swapchain, .effect_capture_pending = capture_pending };
	if (!wlr_scene_output_build_state(scene_output, state, &options) || state->buffer == NULL) {
		return NULL;
	}
	wlr_scene_output_acknowledge_damage_for_test(scene_output, state);
	return wlr_buffer_lock(state->buffer);
}

static struct wlr_buffer *render_frame(struct wlr_scene_output *scene_output, struct wlr_swapchain *swapchain,
		struct wlr_output_state *state) {
	return render_frame_pending(scene_output, swapchain, false, state);
}

// One whole-damage frame per swapchain buffer, so the next frame carries only its own damage.
static bool warm_up(struct wlr_scene_output *scene_output, struct wlr_swapchain *swapchain) {
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		wlr_scene_output_damage_whole_for_test(scene_output);
		struct wlr_output_state state;
		struct wlr_buffer *buffer = render_frame(scene_output, swapchain, &state);
		ok &= buffer != NULL;
		if (buffer != NULL) {
			wlr_buffer_unlock(buffer);
		}
		wlr_output_state_finish(&state);
	}
	return check(ok, "warm-up frames render");
}

// Renders one frame and checks its commit damage extents against (x1,y1)-(x2,y2), x2/y2 exclusive.
static bool frame_damage_is(struct wlr_scene_output *scene_output, struct wlr_swapchain *swapchain,
		int x1, int y1, int x2, int y2, const char *message) {
	struct wlr_output_state state;
	struct wlr_buffer *buffer = render_frame(scene_output, swapchain, &state);
	const pixman_box32_t *extents = pixman_region32_extents(&state.damage);
	bool ok = buffer != NULL && (state.committed & WLR_OUTPUT_STATE_DAMAGE)
		&& extents->x1 == x1 && extents->y1 == y1 && extents->x2 == x2 && extents->y2 == y2;
	if (!ok) {
		fprintf(stderr, "  damage (%d,%d)-(%d,%d), expected (%d,%d)-(%d,%d)\n",
			extents->x1, extents->y1, extents->x2, extents->y2, x1, y1, x2, y2);
	}
	if (buffer != NULL) {
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	return check(ok, message);
}

static const char kGainSource[] = "uniform float gain;\nvec4 window(vec2 uv) { return umbriel_sample(uv) * gain; }";

// A persistent parameter change damages only its own node's box.
static bool test_damage_confinement(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW, kGainSource, "gain");
	if (!check(swapchain != NULL && shader != NULL, "swapchain and gain program")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(shader);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float white[4] = { 1, 1, 1, 1 };
	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 6, 6, white);
	wlr_scene_node_set_position(&rect->node, 5, 5);
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *gain = fx_parameters_add_uniform(&parameters, "gain", FX_UNIFORM_FLOAT, 1);
	gain->floats[0] = 1.0f;
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOW, shader, &parameters);
	bool ok = warm_up(scene_output, swapchain);
	gain->floats[0] = 0.5f;
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOW, shader, &parameters);
	ok &= frame_damage_is(scene_output, swapchain, 5, 5, 11, 11, "a uniform change damages only the effect box");
	fx_effect_shader_unref(shader);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// Damage touching a persistent effect box grows to the whole box, and on to
// every box the grown damage touches.
static bool test_whole_box_invalidation(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW, kGainSource, "gain");
	if (!check(swapchain != NULL && shader != NULL, "swapchain and gain program")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(shader);
		return false;
	}
	const float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	struct fx_uniform *gain = fx_parameters_add_uniform(&parameters, "gain", FX_UNIFORM_FLOAT, 1);
	gain->floats[0] = 1.0f;

	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 6, 6, white);
	wlr_scene_node_set_position(&rect->node, 5, 5);
	struct wlr_scene_rect *marker = wlr_scene_rect_create(&scene->tree, 1, 1, red);
	wlr_scene_node_set_position(&marker->node, 6, 6);
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOW, shader, &parameters);
	bool ok = warm_up(scene_output, swapchain);
	wlr_scene_node_set_position(&marker->node, 7, 7);
	ok &= frame_damage_is(scene_output, swapchain, 5, 5, 11, 11, "damage inside an effect box covers the whole box");
	wlr_scene_node_destroy(&scene->tree.node);

	// A at (2,2) and B at (6,6), both 6x6, overlap at (6,6)-(8,8); the marker at (3,3) is in A only.
	scene = wlr_scene_create();
	scene_output = wlr_scene_output_create(scene, fixture->output);
	struct wlr_scene_rect *a = wlr_scene_rect_create(&scene->tree, 6, 6, white);
	wlr_scene_node_set_position(&a->node, 2, 2);
	struct wlr_scene_rect *b = wlr_scene_rect_create(&scene->tree, 6, 6, white);
	wlr_scene_node_set_position(&b->node, 6, 6);
	marker = wlr_scene_rect_create(&scene->tree, 1, 1, red);
	wlr_scene_node_set_position(&marker->node, 3, 3);
	wlr_scene_node_set_animation(&a->node, FX_SLOT_WINDOW, shader, &parameters);
	wlr_scene_node_set_animation(&b->node, FX_SLOT_WINDOW, shader, &parameters);
	ok &= warm_up(scene_output, swapchain);
	const float green[4] = { 0, 1, 0, 1 };
	wlr_scene_rect_set_color(marker, green);
	ok &= frame_damage_is(scene_output, swapchain, 2, 2, 12, 12, "damage in A grows through A to the overlapping B");
	wlr_scene_node_destroy(&scene->tree.node);
	fx_effect_shader_unref(shader);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// A transient slot keeps whole-output damage every frame and never culls what its node covers.
static bool test_transient_policy(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *identity = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return umbriel_sample(uv); }", "transient-identity");
	struct fx_effect_shader *clear = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return umbriel_sample(uv) * 0.0; }", "transient-clear");
	bool ok = check(swapchain != NULL && identity != NULL && clear != NULL, "swapchain and transient programs");
	if (!ok) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(identity);
		fx_effect_shader_unref(clear);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	struct wlr_scene_rect *background = wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 8, 8, red);
	wlr_scene_node_set_position(&rect->node, 4, 4);
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 1 };
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOWS_IN, identity, &parameters);
	ok &= warm_up(scene_output, swapchain);
	ok &= frame_damage_is(scene_output, swapchain, 0, 0, TEST_WIDTH, TEST_HEIGHT,
		"an unchanged frame with a transient slot is damaged whole");
	ok &= check(pixman_region32_contains_point(&background->node.visible, 8, 8, NULL),
		"the background under the transient node is not culled");

	// A program that drops its input shows whatever was drawn beneath the node.
	parameters.transition_id = 2;
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_WINDOWS_IN, clear, &parameters);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = render_frame(scene_output, swapchain, &state);
	ok &= check(rendered != NULL, "transient frame renders");
	if (rendered != NULL) {
		uint8_t under[4];
		ok &= fixture_read_pixel(fixture, rendered, 8, 8, under);
		ok &= check(under[0] > 250 && under[2] < 5, "the covered background was drawn into the target");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(identity);
	fx_effect_shader_unref(clear);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

enum margin_removal { MARGIN_DISABLE, MARGIN_DISABLE_PARENT, MARGIN_DESTROY };

// Disabling or destroying a node damages its drawn box including the expand margin.
static bool test_margin_damage(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return umbriel_sample(uv); }", "margin");
	bool ok = check(swapchain != NULL && shader != NULL, "swapchain and drag program");
	if (!ok) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(shader);
		return false;
	}
	static const char *const kNames[] = {
		[MARGIN_DISABLE] = "disabling the node damages its expand margin",
		[MARGIN_DISABLE_PARENT] = "disabling an ancestor damages a descendant's expand margin",
		[MARGIN_DESTROY] = "destroying the node damages its expand margin",
	};
	for (int removal = MARGIN_DISABLE; removal <= MARGIN_DESTROY; removal++) {
		struct wlr_scene *scene = wlr_scene_create();
		struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
		struct wlr_scene_tree *parent = wlr_scene_tree_create(&scene->tree);
		const float white[4] = { 1, 1, 1, 1 };
		struct wlr_scene_rect *rect = wlr_scene_rect_create(parent, 6, 6, white);
		wlr_scene_node_set_position(&rect->node, 5, 5);
		struct fx_animation_parameters parameters = {
			.progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 1, .expand = 3 };
		wlr_scene_node_set_animation(&rect->node, FX_SLOT_DRAG, shader, &parameters);
		ok &= warm_up(scene_output, swapchain);
		// A disabled node's slot is cleared too, so the frame carries no transient whole-output damage.
		switch (removal) {
		case MARGIN_DISABLE:
			wlr_scene_node_set_enabled(&rect->node, false);
			wlr_scene_node_set_animation(&rect->node, FX_SLOT_DRAG, NULL, NULL);
			break;
		case MARGIN_DISABLE_PARENT:
			wlr_scene_node_set_enabled(&parent->node, false);
			wlr_scene_node_set_animation(&rect->node, FX_SLOT_DRAG, NULL, NULL);
			break;
		case MARGIN_DESTROY:
			wlr_scene_node_destroy(&rect->node);
			break;
		}
		ok &= frame_damage_is(scene_output, swapchain, 2, 2, 14, 14, kNames[removal]);
		wlr_scene_node_destroy(&scene->tree.node);
	}
	fx_effect_shader_unref(shader);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// Moving an ancestor damages the expand margin its descendant drew at the old position.
static bool test_move_margin_damage(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv) { return vec4(1.0); }", "move-margin");
	bool ok = check(swapchain != NULL && shader != NULL, "swapchain and border program");
	if (!ok) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(shader);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	struct wlr_scene_tree *frame = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&frame->node, 4, 5);
	const float white[4] = { 1, 1, 1, 1 };
	struct wlr_scene_rect *rect = wlr_scene_rect_create(frame, 6, 6, white);
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1, .expand = 3 };
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_BORDER_EFFECT, shader, &parameters);
	ok &= warm_up(scene_output, swapchain);
	// Drawn box (1,2)-(13,14) before the move and (3,2)-(15,14) after it.
	wlr_scene_node_set_position(&frame->node, 6, 5);
	ok &= frame_damage_is(scene_output, swapchain, 1, 2, 15, 14, "moving the parent damages the old and new margins");
	wlr_scene_node_destroy(&scene->tree.node);
	fx_effect_shader_unref(shader);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// A node wholly outside the output still draws the part of its expand margin that reaches the output, for a
// persistent border effect and a transient drag slot alike. Layout (16x16 output): opaque blue background; a 6x6
// rect at (-6,5) whose margin of 3 reaches columns 0-2.
static bool test_offscreen_margin(struct fixture *fixture) {
	struct fx_effect_shader *border = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }", "offscreen-border");
	struct fx_effect_shader *drag = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }", "offscreen-drag");
	bool ok = check(border != NULL && drag != NULL, "border and drag programs compile");
	const struct {
		unsigned slot;
		struct fx_effect_shader *shader;
		const char *name;
	} cases[] = {
		{ FX_SLOT_BORDER_EFFECT, border, "a border effect margin reaches the output from a node outside it" },
		{ FX_SLOT_DRAG, drag, "a drag margin reaches the output from a node outside it" },
	};
	const bool compiled = ok;
	for (size_t i = 0; compiled && i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct wlr_scene *scene = wlr_scene_create();
		struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
		const float blue[4] = { 0, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 };
		wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
		struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 6, 6, white);
		wlr_scene_node_set_position(&rect->node, -6, 5);
		struct fx_animation_parameters parameters = {
			.progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 1, .expand = 3 };
		wlr_scene_node_set_animation(&rect->node, cases[i].slot, cases[i].shader, &parameters);
		struct wlr_output_state state;
		struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
		ok &= check(rendered != NULL, "scene renders with a node outside the output");
		if (rendered != NULL) {
			uint8_t margin[4], beyond[4];
			ok &= fixture_read_pixel(fixture, rendered, 1, 8, margin);
			ok &= fixture_read_pixel(fixture, rendered, 4, 8, beyond);
			ok &= check(margin[2] > 250 && margin[0] < 5, cases[i].name);
			ok &= check(beyond[0] > 250 && beyond[2] < 5, "the margin ends at its expand");
			wlr_buffer_unlock(rendered);
		}
		wlr_output_state_finish(&state);
		wlr_scene_node_destroy(&scene->tree.node);
	}
	fx_effect_shader_unref(border);
	fx_effect_shader_unref(drag);
	return ok;
}

// A node's effect bounds are its enabled leaves' extents in node-local coordinates, and its animation expand is
// the largest expand among its own slots.
static bool test_effect_bounds(struct fixture *fixture) {
	struct fx_effect_shader *shader = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return umbriel_sample(uv); }", "effect-bounds");
	bool ok = check(shader != NULL, "program compiles");
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&tree->node, 7, 7);
	struct wlr_box box;
	ok &= check(!wlr_scene_node_effect_bounds(&tree->node, &box) && box.width == 0, "an empty tree has no bounds");
	const float white[4] = { 1, 1, 1, 1 };
	struct wlr_scene_rect *ring = wlr_scene_rect_create(tree, 10, 8, white);
	wlr_scene_node_set_position(&ring->node, -2, -1);
	struct wlr_scene_rect *hidden = wlr_scene_rect_create(tree, 4, 4, white);
	wlr_scene_node_set_position(&hidden->node, 9, 9);
	wlr_scene_node_set_enabled(&hidden->node, false);
	ok &= check(wlr_scene_node_effect_bounds(&tree->node, &box)
		&& box.x == -2 && box.y == -1 && box.width == 10 && box.height == 8,
		"bounds are node-local and skip disabled leaves");
	ok &= check(wlr_scene_node_animation_expand(&tree->node) == 0, "no slots, no expand");
	struct fx_animation_parameters parameters = {
		.progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 1, .expand = 5 };
	wlr_scene_node_set_animation(&tree->node, FX_SLOT_DRAG, shader, &parameters);
	parameters.expand = 9;
	wlr_scene_node_set_animation(&tree->node, FX_SLOT_WINDOWS_IN, shader, &parameters);
	ok &= check(wlr_scene_node_animation_expand(&tree->node) == 5, "only expanding slots count");
	wlr_scene_node_destroy(&scene->tree.node);
	fx_effect_shader_unref(shader);
	return ok;
}

// A border program sees the client hole through umbriel_border_hole and
// umbriel_border_distance, and its result is cut out of the hole.
static bool test_border_geometry(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	// 12x12 ring at (2,2) with a 2px wall: the hole is 8x8 at (2,2) node-local.
	struct wlr_scene_border *border = wlr_scene_border_create(&scene->tree, white, white);
	wlr_scene_border_set_geometry(border, 12, 12, 2, 0,
		(struct clipped_region){ .area = { 2, 2, 8, 8 } }, (struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	wlr_scene_node_set_position(&border->node, 2, 2);
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0) * step(0.0, umbriel_border_distance(uv)); }", "border-geometry");
	bool ok = check(program != NULL, "border program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&border->node, FX_SLOT_BORDER_EFFECT, program, &parameters);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "renders");
	if (rendered != NULL) {
		uint8_t ring[4], hole[4], outside[4];
		ok &= fixture_read_pixel(fixture, rendered, 3, 8, ring);      // inside the 2px wall
		ok &= fixture_read_pixel(fixture, rendered, 8, 8, hole);      // hole centre
		ok &= fixture_read_pixel(fixture, rendered, 0, 0, outside);   // past the node
		ok &= check(ring[0] > 250 && ring[2] < 5, "the ring is blue where the distance is positive");
		ok &= check(hole[0] < 5 && hole[1] < 5 && hole[2] < 5, "the hole is cut out of the result");
		ok &= check(outside[0] < 5, "nothing draws past the border box");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	// Drawn again without geometry, the program sees no hole: the centre the
	// last composite cut is painted.
	uint8_t centre[4];
	ok &= render_animation(fixture, program, &parameters, 0, centre);
	ok &= check(centre[0] > 250 && centre[3] > 250, "a draw without geometry does not reuse the previous hole");
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// A view's border tree: the ring sits at a negative offset inside the tree, a
// sibling widens the tree's bounds past the ring, and the slot expands the
// drawn box. Absolute layout (16x16 output):
//   tree at (4,4); 1x1 rect child at (-3,-3) -> bounds start at (1,1)
//   12x13 border child at (-2,-2) -> (2,2); hole {2,3,8,8} -> x [4,12), y [5,13)
//   bottom-right hole radius 4, every other corner square; expand 2
static bool test_border_geometry_tree(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_tree *tree = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&tree->node, 4, 4);
	struct wlr_scene_rect *sibling = wlr_scene_rect_create(tree, 1, 1, white);
	wlr_scene_node_set_position(&sibling->node, -3, -3);
	struct wlr_scene_border *border = wlr_scene_border_create(tree, white, white);
	wlr_scene_border_set_geometry(border, 12, 13, 2, 0,
		(struct clipped_region){ .area = { 2, 3, 8, 8 }, .corners = { .bottom_right = 4 } },
		(struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	wlr_scene_node_set_position(&border->node, -2, -2);
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0) * step(0.0, umbriel_border_distance(uv)); }",
		"border-geometry-tree");
	bool ok = check(program != NULL, "border program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1, .expand = 2 };
	wlr_scene_node_set_animation(&tree->node, FX_SLOT_BORDER_EFFECT, program, &parameters);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "renders");
	if (rendered != NULL) {
		uint8_t offset[4], shift[4], inside[4], rounded[4], square[4];
		ok &= fixture_read_pixel(fixture, rendered, 3, 9, offset);
		ok &= fixture_read_pixel(fixture, rendered, 2, 9, shift);
		ok &= fixture_read_pixel(fixture, rendered, 11, 9, inside);
		ok &= fixture_read_pixel(fixture, rendered, 11, 12, rounded);
		ok &= fixture_read_pixel(fixture, rendered, 4, 5, square);
		ok &= check(offset[0] > 250, "the hole starts at the ring's offset within the tree bounds");
		ok &= check(shift[0] > 250, "the hole moves with the expanded drawn box");
		ok &= check(inside[0] < 5, "the hole's right column is cut");
		ok &= check(rounded[0] > 250, "the bottom-right hole corner is rounded");
		ok &= check(square[0] < 5, "the top-left hole corner stays square");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// A ring whose border program emits only along its left `edge` logical px.
struct light_ring {
	struct wlr_box box; // logical
	int wall;           // logical
	float scale;
	enum wl_output_transform transform;
	float spread;
};

static const char kLeftEdgeSource[] =
	"uniform float edge;\n"
	"vec4 border(vec2 uv) { return uv.x * umbriel_size.x < edge ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0); }";

// The buffer pixel under logical pixel (x, y) of a 16x16 output.
static void light_probe_at(const struct light_ring *ring, int x, int y, int *bx, int *by) {
	struct wlr_box box = { .x = x, .y = y, .width = 1, .height = 1 };
	struct wlr_box scaled = { .x = (int)(box.x * ring->scale), .y = (int)(box.y * ring->scale),
		.width = (int)ring->scale, .height = (int)ring->scale };
	wlr_box_transform(&box, &scaled, wlr_output_transform_invert(ring->transform), TEST_WIDTH, TEST_HEIGHT);
	*bx = box.x;
	*by = box.y;
}

// Renders `ring` over black, with a light layer when `with_layer`, and reads
// the buffer pixels under the logical `probes`.
static bool render_light_ring(struct fixture *fixture, const struct light_ring *ring, bool with_layer,
		const int probes[][2], int probe_count, uint8_t out[][4]) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_border *border = wlr_scene_border_create(&scene->tree, white, white);
	wlr_scene_border_set_geometry(border, ring->box.width, ring->box.height, ring->wall, 0,
		(struct clipped_region){ .area = { ring->wall, ring->wall,
			ring->box.width - 2 * ring->wall, ring->box.height - 2 * ring->wall } },
		(struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	wlr_scene_node_set_position(&border->node, ring->box.x, ring->box.y);
	if (with_layer) {
		wlr_scene_set_effect_light_layer(scene, wlr_scene_tree_create(&scene->tree));
	}
	struct fx_effect_shader *program =
		fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER, kLeftEdgeSource, "border-light");
	bool ok = check(program != NULL, "border program compiles");
	struct fx_animation_parameters parameters = {
		.progress = 1, .linear_progress = 1, .direction = 1,
		.light = { .enabled = true, .spread = ring->spread, .intensity = 4, .threshold = 0.1f },
	};
	fx_parameters_add_uniform(&parameters, "edge", FX_UNIFORM_FLOAT, 1)->floats[0] = ring->wall;
	wlr_scene_node_set_animation(&border->node, FX_SLOT_BORDER_EFFECT, program, &parameters);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, ring->scale);
	wlr_output_state_set_transform(&state, ring->transform);
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain =
		format != NULL ? wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format) : NULL;
	struct wlr_buffer *rendered = NULL;
	struct wlr_scene_output_state_options options = { .swapchain = swapchain };
	if (swapchain != NULL && wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL) {
		rendered = wlr_buffer_lock(state.buffer);
	}
	ok &= check(rendered != NULL, "renders");
	if (rendered != NULL) {
		for (int i = 0; i < probe_count; i++) {
			int x, y;
			light_probe_at(ring, probes[i][0], probes[i][1], &x, &y);
			ok &= fixture_read_pixel(fixture, rendered, x, y, out[i]);
		}
		wlr_buffer_unlock(rendered);
	}
	wlr_swapchain_destroy(swapchain);
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Light from a border slot spills past the border box on the emitting side
// only, into the light layer, only when the scene has one. The glow keeps its
// place on a scaled output and the same logical profile on a rotated one.
static bool test_border_light(struct fixture *fixture) {
	enum { SPILL, RING, FAR, ABOVE, CORNER, PROBES };
	uint8_t lit[PROBES][4], dark[PROBES][4];
	const struct light_ring square = { .box = { 4, 4, 8, 8 }, .wall = 2, .scale = 1, .spread = 3 };
	// 2 px left of the ring, inside its left wall, 2 px right of the ring.
	const int square_probes[FAR + 1][2] = { { 2, 8 }, { 5, 8 }, { 13, 8 } };
	bool ok = render_light_ring(fixture, &square, true, square_probes, FAR + 1, lit);
	ok &= render_light_ring(fixture, &square, false, square_probes, FAR + 1, dark);
	ok &= check(lit[RING][2] > 250 && dark[RING][2] > 250, "the emitting wall is red with and without light");
	ok &= check(lit[SPILL][2] > 20, "light spills red past the emitting side");
	ok &= check(lit[SPILL][2] < lit[RING][2], "the spill is dimmer than the ring");
	ok &= check(lit[FAR][2] < 8, "no light past the side that emits nothing");
	ok &= check(dark[SPILL][2] < 5 && dark[SPILL][1] < 5, "without a light layer nothing spills");

	// The same buffer layout at scale 2: a 4x4 logical ring with 1 px walls.
	const struct light_ring scaled = { .box = { 2, 2, 4, 4 }, .wall = 1, .scale = 2, .spread = 3 };
	const int scaled_probes[FAR + 1][2] = { { 1, 4 }, { 2, 4 }, { 7, 4 } };
	ok &= render_light_ring(fixture, &scaled, true, scaled_probes, FAR + 1, lit);
	ok &= check(lit[RING][2] > 250, "the scaled emitting wall is red");
	ok &= check(lit[SPILL][2] > 20 && lit[SPILL][2] < lit[RING][2], "light spills past the scaled ring's emitting side");
	ok &= check(lit[FAR][2] < 8, "no light past the scaled ring's dark side");

	// A wide ring glows the same on a 90-degree output as on a normal one.
	const struct light_ring wide = { .box = { 2, 6, 12, 4 }, .wall = 1, .scale = 1, .spread = 4 };
	struct light_ring rotated = wide;
	rotated.transform = WL_OUTPUT_TRANSFORM_90;
	const int wide_probes[PROBES][2] = { { 1, 8 }, { 2, 8 }, { 15, 8 }, { 2, 3 }, { 0, 4 } };
	ok &= render_light_ring(fixture, &wide, true, wide_probes, PROBES, lit);
	ok &= render_light_ring(fixture, &rotated, true, wide_probes, PROBES, dark);
	ok &= check(dark[RING][2] > 250, "the rotated emitting wall is red");
	ok &= check(lit[SPILL][2] > 12 && dark[SPILL][2] > 12, "light spills past the wide ring's emitting side");
	ok &= check(lit[FAR][2] < 8 && dark[FAR][2] < 8, "no light past the wide ring's dark side");
	bool same = true;
	for (int i = 0; i < PROBES; i++) {
		same &= abs(lit[i][2] - dark[i][2]) <= 4;
	}
	ok &= check(same, "the rotated glow matches the normal one");
	return ok;
}

// The single light proxy in `layer`, or NULL when it holds none.
static struct wlr_scene_rect *light_proxy(struct wlr_scene_tree *layer) {
	if (wl_list_length(&layer->children) != 1) {
		return NULL;
	}
	struct wlr_scene_node *node = wl_container_of(layer->children.next, node, link);
	return node->type == WLR_SCENE_NODE_RECT ? wlr_scene_rect_from_node(node) : NULL;
}

static bool light_proxy_is(struct wlr_scene_tree *layer, int x, int y, int width, int height) {
	struct wlr_scene_rect *rect = light_proxy(layer);
	return rect != NULL && rect->node.x == x && rect->node.y == y && rect->width == width && rect->height == height;
}

// The red channel at (x, y) after a whole-damage frame, or -1 when the frame
// could not be rendered, so a "no light" check cannot pass on a failed render.
static int light_spill_red(struct fixture *fixture, struct wlr_scene_output *scene_output, int x, int y) {
	uint8_t pixel[4] = { 0 };
	wlr_scene_output_damage_whole_for_test(scene_output);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	int red = -1;
	if (rendered != NULL) {
		fixture_read_pixel(fixture, rendered, x, y, pixel);
		wlr_buffer_unlock(rendered);
		red = pixel[2];
	}
	wlr_output_state_finish(&state);
	return red;
}

static bool light_spill_absent(struct fixture *fixture, struct wlr_scene_output *scene_output, int x, int y) {
	const int red = light_spill_red(fixture, scene_output, x, y);
	return red >= 0 && red < 5;
}

// A view-like border tree in a window tree below the light layer. The proxy
// covers the tree's bounds plus ceil(spread * 2 + 8) logical px, follows its
// placement, and exists only while the rules of the light layer allow it.
static bool test_border_light_lifecycle(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_tree *window = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_tree *layer = wlr_scene_tree_create(&scene->tree);
	struct wlr_scene_tree *frame = wlr_scene_tree_create(window);
	wlr_scene_node_set_position(&frame->node, 4, 4);
	struct wlr_scene_border *border = wlr_scene_border_create(frame, white, white);
	const struct clipped_region hole = { .area = { 2, 2, 4, 4 } };
	wlr_scene_border_set_geometry(border, 8, 8, 2, 0, hole, (struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	struct fx_effect_shader *program =
		fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER, kLeftEdgeSource, "border-light-lifecycle");
	bool ok = check(program != NULL, "border program compiles");
	struct fx_animation_parameters parameters = {
		.progress = 1, .linear_progress = 1, .direction = 1,
		.light = { .enabled = true, .spread = 3, .intensity = 4, .threshold = 0.1f },
	};
	fx_parameters_add_uniform(&parameters, "edge", FX_UNIFORM_FLOAT, 1)->floats[0] = 2;
	wlr_scene_set_effect_light_layer(scene, layer);
	wlr_scene_node_set_animation(&frame->node, FX_SLOT_BORDER_EFFECT, program, &parameters);
	ok &= check(light_proxy_is(layer, -10, -10, 36, 36), "the proxy covers the bounds plus the margin");
	ok &= check(light_spill_red(fixture, scene_output, 2, 8) > 20, "the proxy draws the light");

	wlr_scene_border_set_geometry(border, 10, 8, 2, 0, hole, (struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	ok &= check(light_proxy_is(layer, -10, -10, 38, 36), "the proxy follows a resized border child");
	wlr_scene_node_set_position(&window->node, 1, 0);
	ok &= check(light_proxy_is(layer, -9, -10, 38, 36), "the proxy follows a moved ancestor");

	struct wlr_scene_border *snapshot = wlr_scene_border_create(window, white, white);
	wlr_scene_border_set_geometry(snapshot, 8, 8, 2, 0, hole, (struct fx_corner_radii){0}, (struct fx_corner_radii){0});
	wlr_scene_node_copy_animations_for_snapshot(&snapshot->node, &frame->node);
	ok &= check(light_proxy(layer) != NULL, "a snapshot adds no light");
	wlr_scene_node_destroy(&snapshot->node);

	struct wlr_scene_rect *kept = light_proxy(layer);
	const struct fx_animation_parameters opening = { .transition_id = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOWS_IN, program, &opening);
	ok &= check(kept != NULL && light_proxy(layer) == kept && !kept->node.enabled,
		"a transient ancestor disables the proxy without destroying it");
	ok &= check(light_spill_absent(fixture, scene_output, 3, 8), "nothing spills under a transient ancestor");
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOWS_IN, NULL, NULL);
	ok &= check(light_proxy_is(layer, -9, -10, 38, 36) && light_proxy(layer) == kept && kept->node.enabled,
		"the same proxy returns when the ancestor settles");

	wlr_scene_node_raise_to_top(&window->node);
	ok &= check(wl_list_empty(&layer->children), "a border above the light layer emits nothing");
	ok &= check(light_spill_absent(fixture, scene_output, 3, 8), "nothing spills from above the layer");
	wlr_scene_node_raise_to_top(&layer->node);
	ok &= check(light_proxy(layer) != NULL, "raising the layer restores the light");

	struct wlr_scene_tree *other = wlr_scene_tree_create(&scene->tree);
	wlr_scene_set_effect_light_layer(scene, other);
	ok &= check(wl_list_empty(&layer->children), "switching layers empties the old one");
	ok &= check(light_proxy_is(other, -9, -10, 38, 36), "the new layer holds the proxy");
	wlr_scene_set_effect_light_layer(scene, NULL);
	ok &= check(wl_list_empty(&other->children), "unregistering the layer removes the proxy");
	ok &= check(light_spill_absent(fixture, scene_output, 3, 8), "no light without a layer");

	wlr_scene_set_effect_light_layer(scene, other);
	ok &= check(light_proxy(other) != NULL, "registering again restores the proxy");
	wlr_scene_node_destroy(&other->node);
	wlr_scene_node_set_position(&window->node, 2, 0);
	wlr_scene_node_set_animation(&frame->node, FX_SLOT_BORDER_EFFECT, NULL, NULL);
	wlr_scene_set_effect_light_layer(scene, layer);
	ok &= check(wl_list_empty(&layer->children), "no proxy without a border slot");
	wlr_scene_node_set_animation(&frame->node, FX_SLOT_BORDER_EFFECT, program, &parameters);
	ok &= check(light_proxy(layer) != NULL, "a new border slot gets a proxy");

	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// The visibility query tests a subtree's leaf visible regions against a layout box.
static bool test_visible_in_box(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	wlr_scene_output_create(scene, fixture->output);
	const float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	struct wlr_scene_tree *frame = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&frame->node, 4, 4);
	struct wlr_scene_rect *leaf = wlr_scene_rect_create(frame, 8, 8, white);
	const struct wlr_box output = { 0, 0, TEST_WIDTH, TEST_HEIGHT };
	const struct wlr_box right = { 10, 0, 6, TEST_HEIGHT };
	const struct wlr_box beyond = { TEST_WIDTH, 0, TEST_WIDTH, TEST_HEIGHT };
	bool ok = check(wlr_scene_node_visible_in_box(&frame->node, &output), "a tree with a visible leaf is visible");
	ok &= check(wlr_scene_node_visible_in_box(&leaf->node, &right), "a leaf partly inside the box is visible");
	ok &= check(!wlr_scene_node_visible_in_box(&frame->node, &beyond), "nothing is visible outside the box");
	struct wlr_scene_rect *cover = wlr_scene_rect_create(&scene->tree, 6, TEST_HEIGHT, red);
	wlr_scene_node_set_position(&cover->node, 10, 0);
	ok &= check(!wlr_scene_node_visible_in_box(&frame->node, &right), "an occluded part is not visible");
	ok &= check(wlr_scene_node_visible_in_box(&frame->node, &output), "the uncovered part still is");
	wlr_scene_node_set_enabled(&frame->node, false);
	ok &= check(!wlr_scene_node_visible_in_box(&leaf->node, &output), "a leaf under a disabled tree is not visible");
	ok &= check(!wlr_scene_node_visible_in_box(NULL, &output), "a null node is not visible");
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// An in-place window program reads what is already on the target (the blue
// background through a translucent client) and rewrites only its own
// rectangle, keeping the corner fringe.
static bool test_in_place(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 }, quarter_red[4] = { 0.25f, 0, 0, 0.25f };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct wlr_scene_rect *window = wlr_scene_rect_create(&scene->tree, 8, 8, quarter_red);
	wlr_scene_node_set_position(&window->node, 4, 4);
	wlr_scene_rect_set_corner_radius(window, 3);
	// Swap red and blue of whatever is under the window: 0.25 red over blue becomes 0.75 red, 0.25 blue.
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(uv).bgra; }", "in-place");
	bool ok = check(program != NULL, "window program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, program, &parameters);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "renders");
	if (rendered != NULL) {
		uint8_t centre[4], corner[4], outside[4];
		ok &= fixture_read_pixel(fixture, rendered, 8, 8, centre);
		ok &= fixture_read_pixel(fixture, rendered, 4, 4, corner);
		ok &= fixture_read_pixel(fixture, rendered, 2, 2, outside);
		// Under the window: 0.25 red + 0.75 blue, swapped. Unswapped would be the reverse, a capture pure blue.
		ok &= check(centre[2] > 170 && centre[2] < 210 && centre[0] > 45 && centre[0] < 85,
			"the program read the backdrop through the translucent window");
		ok &= check(corner[0] > 250 && corner[2] < 5, "the rounded corner keeps the untouched background");
		ok &= check(outside[0] > 250 && outside[2] < 5, "nothing outside the window changes");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	// When the target cannot be copied the window keeps its plain rendering, and the failure is logged once.
	fx_renderer_fail_target_copies_for_test(fixture->renderer, true);
	failure_logs = 0;
	failure_log_needle = "in-place effect";
	wlr_log_init(WLR_DEBUG, count_failure_logs);
	for (int frame = 0; frame < 2; frame++) {
		wlr_scene_output_damage_whole_for_test(scene_output);
		rendered = fixture_render_scene(fixture, scene_output, &state);
		ok &= check(rendered != NULL, "renders with a failed target copy");
		if (rendered != NULL) {
			uint8_t centre[4];
			ok &= fixture_read_pixel(fixture, rendered, 8, 8, centre);
			ok &= check(centre[2] > 45 && centre[2] < 85 && centre[0] > 170 && centre[0] < 210,
				"a failed copy leaves the window as drawn");
			wlr_buffer_unlock(rendered);
		}
		wlr_output_state_finish(&state);
	}
	wlr_log_init(WLR_ERROR, NULL);
	fx_renderer_fail_target_copies_for_test(fixture->renderer, false);
	ok &= check(failure_logs == 1, "a repeated copy failure is logged once");
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// A mirror program reads the far side of its rectangle. After a small change
// on one side, the mirrored pixels on the other side must update too, even
// though only the small area was damaged.
static bool test_damage_expansion(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *mirror = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(vec2(1.0 - uv.x, uv.y)); }", "mirror");
	if (!check(swapchain != NULL && mirror != NULL, "swapchain and mirror program")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(mirror);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	// The effect node: a 12x12 tree at (2,2) holding a white background and a 2x2 marker that moves.
	struct wlr_scene_tree *window = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_set_position(&window->node, 2, 2);
	wlr_scene_rect_create(window, 12, 12, white);
	struct wlr_scene_rect *marker = wlr_scene_rect_create(window, 2, 2, red);
	wlr_scene_node_set_position(&marker->node, 0, 5);   // left edge, middle rows
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, mirror, &parameters);
	bool ok = warm_up(scene_output, swapchain);
	wlr_scene_output_damage_whole_for_test(scene_output);
	struct wlr_output_state state;
	struct wlr_buffer *buffer = render_frame(scene_output, swapchain, &state);
	ok &= check(buffer != NULL, "whole-damage frame");
	if (buffer != NULL) {
		uint8_t pixel[4];
		ok &= fixture_read_pixel(fixture, buffer, 12, 8, pixel);
		ok &= check(pixel[2] > 250 && pixel[1] < 5, "a whole-damage frame mirrors the marker to the right edge");
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	pixman_region32_t *pending = &scene_output->pending_commit_damage;
	ok &= check(pixman_region32_empty(pending), "acknowledged frames leave no pending damage");
	// Move the marker down by 4 rows. Only the two small rects are damaged by the scene; the mirrored copy at
	// the right edge lies outside that damage and must still update.
	wlr_scene_node_set_position(&marker->node, 0, 9);
	ok &= check(pixman_region32_not_empty(pending) && !pixman_region32_contains_point(pending, 12, 8, NULL),
		"the move damages only the marker's rows on the left");
	buffer = render_frame(scene_output, swapchain, &state);
	ok &= check(buffer != NULL, "partial-damage frame");
	if (buffer != NULL) {
		uint8_t old_spot[4], new_spot[4];
		ok &= fixture_read_pixel(fixture, buffer, 12, 8, old_spot);
		ok &= fixture_read_pixel(fixture, buffer, 12, 12, new_spot);
		ok &= check(old_spot[2] > 250 && old_spot[1] > 250, "the stale mirrored marker was repainted white");
		ok &= check(new_spot[2] > 250 && new_spot[1] < 5, "the moved marker is mirrored at its new rows");
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(mirror);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// With in_capture off and a capture pending, a dmabuf import of the rendered
// buffer sees the unfiltered composition while the display keeps the effect.
static bool test_capture_policy(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct wlr_scene_rect *window = wlr_scene_rect_create(&scene->tree, 8, 8, red);
	wlr_scene_node_set_position(&window->node, 4, 4);
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }", "capture-policy");
	bool ok = check(program != NULL, "window program compiles");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, program, &parameters);
	wlr_scene_output_set_effect_capture_policy(scene_output, false);

	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	struct wlr_scene_output_state_options options = { .swapchain = swapchain, .effect_capture_pending = true };
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
		"renders with a pending capture");
	if (ok) {
		uint8_t display[4], captured[4];
		// The swapchain buffer holds the display composition. Read it through its framebuffer: a texture import
		// of the same buffer is exactly what the capture policy redirects.
		ok &= fixture_read_display_pixel(fixture, state.buffer, 8, 8, display);
		ok &= check(display[1] > 250 && display[2] < 5, "the display shows the window effect");
		// A dmabuf import (what screencopy and image-copy do) resolves to the unfiltered capture.
		struct wlr_texture *import = wlr_texture_from_buffer(fixture->renderer, state.buffer);
		uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4];
		ok &= check(import != NULL && wlr_texture_read_pixels(import, &(struct wlr_texture_read_pixels_options) {
			.data = pixels, .format = DRM_FORMAT_ARGB8888, .stride = TEST_WIDTH * 4 }), "import reads");
		memcpy(captured, &pixels[(8 * TEST_WIDTH + 8) * 4], 4);
		ok &= check(captured[2] > 250 && captured[1] < 5, "the capture sees the plain red window");
		wlr_texture_destroy(import);
	}
	wlr_output_state_finish(&state);
	// A capture that cannot be saved leaves the frame unfiltered for display and capture alike, logged once.
	fx_renderer_fail_effect_capture_for_test(fixture->renderer, true);
	failure_logs = 0;
	failure_log_needle = "effect capture";
	wlr_log_init(WLR_DEBUG, count_failure_logs);
	for (int frame = 0; ok && frame < 2; frame++) {
		wlr_output_state_init(&state);
		wlr_scene_output_damage_whole_for_test(scene_output);
		ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
			"renders with a failed capture save");
		if (ok) {
			uint8_t display[4], captured[4];
			ok &= fixture_read_display_pixel(fixture, state.buffer, 8, 8, display);
			ok &= check(display[2] > 250 && display[1] < 5, "without a capture the display shows the plain window");
			ok &= check(fixture_read_pixel(fixture, state.buffer, 8, 8, captured), "import reads");
			ok &= check(captured[2] > 250 && captured[1] < 5, "without a capture the import sees the plain window");
		}
		wlr_output_state_finish(&state);
	}
	wlr_log_init(WLR_ERROR, NULL);
	fx_renderer_fail_effect_capture_for_test(fixture->renderer, false);
	ok &= check(failure_logs == 1, "a repeated save failure is logged once");
	// in_capture = true: the import sees the effect too.
	wlr_scene_output_set_effect_capture_policy(scene_output, true);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL, "renders again");
	if (state.buffer != NULL) {
		struct wlr_texture *import = wlr_texture_from_buffer(fixture->renderer, state.buffer);
		uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4];
		ok &= check(import != NULL && wlr_texture_read_pixels(import, &(struct wlr_texture_read_pixels_options) {
			.data = pixels, .format = DRM_FORMAT_ARGB8888, .stride = TEST_WIDTH * 4 }), "import reads");
		ok &= check(pixels[(8 * TEST_WIDTH + 8) * 4 + 1] > 250, "with in_capture the capture includes the effect");
		wlr_texture_destroy(import);
	}
	wlr_output_state_finish(&state);
	wlr_swapchain_destroy(swapchain);
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Feedback history is keyed by composition role: the unfiltered capture pass
// never reads or advances the display's history, and a role without history
// yet reads its own current input.
static bool test_capture_feedback(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_rect *window = wlr_scene_rect_create(&scene->tree, 8, 8, blue);
	wlr_scene_node_set_position(&window->node, 4, 4);
	// Each frame adds 0.25 red to the previous result and moves the previous blue into green; the first
	// frame sees its input as the previous result.
	struct fx_effect_shader *accumulate = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { vec4 p = umbriel_sample_previous(uv); "
		"return vec4(min(p.r + 0.25, 1.0), p.b, umbriel_sample(uv).b, 1.0); }", "accumulate");
	struct fx_effect_shader *green = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }", "green");
	bool ok = check(accumulate != NULL && green != NULL, "programs compile");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 7 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, green, &parameters);
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOWS_IN, accumulate, &parameters);
	wlr_scene_output_set_effect_capture_policy(scene_output, false);
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format);
	uint8_t display[4], captured[4];
	uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4];
	// Holding frame 1's buffer makes frame 2 render into another one.
	struct wlr_buffer *held = NULL;
	for (int frame = 0; frame < 3; frame++) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		// Frames 0 and 1 have a capture pending; frame 2 does not.
		struct wlr_scene_output_state_options options = { .swapchain = swapchain, .effect_capture_pending = frame < 2 };
		wlr_scene_output_damage_whole_for_test(scene_output);
		ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL, "renders");
		if (!ok) {
			wlr_output_state_finish(&state);
			break;
		}
		ok &= fixture_read_display_pixel(fixture, state.buffer, 8, 8, display);
		if (frame < 2) {
			struct wlr_texture *import = wlr_texture_from_buffer(fixture->renderer, state.buffer);
			ok &= check(import != NULL && wlr_texture_read_pixels(import, &(struct wlr_texture_read_pixels_options) {
				.data = pixels, .format = DRM_FORMAT_ARGB8888, .stride = TEST_WIDTH * 4 }), "import reads");
			memcpy(captured, &pixels[(8 * TEST_WIDTH + 8) * 4], 4);
			wlr_texture_destroy(import);
			// Capture role: first frame red 0.25 (fallback to its own input), second 0.5; blue from the plain client.
			ok &= check(captured[0] > 250, "the capture role sees the plain client");
			// Green is the previous blue: the capture's own (blue client), never the display's (green client).
			ok &= check(captured[1] > 250, "the capture role reads only its own history");
			ok &= check(captured[2] > 52 + 64 * frame && captured[2] < 76 + 64 * frame,
				"the capture role accumulates on its own");
		}
		// Display role: red grows by 0.25 per frame regardless of captures, and the window is green underneath (blue 0).
		const int expected = 64 * (frame + 1);
		ok &= check(display[2] > expected - 12 && display[2] < expected + 12, "the display role accumulates once per frame");
		ok &= check(display[0] < 5, "the display role never sees the capture's plain client");
		if (frame == 1) {
			held = wlr_buffer_lock(state.buffer);
		}
		wlr_output_state_finish(&state);
	}
	// The capture ended with frame 2: no swapchain buffer keeps a copy.
	struct fx_framebuffer *framebuffer;
	wl_list_for_each(framebuffer, &fx_get_renderer(fixture->renderer)->buffers, link) {
		ok &= check(framebuffer->effect_capture_buffer == NULL, "no capture buffer outlives the capture");
	}
	if (held != NULL) {
		wlr_buffer_unlock(held);
	}
	wlr_swapchain_destroy(swapchain);
	fx_effect_shader_unref(accumulate);
	fx_effect_shader_unref(green);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// The capture is encoded like what an import reads without one: the plain
// frame, the transformed frame, or the SDR view of a transformed output.
static bool test_capture_policy_encoding(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float half_blue[4] = { 0, 0, 0.5f, 1 }, red[4] = { 1, 0, 0, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, half_blue);
	struct wlr_scene_rect *window = wlr_scene_rect_create(&scene->tree, 8, 8, red);
	wlr_scene_node_set_position(&window->node, 4, 4);
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }", "capture-policy-encoding");
	struct wlr_color_transform *transform =
		wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_SRGB);
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format);
	bool ok = check(program != NULL && transform != NULL && swapchain != NULL, "program, transform and swapchain");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, program, &parameters);
	// A second window swaps red and blue in place: its half red must come back as half blue, not decoded twice.
	const float half_red[4] = { 0.5f, 0, 0, 1 };
	struct wlr_scene_rect *swapped = wlr_scene_rect_create(&scene->tree, 3, 3, half_red);
	wlr_scene_node_set_position(&swapped->node, 13, 13);
	struct fx_effect_shader *swap = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		kSources[FX_EFFECT_WINDOW], "capture-policy-encoding-swap");
	ok &= check(swap != NULL, "swap program");
	wlr_scene_node_set_animation(&swapped->node, FX_SLOT_WINDOW, swap, &parameters);
	// Mode 0 renders without a transform, 1 with one, 2 with one and the SDR view.
	for (int mode = 0; ok && mode < 3; mode++) {
		uint8_t backgrounds[2][4];
		// Frame 0 has a capture pending; frame 1 does not, so its import reads the usual view.
		for (int frame = 0; ok && frame < 2; frame++) {
			struct wlr_output_state state;
			wlr_output_state_init(&state);
			struct wlr_scene_output_state_options options = { .swapchain = swapchain,
				.color_transform = mode > 0 ? transform : NULL, .capture_sdr = mode == 2,
				.effect_capture_pending = frame == 0 };
			wlr_scene_output_damage_whole_for_test(scene_output);
			ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
				"renders");
			if (ok) {
				ok &= check(fixture_read_pixel(fixture, state.buffer, 1, 1, backgrounds[frame]), "import reads");
			}
			if (ok && frame == 0) {
				uint8_t display[4], captured[4];
				ok &= fixture_read_display_pixel(fixture, state.buffer, 8, 8, display);
				ok &= check(display[1] > 250 && display[2] < 5, "the display shows the window effect");
				ok &= check(fixture_read_pixel(fixture, state.buffer, 8, 8, captured), "import reads");
				ok &= check(captured[2] > 250 && captured[1] < 5, "the capture sees the plain red window");
			}
			if (ok && frame == 0 && mode == 1) {
				uint8_t display[4];
				ok &= fixture_read_display_pixel(fixture, state.buffer, 14, 14, display);
				ok &= check(display[0] > 122 && display[0] < 134 && display[2] < 5,
					"an in-place swap under a colour transform keeps the encoding");
			}
			wlr_output_state_finish(&state);
		}
		ok &= check(abs(backgrounds[0][0] - backgrounds[1][0]) <= 2, "the capture is encoded like the usual view");
	}
	wlr_swapchain_destroy(swapchain);
	wlr_color_transform_unref(transform);
	fx_effect_shader_unref(swap);
	fx_effect_shader_unref(program);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

// Feedback window and border programs keep their history through frames whose
// damage misses their boxes: each run adds 1/16 red to its previous result.
static bool test_in_place_feedback(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *feedback = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return vec4(min(umbriel_sample_previous(uv).r + 0.0625, 1.0), 0.0, 0.0, 1.0); }",
		"feedback");
	struct fx_effect_shader *border_feedback = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		"vec4 border(vec2 uv) { return vec4(min(umbriel_sample_previous(uv).r + 0.0625, 1.0), 0.0, 0.0, 1.0); }",
		"border-feedback");
	if (!check(swapchain != NULL && feedback != NULL && border_feedback != NULL, "swapchain and feedback programs")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(feedback);
		fx_effect_shader_unref(border_feedback);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_rect *window = wlr_scene_rect_create(&scene->tree, 6, 6, black);
	wlr_scene_node_set_position(&window->node, 2, 2);
	struct wlr_scene_rect *elsewhere = wlr_scene_rect_create(&scene->tree, 2, 2, white);
	wlr_scene_node_set_position(&elsewhere->node, 12, 12);
	struct wlr_scene_rect *framed = wlr_scene_rect_create(&scene->tree, 2, 6, black);
	wlr_scene_node_set_position(&framed->node, 9, 2);
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&window->node, FX_SLOT_WINDOW, feedback, &parameters);
	wlr_scene_node_set_animation(&framed->node, FX_SLOT_BORDER_EFFECT, border_feedback, &parameters);
	// Four whole-damage runs: 4/16 red.
	bool ok = warm_up(scene_output, swapchain);
	wlr_scene_rect_set_color(elsewhere, grey);
	ok &= frame_damage_is(scene_output, swapchain, 12, 12, 14, 14, "the change damages only its own rect");
	wlr_scene_output_damage_whole_for_test(scene_output);
	struct wlr_output_state state;
	struct wlr_buffer *buffer = render_frame(scene_output, swapchain, &state);
	ok &= check(buffer != NULL, "whole-damage frame");
	if (buffer != NULL) {
		static const int probes[][2] = { { 5, 5 }, { 10, 5 } };
		for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			uint8_t pixel[4];
			ok &= fixture_read_pixel(fixture, buffer, probes[i][0], probes[i][1], pixel);
			if (pixel[2] < 72 || pixel[2] > 88) {
				fprintf(stderr, "  red %d at (%d,%d), expected 80\n", pixel[2], probes[i][0], probes[i][1]);
			}
			ok &= check(pixel[2] >= 72 && pixel[2] <= 88, "the fifth run reads the fourth run's result");
		}
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(feedback);
	fx_effect_shader_unref(border_feedback);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// A transient slot's history covers its drawn box: each run adds 1/16 red to the previous result, margin included.
static bool test_expand_feedback(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *feedback = fx_effect_shader_create(fixture->renderer, FX_EFFECT_ANIMATION,
		"vec4 animation(vec2 uv) { return vec4(min(umbriel_sample_previous(uv).r + 0.0625, 1.0), 0.0, 0.0, 1.0); }",
		"expand-feedback");
	if (!check(swapchain != NULL && feedback != NULL, "swapchain and expand feedback program")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(feedback);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 };
	struct wlr_scene_rect *rect = wlr_scene_rect_create(&scene->tree, 4, 4, black);
	wlr_scene_node_set_position(&rect->node, 6, 6);
	struct fx_animation_parameters parameters = {
		.progress = 1, .linear_progress = 1, .direction = 1, .transition_id = 1, .expand = 2 };
	wlr_scene_node_set_animation(&rect->node, FX_SLOT_DRAG, feedback, &parameters);
	// Four warm-up runs and a fifth: 5/16 red over the drawn box (4,4)-(12,12).
	bool ok = warm_up(scene_output, swapchain);
	struct wlr_output_state state;
	struct wlr_buffer *buffer = render_frame(scene_output, swapchain, &state);
	ok &= check(buffer != NULL, "fifth frame");
	if (buffer != NULL) {
		static const int probes[][2] = { { 8, 8 }, { 4, 4 }, { 11, 8 } };
		for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			uint8_t pixel[4];
			ok &= fixture_read_pixel(fixture, buffer, probes[i][0], probes[i][1], pixel);
			if (pixel[2] < 72 || pixel[2] > 88) {
				fprintf(stderr, "  red %d at (%d,%d), expected 80\n", pixel[2], probes[i][0], probes[i][1]);
			}
			ok &= check(pixel[2] >= 72 && pixel[2] <= 88, "the fifth run reads the fourth run's result at its texel");
		}
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(feedback);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

static bool capture_saved(struct fixture *fixture) {
	struct fx_framebuffer *framebuffer;
	wl_list_for_each(framebuffer, &fx_get_renderer(fixture->renderer)->buffers, link) {
		if (framebuffer->effect_capture_buffer != NULL) {
			return true;
		}
	}
	return false;
}

// With a capture pending, only a visible window or overlay slot composes the
// frame a second time: a border effect stays in captures.
static bool test_capture_composition(struct fixture *fixture) {
	struct wlr_swapchain *swapchain = create_swapchain(fixture);
	struct fx_effect_shader *border = fx_effect_shader_create(fixture->renderer, FX_EFFECT_BORDER,
		kSources[FX_EFFECT_BORDER], "capture-composition-border");
	struct fx_effect_shader *window_program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		kSources[FX_EFFECT_WINDOW], "capture-composition-window");
	if (!check(swapchain != NULL && border != NULL && window_program != NULL, "swapchain and programs")) {
		wlr_swapchain_destroy(swapchain);
		fx_effect_shader_unref(border);
		fx_effect_shader_unref(window_program);
		return false;
	}
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, black);
	struct wlr_scene_rect *framed = wlr_scene_rect_create(&scene->tree, 4, 4, white);
	wlr_scene_node_set_position(&framed->node, 2, 2);
	struct wlr_scene_rect *elsewhere = wlr_scene_rect_create(&scene->tree, 2, 2, white);
	wlr_scene_node_set_position(&elsewhere->node, 12, 12);
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_node_set_animation(&framed->node, FX_SLOT_BORDER_EFFECT, border, &parameters);
	wlr_scene_output_set_effect_capture_policy(scene_output, false);
	bool ok = warm_up(scene_output, swapchain);
	// Border only: one composition, so the frame keeps its own damage and saves no capture.
	wlr_scene_rect_set_color(elsewhere, grey);
	struct wlr_output_state state;
	struct wlr_buffer *buffer = render_frame_pending(scene_output, swapchain, true, &state);
	const pixman_box32_t *extents = pixman_region32_extents(&state.damage);
	ok &= check(buffer != NULL, "border-only frame with a capture pending");
	if (extents->x1 != 12 || extents->y1 != 12 || extents->x2 != 14 || extents->y2 != 14) {
		fprintf(stderr, "  damage (%d,%d)-(%d,%d)\n", extents->x1, extents->y1, extents->x2, extents->y2);
	}
	ok &= check(extents->x1 == 12 && extents->y1 == 12 && extents->x2 == 14 && extents->y2 == 14,
		"a border-only frame keeps its own damage");
	ok &= check(!capture_saved(fixture), "a border-only frame saves no unfiltered capture");
	if (buffer != NULL) {
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	// A window slot: the unfiltered composition runs and its capture is saved.
	wlr_scene_node_set_animation(&framed->node, FX_SLOT_WINDOW, window_program, &parameters);
	buffer = render_frame_pending(scene_output, swapchain, true, &state);
	ok &= check(buffer != NULL, "window frame with a capture pending");
	ok &= check(capture_saved(fixture), "a window slot saves the unfiltered capture");
	if (buffer != NULL) {
		wlr_buffer_unlock(buffer);
	}
	wlr_output_state_finish(&state);
	fx_effect_shader_unref(border);
	fx_effect_shader_unref(window_program);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_swapchain_destroy(swapchain);
	return ok;
}

// Renders one whole-damage frame at `scale` and `transform` and returns the locked buffer.
static struct wlr_buffer *render_transformed(struct fixture *fixture, struct wlr_scene_output *scene_output,
		float scale, enum wl_output_transform transform) {
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain =
		format != NULL ? wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format) : NULL;
	if (swapchain == NULL) {
		return NULL;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, scale);
	wlr_output_state_set_transform(&state, transform);
	wlr_scene_output_damage_whole_for_test(scene_output);
	struct wlr_scene_output_state_options options = { .swapchain = swapchain };
	struct wlr_buffer *rendered = NULL;
	if (wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL) {
		rendered = wlr_buffer_lock(state.buffer);
	}
	wlr_output_state_finish(&state);
	wlr_swapchain_destroy(swapchain);
	return rendered;
}

// Reads the buffer pixel holding logical point (x, y) at scale 2 on a square output.
static bool read_logical(struct fixture *fixture, struct wlr_buffer *buffer, enum wl_output_transform transform,
		float x, float y, uint8_t out[4]) {
	struct wlr_box box = { .x = (int)(x * 2), .y = (int)(y * 2), .width = 1, .height = 1 };
	wlr_box_transform(&box, &box, wlr_output_transform_invert(transform), TEST_WIDTH, TEST_HEIGHT);
	return fixture_read_pixel(fixture, buffer, box.x, box.y, out);
}

static bool is_colour(const uint8_t pixel[4], int red, int green, int blue, const char *message) {
	const bool ok = abs(pixel[2] - red) < 6 && abs(pixel[1] - green) < 6 && abs(pixel[0] - blue) < 6;
	if (!ok) {
		fprintf(stderr, "  rgb (%d,%d,%d), expected (%d,%d,%d)\n", pixel[2], pixel[1], pixel[0], red, green, blue);
	}
	return check(ok, message);
}

// An in-place window at scale 2: the mask edge is one buffer pixel wide, and a
// buffer's corner box (the window's content inside client-side margins) is the
// shaped rectangle, also on a rotated output.
static bool test_in_place_shape(struct fixture *fixture) {
	// Mirrors the rectangle horizontally and swaps red and blue: red content turns blue, the blue desktop red.
	struct fx_effect_shader *program = fx_effect_shader_create(fixture->renderer, FX_EFFECT_WINDOW,
		"vec4 window(vec2 uv) { return umbriel_sample(vec2(1.0 - uv.x, uv.y)).bgra; }", "in-place-shape");
	struct wlr_buffer *content = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, 16, 12);
	struct wlr_render_pass *pass =
		content != NULL ? wlr_renderer_begin_buffer_pass(fixture->renderer, content, NULL) : NULL;
	if (pass != NULL) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
			.box = { .width = 16, .height = 12 }, .color = { .r = 1, .a = 1 } });
	}
	bool ok = check(program != NULL && pass != NULL && wlr_render_pass_submit(pass), "program and content buffer");
	if (!ok) {
		wlr_buffer_drop(content);
		fx_effect_shader_unref(program);
		return false;
	}
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };

	// A plain 4x4 window at (2,2): buffer pixels 4..11. Every edge pixel matches the interior.
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	wlr_scene_rect_create(&scene->tree, 8, 8, blue);
	struct wlr_scene_rect *plain = wlr_scene_rect_create(&scene->tree, 4, 4, red);
	wlr_scene_node_set_position(&plain->node, 2, 2);
	wlr_scene_node_set_animation(&plain->node, FX_SLOT_WINDOW, program, &parameters);
	struct wlr_buffer *rendered = render_transformed(fixture, scene_output, 2, WL_OUTPUT_TRANSFORM_NORMAL);
	ok &= check(rendered != NULL, "scale-2 frame");
	if (rendered != NULL) {
		static const int probes[][2] = { { 8, 8 }, { 4, 8 }, { 11, 8 }, { 8, 4 }, { 8, 11 } };
		for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
			uint8_t pixel[4];
			ok &= fixture_read_pixel(fixture, rendered, probes[i][0], probes[i][1], pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the outermost buffer pixels are fully in the mask");
		}
		wlr_buffer_unlock(rendered);
	}
	wlr_scene_node_destroy(&scene->tree.node);

	// A surface tree at (0,1) whose 8x6 buffer has a one-pixel margin around its 6x4 content box, rounded by 2,
	// with a green marker in the content's left column. Logical coordinates; the output is 8x8 at scale 2.
	for (int rotated = 0; rotated < 2; rotated++) {
		const enum wl_output_transform transform = rotated ? WL_OUTPUT_TRANSFORM_90 : WL_OUTPUT_TRANSFORM_NORMAL;
		scene = wlr_scene_create();
		scene_output = wlr_scene_output_create(scene, fixture->output);
		wlr_scene_rect_create(&scene->tree, 8, 8, blue);
		struct wlr_scene_tree *surface = wlr_scene_tree_create(&scene->tree);
		wlr_scene_node_set_position(&surface->node, 0, 1);
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(surface, content);
		wlr_scene_buffer_set_dest_size(buffer, 8, 6);
		wlr_scene_buffer_set_corner_radii(buffer, corner_radii_all(2));
		wlr_scene_buffer_set_corner_box(buffer, &(struct wlr_box) { 1, 1, 6, 4 });
		struct wlr_scene_rect *marker = wlr_scene_rect_create(surface, 1, 2, green);
		wlr_scene_node_set_position(&marker->node, 1, 2);
		wlr_scene_node_set_animation(&surface->node, FX_SLOT_WINDOW, program, &parameters);
		rendered = render_transformed(fixture, scene_output, 2, transform);
		ok &= check(rendered != NULL, rotated ? "rotated frame" : "margin frame");
		if (rendered != NULL) {
			uint8_t pixel[4];
			ok &= read_logical(fixture, rendered, transform, 6.25f, 4.25f, pixel);
			ok &= is_colour(pixel, 0, 255, 0, "the marker is mirrored across the content box");
			ok &= read_logical(fixture, rendered, transform, 1.75f, 4.25f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the marker's own column shows the mirrored content");
			ok &= read_logical(fixture, rendered, transform, 0.25f, 4.25f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the side margin keeps the desktop");
			ok &= read_logical(fixture, rendered, transform, 4.25f, 1.25f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the top margin keeps the desktop");
			ok &= read_logical(fixture, rendered, transform, 1.25f, 2.25f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the content box's rounded corner keeps the desktop");
			ok &= read_logical(fixture, rendered, transform, 4.25f, 2.25f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the content's top row is fully in the mask");
			wlr_buffer_unlock(rendered);
		}
		wlr_scene_node_destroy(&scene->tree.node);
	}

	// A corner box larger than the node bounds (a client buffer lagging a resize): the
	// intersection cuts the box's right and bottom edges, so only its bottom-right corner
	// must lose its mask radius.
	struct fx_effect_shader *paint = fx_effect_shader_create(
		fixture->renderer, FX_EFFECT_WINDOW, "vec4 window(vec2 uv) { return vec4(1.0, 0.0, 1.0, 1.0); }",
		"in-place-shape-paint");
	ok &= check(paint != NULL, "paint program compiles");
	if (paint != NULL) {
		scene = wlr_scene_create();
		scene_output = wlr_scene_output_create(scene, fixture->output);
		wlr_scene_rect_create(&scene->tree, 8, 8, blue);
		struct wlr_scene_tree *surface = wlr_scene_tree_create(&scene->tree);
		wlr_scene_node_set_position(&surface->node, 0, 1);
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(surface, content);
		wlr_scene_buffer_set_dest_size(buffer, 8, 6);
		wlr_scene_buffer_set_corner_radii(buffer, corner_radii_all(2));
		// Node bounds are the buffer's own dest box, (0,1,8,6): this corner box, (1,2,11,8) once
		// positioned, is cut on the right and bottom, well past the buffer's own (uncut) rounded
		// corner, so the two corners' arcs do not overlap.
		wlr_scene_buffer_set_corner_box(buffer, &(struct wlr_box) { 1, 1, 10, 6 });
		wlr_scene_node_set_animation(&surface->node, FX_SLOT_WINDOW, paint, &parameters);
		rendered = render_transformed(fixture, scene_output, 2, WL_OUTPUT_TRANSFORM_NORMAL);
		ok &= check(rendered != NULL, "cut-corner frame");
		if (rendered != NULL) {
			uint8_t pixel[4];
			ok &= read_logical(fixture, rendered, WL_OUTPUT_TRANSFORM_NORMAL, 7.9f, 6.8f, pixel);
			ok &= is_colour(pixel, 255, 0, 255, "the node-bounds-cut corner is fully shaded");
			wlr_buffer_unlock(rendered);
		}
		wlr_scene_node_destroy(&scene->tree.node);
		fx_effect_shader_unref(paint);
	}
	wlr_buffer_drop(content);
	fx_effect_shader_unref(program);
	return ok;
}

// A screen program shades the whole output after the scene and a cursor program
// the square around the pointer after it. A hidden pointer drops the cursor
// effect, captures exclude both, motion damages only the two squares, and a
// re-set cursor effect waits for a pointer push. The square and the pointer uv
// follow scale and rotation.
static bool test_output_effects(struct fixture *fixture) {
	struct wlr_scene *scene = wlr_scene_create();
	struct wlr_scene_output *scene_output = wlr_scene_output_create(scene, fixture->output);
	const float blue[4] = { 0, 0, 1, 1 };
	wlr_scene_rect_create(&scene->tree, TEST_WIDTH, TEST_HEIGHT, blue);
	struct fx_effect_shader *screen = fx_effect_shader_create(fixture->renderer, FX_EFFECT_SCREEN,
		"vec4 screen(vec2 uv) { return umbriel_sample(uv).bgra; }", "screen");
	struct fx_effect_shader *cursor = fx_effect_shader_create(fixture->renderer, FX_EFFECT_CURSOR,
		"vec4 cursor(vec2 uv) { if (distance(uv, umbriel_pointer) >= 0.5) return umbriel_sample(uv);"
		" return umbriel_sample(uv).r > 0.5 ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0); }", "cursor");
	// Paints only the texels within 0.15 uv of the pointer.
	struct fx_effect_shader *marker = fx_effect_shader_create(fixture->renderer, FX_EFFECT_CURSOR,
		"vec4 cursor(vec2 uv) { return distance(uv, umbriel_pointer) < 0.15 ? vec4(0.0, 1.0, 0.0, 1.0) : umbriel_sample(uv); }",
		"cursor-marker");
	bool ok = check(screen != NULL && cursor != NULL && marker != NULL, "screen and cursor programs compile");
	struct fx_animation_parameters parameters = { .progress = 1, .linear_progress = 1, .direction = 1 };
	wlr_scene_output_set_screen_effect(scene_output, screen, &parameters);
	wlr_scene_output_set_cursor_effect(scene_output, cursor, &parameters, 2);
	wlr_scene_output_set_effect_pointer(scene_output, 12, 12, true);
	struct wlr_output_state state;
	struct wlr_buffer *rendered = fixture_render_scene(fixture, scene_output, &state);
	ok &= check(rendered != NULL, "renders");
	if (rendered != NULL) {
		uint8_t far[4], at_pointer[4];
		ok &= fixture_read_pixel(fixture, rendered, 2, 2, far);
		ok &= fixture_read_pixel(fixture, rendered, 12, 12, at_pointer);
		ok &= check(far[2] > 250 && far[0] < 5, "the screen effect swapped the whole output to red");
		ok &= check(at_pointer[1] > 250, "the cursor effect reads the screen effect's result");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	// A hidden pointer removes the cursor square.
	wlr_scene_output_set_effect_pointer(scene_output, 12, 12, false);
	rendered = fixture_render_scene(fixture, scene_output, &state);
	if (rendered != NULL) {
		uint8_t at_pointer[4];
		ok &= fixture_read_pixel(fixture, rendered, 12, 12, at_pointer);
		ok &= check(at_pointer[1] < 5 && at_pointer[2] > 250, "a hidden pointer has no cursor effect");
		wlr_buffer_unlock(rendered);
	}
	wlr_output_state_finish(&state);
	// With in_capture off, a capture reads the output without the output effects.
	const struct wlr_drm_format *format = get_render_format(fixture, DRM_FORMAT_ARGB8888);
	struct wlr_swapchain *swapchain = wlr_swapchain_create(fixture->allocator, TEST_WIDTH, TEST_HEIGHT, format);
	struct wlr_scene_output_state_options options = { .swapchain = swapchain, .effect_capture_pending = true };
	wlr_scene_output_set_effect_pointer(scene_output, 12, 12, true);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
		"renders with a pending capture");
	if (ok) {
		uint8_t display[4], captured[4];
		ok &= fixture_read_display_pixel(fixture, state.buffer, 2, 2, display);
		ok &= check(display[2] > 250 && display[0] < 5, "the display keeps the screen effect");
		ok &= fixture_read_display_pixel(fixture, state.buffer, 12, 12, display);
		ok &= check(display[1] > 250, "the display keeps the cursor effect");
		ok &= fixture_read_pixel(fixture, state.buffer, 2, 2, captured);
		ok &= check(captured[0] > 250 && captured[2] < 5, "the capture has no screen effect");
		ok &= fixture_read_pixel(fixture, state.buffer, 12, 12, captured);
		ok &= check(captured[0] > 250 && captured[1] < 5, "the capture has no cursor effect");
	}
	wlr_output_state_finish(&state);
	// Without a screen effect, motion damages only the old and new cursor squares.
	wlr_scene_output_set_screen_effect(scene_output, NULL, NULL);
	options.effect_capture_pending = false;
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options), "renders the cursor effect alone");
	wlr_scene_output_acknowledge_damage_for_test(scene_output, &state);
	wlr_output_state_finish(&state);
	wlr_scene_output_set_effect_pointer(scene_output, 4, 4, true);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options), "renders after motion");
	ok &= check(pixman_region32_contains_point(&state.damage, 2, 2, NULL)
		&& pixman_region32_contains_point(&state.damage, 6, 6, NULL), "motion damages the new square");
	ok &= check(pixman_region32_contains_point(&state.damage, 10, 10, NULL)
		&& pixman_region32_contains_point(&state.damage, 14, 14, NULL), "motion damages the old square");
	ok &= check(!pixman_region32_contains_point(&state.damage, 8, 8, NULL)
		&& !pixman_region32_contains_point(&state.damage, 0, 15, NULL), "motion damages nothing else");
	wlr_scene_output_acknowledge_damage_for_test(scene_output, &state);
	wlr_output_state_finish(&state);
	// Pointer updates while the cursor effect is cleared are dropped: a re-set effect draws nothing until the
	// pointer is pushed again. A one-pixel change inside the old square then damages only itself.
	struct wlr_scene_rect *dot = wlr_scene_rect_create(&scene->tree, 1, 1, blue);
	wlr_scene_node_set_position(&dot->node, 3, 3);
	wlr_scene_output_set_cursor_effect(scene_output, NULL, NULL, 0);
	wlr_scene_output_set_effect_pointer(scene_output, 12, 12, false);
	wlr_scene_output_set_cursor_effect(scene_output, cursor, &parameters, 2);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
		"renders the re-set cursor effect");
	if (state.buffer != NULL) {
		uint8_t old_square[4];
		ok &= fixture_read_display_pixel(fixture, state.buffer, 4, 4, old_square);
		ok &= check(old_square[0] > 250, "a re-set cursor effect draws nothing at the stale pointer");
	}
	wlr_scene_output_acknowledge_damage_for_test(scene_output, &state);
	wlr_output_state_finish(&state);
	const float near_blue[4] = { 0, 0, 0.5, 1 };
	wlr_scene_rect_set_color(dot, near_blue);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options), "renders the changed pixel");
	ok &= check(pixman_region32_contains_point(&state.damage, 3, 3, NULL)
		&& !pixman_region32_contains_point(&state.damage, 2, 2, NULL)
		&& !pixman_region32_contains_point(&state.damage, 6, 6, NULL),
		"an inactive cursor effect grows no damage around the stale pointer");
	wlr_output_state_finish(&state);
	wlr_scene_output_set_effect_pointer(scene_output, 12, 12, true);
	wlr_output_state_init(&state);
	ok &= check(wlr_scene_output_build_state(scene_output, &state, &options) && state.buffer != NULL,
		"renders after the pointer push");
	if (state.buffer != NULL) {
		uint8_t at_pointer[4];
		ok &= fixture_read_display_pixel(fixture, state.buffer, 12, 12, at_pointer);
		ok &= check(at_pointer[0] < 5 && at_pointer[1] < 5, "the pushed pointer draws the cursor effect");
	}
	wlr_output_state_finish(&state);
	wlr_swapchain_destroy(swapchain);
	wlr_scene_node_destroy(&dot->node);
	// At scale 2, normal and rotated, the square sits around the pointer's logical position and the pointer uv
	// marks it inside the square: the transposed point stays unpainted.
	wlr_scene_output_set_cursor_effect(scene_output, marker, &parameters, 2);
	wlr_scene_output_set_effect_pointer(scene_output, 6.9, 2.1, true);
	for (int rotated = 0; rotated < 2; rotated++) {
		const enum wl_output_transform transform = rotated ? WL_OUTPUT_TRANSFORM_90 : WL_OUTPUT_TRANSFORM_NORMAL;
		rendered = render_transformed(fixture, scene_output, 2, transform);
		ok &= check(rendered != NULL, rotated ? "rotated scale-2 frame" : "scale-2 frame");
		if (rendered != NULL) {
			uint8_t pixel[4];
			ok &= read_logical(fixture, rendered, transform, 6.9f, 2.1f, pixel);
			ok &= is_colour(pixel, 0, 255, 0, "the pointer's texel is painted");
			ok &= read_logical(fixture, rendered, transform, 6.1f, 2.9f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "the transposed texel is not");
			ok &= read_logical(fixture, rendered, transform, 2.1f, 6.9f, pixel);
			ok &= is_colour(pixel, 0, 0, 255, "outside the square is untouched");
			wlr_buffer_unlock(rendered);
		}
	}
	// NULL parameters are zeroed, also when the same program is set again.
	wlr_scene_output_set_screen_effect(scene_output, screen, NULL);
	wlr_scene_output_set_screen_effect(scene_output, screen, NULL);
	wlr_scene_output_set_cursor_effect(scene_output, marker, NULL, 2);
	wlr_scene_output_set_screen_effect(scene_output, NULL, NULL);
	wlr_scene_output_set_cursor_effect(scene_output, NULL, NULL, 0);
	fx_effect_shader_unref(screen);
	fx_effect_shader_unref(cursor);
	fx_effect_shader_unref(marker);
	wlr_scene_node_destroy(&scene->tree.node);
	return ok;
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s CASE\n", argv[0]);
		return EXIT_FAILURE;
	}
	struct fixture fixture;
	if (!fixture_init(&fixture)) {
		fprintf(stderr, "SKIP: no FP16-capable DRM render node\n");
		fixture_finish(&fixture);
		return 77;
	}
	bool ok;
	if (strcmp(argv[1], "kinds") == 0) {
		ok = test_kinds(&fixture);
	} else if (strcmp(argv[1], "reads") == 0) {
		ok = test_reads(&fixture);
	} else if (strcmp(argv[1], "uniforms") == 0) {
		ok = test_uniforms(&fixture);
	} else if (strcmp(argv[1], "expand") == 0) {
		ok = test_expand(&fixture);
		ok &= test_expand_feedback(&fixture);
	} else if (strcmp(argv[1], "renderer-destroy") == 0) {
		ok = test_renderer_destroy(&fixture);
	} else if (strcmp(argv[1], "persistent-scene") == 0) {
		ok = test_persistent_scene(&fixture);
	} else if (strcmp(argv[1], "occlusion") == 0) {
		ok = test_occlusion(&fixture);
	} else if (strcmp(argv[1], "damage-confinement") == 0) {
		ok = test_damage_confinement(&fixture);
	} else if (strcmp(argv[1], "whole-box-invalidation") == 0) {
		ok = test_whole_box_invalidation(&fixture);
	} else if (strcmp(argv[1], "transient-policy") == 0) {
		ok = test_transient_policy(&fixture);
	} else if (strcmp(argv[1], "margin-damage") == 0) {
		ok = test_margin_damage(&fixture);
	} else if (strcmp(argv[1], "effect-bounds") == 0) {
		ok = test_effect_bounds(&fixture);
	} else if (strcmp(argv[1], "move-margin-damage") == 0) {
		ok = test_move_margin_damage(&fixture);
	} else if (strcmp(argv[1], "offscreen-margin") == 0) {
		ok = test_offscreen_margin(&fixture);
	} else if (strcmp(argv[1], "border-geometry") == 0) {
		ok = test_border_geometry(&fixture);
	} else if (strcmp(argv[1], "border-geometry-tree") == 0) {
		ok = test_border_geometry_tree(&fixture);
	} else if (strcmp(argv[1], "border-light") == 0) {
		ok = test_border_light(&fixture);
	} else if (strcmp(argv[1], "border-light-lifecycle") == 0) {
		ok = test_border_light_lifecycle(&fixture);
	} else if (strcmp(argv[1], "visible-in-box") == 0) {
		ok = test_visible_in_box(&fixture);
	} else if (strcmp(argv[1], "in-place") == 0) {
		ok = test_in_place(&fixture);
	} else if (strcmp(argv[1], "damage-expansion") == 0) {
		ok = test_damage_expansion(&fixture);
	} else if (strcmp(argv[1], "capture-policy") == 0) {
		ok = test_capture_policy(&fixture);
	} else if (strcmp(argv[1], "capture-policy-encoding") == 0) {
		ok = test_capture_policy_encoding(&fixture);
	} else if (strcmp(argv[1], "capture-feedback") == 0) {
		ok = test_capture_feedback(&fixture);
	} else if (strcmp(argv[1], "in-place-feedback") == 0) {
		ok = test_in_place_feedback(&fixture);
	} else if (strcmp(argv[1], "capture-composition") == 0) {
		ok = test_capture_composition(&fixture);
	} else if (strcmp(argv[1], "in-place-shape") == 0) {
		ok = test_in_place_shape(&fixture);
	} else if (strcmp(argv[1], "output-effects") == 0) {
		ok = test_output_effects(&fixture);
	} else {
		fprintf(stderr, "unknown case: %s\n", argv[1]);
		ok = false;
	}
	fixture_finish(&fixture);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
