#include "render/fx_renderer/scene_program.h"

#include <math.h>
#include <drm_fourcc.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include "render/egl.h"
#include "render/fx_renderer/glsl_common.h"
#include "render/fx_renderer/effect.h"
#include "umbrielfx/render/fx_renderer/fx_offscreen_buffers.h"
#include "render/fx_renderer/fx_renderer.h"
#include "umbrielfx/render/pass.h"

enum location { OUTPUT_SIZE, SCALE, TIME, PROGRESS, LINEAR_PROGRESS, DIRECTION, SEED, SCENE_COUNT, ROLE, PALETTE, PALETTE_COUNT, AXIS, VIEWPORT, CLIP_MATRIX, RASTER_MATRIX, TARGET_SIZE, INPUT0, INPUT1, MATRIX0, MATRIX1, LOCATION_COUNT };
static const char *names[LOCATION_COUNT] = { "umbriel_output_size", "umbriel_scale", "umbriel_time", "umbriel_progress", "umbriel_linear_progress", "umbriel_direction", "umbriel_random_seed", "umbriel_scene_count", "umbriel_role", "umbriel_palette", "umbriel_palette_count", "umbriel_axis", "umbriel_viewport", "_fx_clip_matrix", "_fx_raster_matrix", "_fx_target_size", "_fx_input0", "_fx_input1", "_fx_matrix0", "_fx_matrix1" };

struct scene_stage {
	GLuint program;
	GLint location[LOCATION_COUNT];
};

struct fx_scene_program {
	struct fx_renderer *renderer;
	struct wl_listener destroy;
	struct scene_stage main;
};

struct fx_scene_target {
	struct fx_renderer *renderer;
	struct wl_listener destroy;
	struct wlr_buffer *buffer;
	struct fx_framebuffer *framebuffer;
	bool working_space;
};

static const char preamble[] =
	"precision highp float;\n"
	// GLES gives vertex and fragment integers different default precision.
	// Shared uniforms must have identical precision to link on strict drivers.
	"precision highp int;\n"
	FX_GLSL_TRIG
	"uniform vec2 umbriel_output_size;\n"
	"uniform float umbriel_scale, umbriel_time;\n"
	FX_GLSL_ANIMATION
	"uniform int umbriel_scene_count, umbriel_role;\n"
	"uniform vec4 umbriel_palette[4]; uniform int umbriel_palette_count;\n"
	FX_GLSL_PALETTE
	"uniform vec2 umbriel_axis; uniform vec4 umbriel_viewport;\n";

static const char fragment_preamble[] =
	"uniform vec2 _fx_target_size;\n"
	"uniform mat3 _fx_raster_matrix;\n"
	"vec2 _fx_output_uv(){return (_fx_raster_matrix*vec3(gl_FragCoord.xy/_fx_target_size,1.0)).xy;}\n"
	"uniform sampler2D _fx_input0, _fx_input1; uniform mat3 _fx_matrix0, _fx_matrix1;\n"
	"vec4 _fx_sample0(vec2 uv) {\n"
	" if (any(lessThan(uv,vec2(0.0))) || any(greaterThan(uv,vec2(1.0)))) return vec4(0.0);\n"
	" vec2 p=(_fx_matrix0*vec3(uv,1.0)).xy;\n"
	" if (any(lessThan(p,vec2(0.0))) || any(greaterThan(p,vec2(1.0)))) return vec4(0.0);\n"
	" return texture2D(_fx_input0,p); }\n"
	"vec4 _fx_sample1(vec2 uv) {\n"
	" if (any(lessThan(uv,vec2(0.0))) || any(greaterThan(uv,vec2(1.0)))) return vec4(0.0);\n"
	" vec2 p=(_fx_matrix1*vec3(uv,1.0)).xy;\n"
	" if (any(lessThan(p,vec2(0.0))) || any(greaterThan(p,vec2(1.0)))) return vec4(0.0);\n"
	" return texture2D(_fx_input1,p); }\n";

static const char vertex_source[] =
  "attribute vec2 pos; uniform mat3 _fx_clip_matrix;\n"
  "void main(){vec3 p=_fx_clip_matrix*vec3(pos*2.0-1.0,1.0);gl_Position=vec4(p.xy,0.0,p.z); }\n";

static bool stage_create(struct scene_stage *stage, const struct fx_scene_sources *sources,
    const struct fx_scene_parameter *parameters, unsigned parameter_count) {
  const char *parts[] = {preamble, fragment_preamble,
      "vec4 umbriel_sample_from(vec2 uv){return _fx_sample0(uv); }\n"
      "vec4 umbriel_sample_to(vec2 uv){return _fx_sample1(uv); }\n",
      sources->common ? sources->common : "", sources->fragment, "void main(){gl_FragColor=transition(_fx_output_uv());}\n"};
  size_t size = 1;
  for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) size += strlen(parts[i]) + 1;
  char *fragment = calloc(size, 1);
  if (!fragment) return false;
  for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    strcat(fragment, parts[i]);
    strcat(fragment, "\n");
  }
  stage->program = link_program_sources(vertex_source, fragment, "pos");
  free(fragment);
  if (!stage->program) return false;
	GLint active = 0;
	glGetProgramiv(stage->program, GL_ACTIVE_UNIFORMS, &active);
	for (GLint i = 0; i < active; i++) {
		char name[128];
		GLsizei length;
		GLint size;
		GLenum type;
		glGetActiveUniform(stage->program, i, sizeof(name), &length, &size, &type, name);
		char *bracket = strchr(name, '[');
		if (bracket) {
			*bracket = '\0';
		}
		bool known = false;
		for (unsigned j = 0; j < LOCATION_COUNT; j++) {
			known |= strcmp(name, names[j]) == 0;
		}
		for (unsigned j = 0; j < parameter_count; j++) {
			if (strcmp(name, parameters[j].name) != 0) {
				continue;
			}
			static const GLenum types[] = {GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4};
			if (bracket != NULL || size != 1 || type != types[parameters[j].components - 1]) {
				return false;
			}
			known = true;
		}
		if (!known) {
			return false;
		}
	}
	for (unsigned i = 0; i < LOCATION_COUNT; i++) {
		stage->location[i] = glGetUniformLocation(stage->program, names[i]);
	}
	glUseProgram(stage->program);
	for (unsigned i = 0; i < parameter_count; i++) {
		GLint location = glGetUniformLocation(stage->program, parameters[i].name);
		switch (parameters[i].components) {
		case 1: glUniform1fv(location, 1, parameters[i].value); break;
		case 2: glUniform2fv(location, 1, parameters[i].value); break;
		case 3: glUniform3fv(location, 1, parameters[i].value); break;
		case 4: glUniform4fv(location, 1, parameters[i].value); break;
		}
	}
	return glGetError() == GL_NO_ERROR;
}

static void program_renderer_destroy(struct wl_listener *listener, void *data) {
	struct fx_scene_program *program = wl_container_of(listener, program, destroy);
	wl_list_remove(&program->destroy.link);
	program->renderer = NULL;
}

bool fx_scene_program_reads_role(const struct fx_scene_program *program) {
  return program && program->renderer && program->main.location[ROLE] >= 0;
}

bool fx_scene_program_reads_time(const struct fx_scene_program *program) {
 return program && program->renderer && program->main.location[TIME] >= 0;
}

static void target_renderer_destroy(struct wl_listener *listener, void *data) {
	struct fx_scene_target *target = wl_container_of(listener, target, destroy);
	wl_list_remove(&target->destroy.link);
	target->renderer = NULL;
	target->framebuffer = NULL;
}

struct wlr_buffer *fx_scene_buffer_create(struct wlr_renderer *renderer,
		struct wlr_allocator *allocator, int width, int height, bool floating_point) {
	if (!renderer || !wlr_renderer_is_fx(renderer) || !allocator || width <= 0 || height <= 0 ||
			!renderer->impl->get_render_formats ||
			(floating_point && !renderer->features.output_color_transform)) return NULL;
	struct fx_scene_limits limits;
	if (!fx_scene_program_get_limits(renderer, &limits) ||
			(unsigned)width > limits.texture_size || (unsigned)height > limits.texture_size) return NULL;
	const struct wlr_drm_format_set *formats = renderer->impl->get_render_formats(renderer);
	const struct wlr_drm_format *format = formats ? wlr_drm_format_set_get(formats,
		floating_point ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ARGB8888) : NULL;
	return format ? wlr_allocator_create_buffer(allocator, width, height, format) : NULL;
}

static struct fx_scene_target *target_create(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer, int working_space) {
	if (!wlr_renderer || !wlr_renderer_is_fx(wlr_renderer) || !buffer || buffer->width <= 0 || buffer->height <= 0) {
		return NULL;
	}
	struct fx_renderer *renderer = fx_get_renderer(wlr_renderer);
	struct wlr_egl_context previous;
	if (!wlr_egl_make_current(renderer->egl, &previous)) {
		return NULL;
	}
	GLint old_fbo;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
	struct fx_scene_target *target = calloc(1, sizeof(*target));
	if (!target) {
		goto out;
	}
	target->renderer = renderer;
	target->framebuffer = fx_framebuffer_get_or_create(renderer, buffer);
	if (!target->framebuffer) {
		free(target);
		target = NULL;
		goto out;
	}
	target->working_space = working_space < 0 ? target->framebuffer->drm_format == DRM_FORMAT_ABGR16161616F : working_space;
	glBindFramebuffer(GL_FRAMEBUFFER, fx_framebuffer_get_fbo(target->framebuffer));

	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
	if (!ok) {

		free(target);
		target = NULL;
		goto out;
	}
	target->buffer = wlr_buffer_lock(buffer);
	target->destroy.notify = target_renderer_destroy;
	wl_signal_add(&wlr_renderer->events.destroy, &target->destroy);
out:
	glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
	wlr_egl_restore_context(&previous);
	return target;
}

struct fx_scene_target *fx_scene_target_create(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	return target_create(renderer, buffer, -1);
}

struct fx_scene_target *fx_scene_target_create_with_color(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer, bool working_space) {
	return target_create(renderer, buffer, working_space);
}

void fx_scene_target_destroy(struct fx_scene_target *target) {
	if (!target) {
		return;
	}
	if (target->renderer) { wl_list_remove(&target->destroy.link); }
	wlr_buffer_unlock(target->buffer);
	free(target);
}

static bool input_valid(const struct fx_scene_input *input, const struct fx_scene_target *target) {
	if (!input || !input->texture || !wlr_texture_is_fx(input->texture)) {
		return false;
	}
	struct fx_texture *texture = fx_get_texture(input->texture);
	return texture->fx_renderer == target->renderer && texture->target == GL_TEXTURE_2D &&
		texture->locked_buffer != target->buffer && (!texture->buffer || texture->buffer->buffer != target->buffer);
}

static void bind_input(const struct scene_stage *stage, const struct fx_scene_input *input, unsigned unit) {
	static const float identity[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, fx_get_texture(input->texture)->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUniform1i(stage->location[unit ? INPUT1 : INPUT0], unit);
	glUniformMatrix3fv(stage->location[unit ? MATRIX1 : MATRIX0], 1, GL_FALSE,
		input->sample_matrix ? input->sample_matrix : identity);
}

static void transform_uv_matrix(enum wl_output_transform transform, float matrix[9]) {
	struct wlr_box origin = {0}, x = {.x = 1}, y = {.y = 1};
	wlr_box_transform(&origin, &origin, transform, 1, 1);
	wlr_box_transform(&x, &x, transform, 1, 1);
	wlr_box_transform(&y, &y, transform, 1, 1);
	const float values[] = {
		x.x - origin.x, x.y - origin.y, 0,
		y.x - origin.x, y.y - origin.y, 0,
		origin.x, origin.y, 1,
	};
	memcpy(matrix, values, sizeof(values));
}

static void bind_frame(const struct scene_stage *stage, const struct fx_scene_frame *frame,
		const struct fx_scene_target *target) {
	const GLint *l = stage->location;
	glUseProgram(stage->program);
	float raster[9], clip[9];
	transform_uv_matrix(frame->output_transform, raster);
	transform_uv_matrix(wlr_output_transform_invert(frame->output_transform), clip);
	clip[6] = clip[0] + clip[3] + 2 * clip[6] - 1;
	clip[7] = clip[1] + clip[4] + 2 * clip[7] - 1;
	glUniformMatrix3fv(l[CLIP_MATRIX], 1, GL_FALSE, clip);
	glUniformMatrix3fv(l[RASTER_MATRIX], 1, GL_FALSE, raster);
	glUniform2fv(l[OUTPUT_SIZE], 1, frame->output_size);
	glUniform2f(l[TARGET_SIZE], target->buffer->width, target->buffer->height);
	glUniform1f(l[SCALE], frame->scale);
	glUniform1f(l[TIME], frame->time);
	glUniform1f(l[PROGRESS], frame->progress);
	glUniform1f(l[LINEAR_PROGRESS], frame->linear_progress);
	glUniform1f(l[DIRECTION], frame->direction);
	glUniform4fv(l[SEED], 1, frame->random_seed);
	glUniform1i(l[SCENE_COUNT], frame->scene_count);
	glUniform1i(l[ROLE], frame->role);
	glUniform2fv(l[AXIS], 1, frame->axis);
	glUniform4fv(l[VIEWPORT], 1, frame->viewport);
	glUniform4fv(l[PALETTE], 4, frame->palette);
	glUniform1i(l[PALETTE_COUNT], frame->palette_count);
}

static bool render_stage(struct fx_scene_program *program, const struct scene_stage *stage,
		struct fx_scene_target *target, const struct fx_scene_frame *frame,
		const struct fx_scene_input pair[2]) {
	struct wlr_render_pass *wlr_pass = wlr_renderer_begin_buffer_pass(&program->renderer->wlr_renderer,
		target->buffer, NULL);
	if (!wlr_pass) {
		return false;
	}
	struct fx_gles_render_pass *pass = fx_get_render_pass(wlr_pass);
	pass->working_space = target->working_space;

	pixman_region32_union_rect(&pass->updated_region, &pass->updated_region, 0, 0,
		target->buffer->width, target->buffer->height);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glClearColor(0, 0, 0, 0);
	glClearDepthf(1);
	glDepthRangef(0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	bind_frame(stage, frame, target);
	static float uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
	static uint16_t indices[] = {0, 1, 2, 2, 1, 3};
	bind_input(stage, &pair[0], 0);
	bind_input(stage, &pair[1], 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
	bool ok = !pass->incomplete && glGetError() == GL_NO_ERROR;
	// Restore the renderer's ordinary-pass invariants before submission and
	// any output conversion. There is no renderer state cache to invalidate.
	glDisableVertexAttribArray(0);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return wlr_render_pass_submit(wlr_pass) && ok;
}

bool fx_scene_program_render(struct fx_scene_program *program,
		struct fx_scene_target *target, const struct fx_scene_frame *frame, const struct fx_scene_input pair[2]) {
	if (!program || !program->renderer || !target || target->renderer != program->renderer || !frame ||
			!isfinite(frame->output_size[0]) || !isfinite(frame->output_size[1]) ||
			frame->output_size[0] <= 0 || frame->output_size[1] <= 0 ||
			frame->output_transform > WL_OUTPUT_TRANSFORM_FLIPPED_270 ||
			frame->scene_count != 2 ||
			frame->role < 0 || frame->role > 1 || frame->palette_count < 0 || frame->palette_count > 4) {
		return false;
	}
	if (!pair || !input_valid(&pair[0], target) || !input_valid(&pair[1], target)) return false;
	return render_stage(program, &program->main, target, frame, pair);
}

void fx_scene_program_unref(struct fx_scene_program *program) {
	if (!program) {
		return;
	}
	if (program->renderer) {
		struct wlr_egl_context previous;
		if (wlr_egl_make_current(program->renderer->egl, &previous)) {
			glDeleteProgram(program->main.program);
			wlr_egl_restore_context(&previous);
		}
		wl_list_remove(&program->destroy.link);
	}
	free(program);
}

struct fx_scene_program *fx_scene_program_create(struct wlr_renderer *wlr_renderer,
		const struct fx_scene_sources *sources,
		const struct fx_scene_parameter *parameters, unsigned parameter_count) {
	if (!wlr_renderer || !wlr_renderer_is_fx(wlr_renderer) || !sources || !sources->fragment ||
			parameter_count > FX_SCENE_PARAMETERS ||
			(parameter_count && !parameters)) {
		return NULL;
	}
	size_t source_size = strlen(sources->fragment) + (sources->common ? strlen(sources->common) : 0);
	if (source_size > 512u * 1024) {
		return NULL;
	}
	for (unsigned i = 0; i < parameter_count; i++) {
		const struct fx_scene_parameter *p = &parameters[i];
		if (!memchr(p->name, 0, sizeof(p->name)) || !fx_scene_parameter_identifier(p->name) ||
				p->components < 1 || p->components > 4) {
			return NULL;
		}
		for (unsigned j = 0; j < p->components; j++) {
			if (!isfinite(p->value[j])) {
				return NULL;
			}
		}
		for (unsigned j = 0; j < i; j++) {
			if (strcmp(p->name, parameters[j].name) == 0) {
				return NULL;
			}
		}
	}
	struct fx_renderer *renderer = fx_get_renderer(wlr_renderer);
	struct wlr_egl_context previous;
	if (!wlr_egl_make_current(renderer->egl, &previous)) {
		return NULL;
	}
	GLint vertex_limit, fragment_limit, texture_units;
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vertex_limit);
	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &fragment_limit);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &texture_units);
	struct fx_scene_program *program = NULL;
	// Reserve every declared built-in, including palette, independently
	// for each stage. Do not rely on optimizer-specific uniform elimination.
	if (vertex_limit < (int)FX_SCENE_VERTEX_VECTORS
			|| fragment_limit < (int)FX_SCENE_FRAGMENT_VECTORS + (int)parameter_count || texture_units < 2) {
		goto out;
	}
	program = calloc(1, sizeof(*program));
	if (!program) {
		goto out;
	}
	program->renderer = renderer;
	program->destroy.notify = program_renderer_destroy;
	wl_signal_add(&wlr_renderer->events.destroy, &program->destroy);
	bool ok = stage_create(&program->main, sources, parameters, parameter_count);
	if (!ok) {
		fx_scene_program_unref(program);
		program = NULL;
	}
out:
	glUseProgram(0);
	wlr_egl_restore_context(&previous);
	return program;
}

bool fx_scene_program_get_limits(struct wlr_renderer *wlr_renderer, struct fx_scene_limits *limits) {
	if (!limits) {
		return false;
	}
	*limits = (struct fx_scene_limits){0};
	if (!wlr_renderer || !wlr_renderer_is_fx(wlr_renderer)) {
		return false;
	}
  struct fx_renderer *renderer = fx_get_renderer(wlr_renderer);
  if (renderer->scene_limits[0]) {
    *limits = (struct fx_scene_limits){renderer->scene_limits[0], renderer->scene_limits[1],
        renderer->scene_limits[2], renderer->scene_limits[3]};
    return true;
  }
	struct wlr_egl_context previous;
	if (!wlr_egl_make_current(fx_get_renderer(wlr_renderer)->egl, &previous)) {
		return false;
	}
	GLint texture, vertex, fragment, units;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture);
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vertex);
	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &fragment);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
	bool ok = glGetError() == GL_NO_ERROR && texture > 0 && vertex > 0 && fragment > 0 && units > 0;
	if (ok) {
		*limits = (struct fx_scene_limits){texture, vertex, fragment, units};
    unsigned values[] = {texture, vertex, fragment, units};
    memcpy(renderer->scene_limits, values, sizeof(values));
	}
	wlr_egl_restore_context(&previous);
	return ok;
}
