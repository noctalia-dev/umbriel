// Frozen light ownership copies raw emission/pyramid pixels, never an effect.
#include "render_fixture.h"
#include "render/egl.h"
#include "render/fx_renderer/effect.h"
#include "render/pass.h"
#include <GLES2/gl2ext.h>

static bool seed_image(GLuint *texture, GLuint *framebuffer, int width, int height, GLenum type, unsigned level) {
	uint8_t bytes[8 * 4 * 4];
	uint16_t half[8 * 4 * 4];
	for (int i = 0; i < width * height; i++) {
		bytes[i * 4] = 64 + level * 16;
		bytes[i * 4 + 1] = 128;
		bytes[i * 4 + 2] = 32;
		bytes[i * 4 + 3] = 255;
		half[i * 4] = 0x4000 + level * 0x400; // 2, 4, 8, 16: distinguish every level.
		half[i * 4 + 1] = 0x3800;
		half[i * 4 + 2] = 0x3000;
		half[i * 4 + 3] = 0x3c00;
	}
	glGenTextures(1, texture);
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, type,
		type == GL_UNSIGNED_BYTE ? (void *)bytes : (void *)half);
	glGenFramebuffers(1, framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
}

static bool read_pixel(GLuint framebuffer, GLenum type, unsigned level) {
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	if (type == GL_UNSIGNED_BYTE) {
		uint8_t pixel[4] = {0};
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		return check(glGetError() == GL_NO_ERROR && pixel[0] == 64 + level * 16 &&
			pixel[1] == 128 && pixel[2] == 32 && pixel[3] == 255, "RGBA8 light image copied exactly");
	}
	float pixel[4] = {0};
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel);
	return check(glGetError() == GL_NO_ERROR && pixel[0] == (float)(2u << level) &&
		pixel[1] == 0.5f && pixel[2] == 0.125f && pixel[3] == 1.0f,
		"FP16 light image keeps extended raw values without transfer conversion");
}

static bool experiment(struct fixture *fixture, GLenum type, struct fx_effect_light_cache **retained) {
	struct fx_renderer *renderer = fx_get_renderer(fixture->renderer);
	struct fx_effect_light_cache *source = fx_effect_light_cache_create(renderer);
	if (!check(source != NULL, "create owned light cache")) {
		return false;
	}
	bool ok = check(fx_effect_light_cache_bytes(source) == 0 && fx_effect_light_cache_clone(source) == NULL,
		"cold cache cannot masquerade as a completed source");
	struct wlr_egl_context previous;
	if (!check(wlr_egl_make_current(renderer->egl, &previous), "light clone fixture context")) {
		fx_effect_light_cache_destroy(source);
		return false;
	}
	source->emission_width = 8;
	source->emission_height = 4;
	source->emission_type = type;
	source->levels = 2;
	source->margin = 4;
	ok &= check(seed_image(&source->emission_texture, &source->emission_framebuffer, 8, 4, type, 0),
		"seed completed emission pixels");
	uint64_t bytes = sizeof(*source) + 8 * 4 * (type == GL_UNSIGNED_BYTE ? 4 : 8);
	for (int i = 0; ok && i <= source->levels; i++) {
		source->widths[i] = 4 >> i;
		source->heights[i] = i == 0 ? 3 : 1;
		source->types[i] = type;
		ok &= check(seed_image(&source->textures[i], &source->framebuffers[i],
			source->widths[i], source->heights[i], type, i + 1), "seed distinct blur pyramid level");
		bytes += source->widths[i] * source->heights[i] * (type == GL_UNSIGNED_BYTE ? 4 : 8);
	}
	source->valid = ok;
	source->output = fixture->output;
	source->transform = WL_OUTPUT_TRANSFORM_90;
	wl_signal_add(&fixture->output->events.destroy, &source->output_destroy);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, source->emission_texture);
	glBindFramebuffer(GL_FRAMEBUFFER, source->emission_framebuffer);
	if (renderer->is_gles3) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER_ANGLE, source->framebuffers[0]);
	}
	glViewport(2, 3, 7, 8);
	glEnable(GL_SCISSOR_TEST);
	struct fx_effect_light_cache *copy = ok ? fx_effect_light_cache_clone(source) : NULL;
	ok &= check(copy && fx_effect_light_cache_bytes(source) == bytes && fx_effect_light_cache_bytes(copy) == bytes,
		"exact completed cache reservation and atomic deep clone");
	GLint framebuffer, texture, active, viewport[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glGetIntegerv(GL_VIEWPORT, viewport);
	ok &= check(framebuffer == (GLint)source->emission_framebuffer && texture == (GLint)source->emission_texture &&
		active == GL_TEXTURE2 && viewport[0] == 2 && viewport[1] == 3 && viewport[2] == 7 && viewport[3] == 8 &&
		glIsEnabled(GL_SCISSOR_TEST), "cloning preserves caller framebuffer, texture and raster state");
	if (renderer->is_gles3) {
		GLint read_framebuffer;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_ANGLE, &read_framebuffer);
		ok &= check(read_framebuffer == (GLint)source->framebuffers[0], "cloning preserves independent read framebuffer");
	}
	glDisable(GL_SCISSOR_TEST);
	if (copy) {
		ok &= check(copy->emission_texture != source->emission_texture &&
			copy->emission_framebuffer != source->emission_framebuffer && copy->emission_type == type &&
			copy->margin == source->margin && copy->levels == source->levels &&
			copy->output == source->output && copy->transform == source->transform,
			"clone owns independent exact-format storage with weak output provenance");
		ok &= read_pixel(copy->emission_framebuffer, type, 0);
		for (int i = 0; ok && i <= source->levels; i++) {
			ok &= check(copy->textures[i] != source->textures[i] && copy->framebuffers[i] != source->framebuffers[i] &&
				copy->types[i] == type, "every pyramid level owns a different image");
			ok &= read_pixel(copy->framebuffers[i], type, i + 1);
		}
		// A subsequent native frame may replace every source pixel. A frozen
		// clone must retain its earlier emission and each earlier pyramid level.
		for (int i = -1; i <= source->levels; i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, i < 0 ? source->emission_framebuffer : source->framebuffers[i]);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			ok &= read_pixel(i < 0 ? copy->emission_framebuffer : copy->framebuffers[i], type, i + 1);
		}
	}
	source->failed = true;
	ok &= check(fx_effect_light_cache_bytes(source) == 0 && fx_effect_light_cache_clone(source) == NULL,
		"failed native cache is unavailable for frozen admission");
	source->failed = false;
	fx_effect_light_cache_destroy(source);
	if (copy) {
		ok &= read_pixel(copy->emission_framebuffer, type, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	wlr_egl_restore_context(&previous);
	*retained = copy;
	return ok;
}

int main(void) {
	struct fixture fixture;
	if (!fixture_init(&fixture)) {
		fixture_finish(&fixture);
		return 77;
	}
	struct fx_effect_light_cache *byte = NULL, *half = NULL;
	bool ok = experiment(&fixture, GL_UNSIGNED_BYTE, &byte);
	if (fx_get_renderer(fixture.renderer)->exts.OES_texture_half_float_linear) {
		ok &= experiment(&fixture, GL_HALF_FLOAT_OES, &half);
	}
	struct wlr_buffer *buffer = create_output_buffer(&fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
	struct wlr_render_pass *pass = buffer ? wlr_renderer_begin_buffer_pass(fixture.renderer, buffer, NULL) : NULL;
	ok &= check(pass != NULL, "ordinary pass after light cloning");
	if (pass) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = {.width = TEST_WIDTH, .height = TEST_HEIGHT},
			.color = {.g = 1, .a = 1}, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		uint8_t pixels[TEST_WIDTH * TEST_HEIGHT * 4] = {0};
		ok &= check(wlr_render_pass_submit(pass) &&
			read_buffer(&fixture, buffer, DRM_FORMAT_ABGR8888, TEST_WIDTH * 4, pixels) &&
			pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255,
			"cloning preserves subsequent ordinary renderer drawing");
	}
	if (byte && buffer) {
		fx_effect_light_cache_await_commit(byte, buffer);
		ok &= check(!byte->committed && byte->pending_buffer == buffer,
			"rendered emission is unavailable before its output submission succeeds");
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_state_set_custom_mode(&state, TEST_WIDTH, TEST_HEIGHT, 60000);
		wlr_output_state_set_buffer(&state, buffer);
		ok &= check(wlr_output_commit_state(fixture.output, &state), "commit matching native light output buffer");
		wlr_output_state_finish(&state);
		ok &= check(byte->committed && byte->pending_buffer == NULL,
			"only matching output sequence and buffer make the emission snapshot ready");
		struct fx_effect_light_cache *frozen = fx_effect_light_cache_clone(byte);
		ok &= check(frozen && frozen->committed && frozen->pending_buffer == NULL,
			"frozen clone retains successful provenance independently of later frames");
		struct wlr_buffer *other = create_output_buffer(&fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
		if (other) {
			fx_effect_light_cache_await_commit(byte, buffer);
			wlr_output_state_init(&state);
			wlr_output_state_set_buffer(&state, other);
			ok &= check(wlr_output_commit_state(fixture.output, &state), "commit a different native buffer");
			wlr_output_state_finish(&state);
			ok &= check(!byte->committed && byte->pending_buffer == NULL && frozen && frozen->committed,
				"mismatched native submission cannot bless candidate emission or change a frozen snapshot");
			wlr_buffer_drop(other);
		} else {
			ok &= check(false, "allocate distinct submission control");
		}
		struct wlr_buffer *abandoned = create_output_buffer(&fixture, DRM_FORMAT_ARGB8888, TEST_WIDTH, TEST_HEIGHT);
		if (abandoned) {
			fx_effect_light_cache_await_commit(byte, abandoned);
			wlr_buffer_drop(abandoned);
			ok &= check(!byte->committed && byte->pending_buffer == NULL,
				"destroying an unsubmitted buffer invalidates its weak identity");
		} else {
			ok &= check(false, "allocate abandoned submission control");
		}
		fx_effect_light_cache_destroy(frozen);
	}
	if (buffer) {
		wlr_buffer_drop(buffer);
	}
	wlr_output_destroy(fixture.output);
	fixture.output = NULL;
	ok &= check(byte && byte->renderer != NULL && byte->output == NULL && !byte->valid &&
		fx_effect_light_cache_bytes(byte) == 0, "output destruction invalidates retained cache without a stale owner");
	if (half) {
		ok &= check(half->output == NULL && !half->valid, "output destruction invalidates retained FP16 provenance");
	}
	fixture_finish(&fixture);
	ok &= check(byte && byte->renderer == NULL && !byte->valid && fx_effect_light_cache_bytes(byte) == 0 &&
		fx_effect_light_cache_clone(byte) == NULL, "renderer destruction invalidates retained byte cache safely");
	if (half) {
		ok &= check(half->renderer == NULL && !half->valid && fx_effect_light_cache_bytes(half) == 0,
			"renderer destruction invalidates retained FP16 cache safely");
	}
	fx_effect_light_cache_destroy(byte);
	fx_effect_light_cache_destroy(half);
	return ok ? 0 : 1;
}
