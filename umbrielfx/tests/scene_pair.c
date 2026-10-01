// Editable authored pair stages, independent of source capture/lifecycle gates.
#include "render_fixture.h"
#include "render/fx_renderer/scene_program.h"

#define WIDTH 128
#define HEIGHT 64
#define BYTES (WIDTH * HEIGHT * 4)

static char *read_stage(const char *name) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", SCENE_PAIR_FIXTURE_DIR, name);
	FILE *file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	char *source = calloc(8192, 1);
	if (!source) {
		fclose(file);
		return NULL;
	}
	size_t size = fread(source, 1, 8191, file);
	bool ok = !ferror(file) && feof(file) && size > 0;
	fclose(file);
	if (!ok) {
		free(source);
		return NULL;
	}
	return source;
}

static bool same_pixels(const uint8_t *a, const uint8_t *b) {
	for (unsigned i = 0; i < BYTES; i++) {
		if (abs((int)a[i] - b[i]) > 1) {
			return false;
		}
	}
	return true;
}

static bool render(struct fixture *fixture, struct fx_scene_program *program,
		struct fx_scene_target *target, struct wlr_buffer *buffer,
		struct fx_scene_frame *frame, const struct fx_scene_input pair[2], uint8_t *pixels) {
	return fx_scene_program_render(program, target, frame, pair) &&
		read_buffer(fixture, buffer, DRM_FORMAT_ABGR8888, WIDTH * 4, pixels);
}

static bool experiment(struct fixture *fixture, const char *name) {
	char *source = read_stage(name);
	if (!check(source != NULL, "read editable authored pair fixture")) {
		return false;
	}
	struct fx_scene_sources sources = {.fragment = source};
	struct fx_scene_program *program = fx_scene_program_create(fixture->renderer, &sources, NULL, 0);
	free(source);
	struct wlr_buffer *buffer = create_output_buffer(fixture, DRM_FORMAT_ARGB8888, WIDTH, HEIGHT);
	struct fx_scene_target *target = buffer ? fx_scene_target_create(fixture->renderer, buffer) : NULL;
	uint8_t from[BYTES], to[BYTES], pixels[BYTES], held[BYTES];
	for (unsigned y = 0; y < HEIGHT; y++) {
		for (unsigned x = 0; x < WIDTH; x++) {
			unsigned i = (y * WIDTH + x) * 4;
			from[i] = 180 + y;
			from[i + 1] = (2 * x + 3 * y) % 256;
			from[i + 2] = 0;
			from[i + 3] = 255;
			to[i] = 0;
			to[i + 1] = (3 * x + y) % 256;
			to[i + 2] = 200 + y / 2;
			to[i + 3] = 255;
		}
	}
	struct wlr_texture *from_texture = wlr_texture_from_pixels(fixture->renderer, DRM_FORMAT_ABGR8888,
		WIDTH * 4, WIDTH, HEIGHT, from);
	struct wlr_texture *to_texture = wlr_texture_from_pixels(fixture->renderer, DRM_FORMAT_ABGR8888,
		WIDTH * 4, WIDTH, HEIGHT, to);
	struct fx_scene_input pair[2] = {{.texture = from_texture}, {.texture = to_texture}};
	bool ok = check(program && target && from_texture && to_texture, "prepare authored pair resources");
  ok &= check(program && !fx_scene_program_reads_role(program), "bundled pair shader permits role aliasing");
	struct fx_scene_frame frame = {.output_size = {WIDTH, HEIGHT}, .scale = 1, .scene_count = 2,
		.random_seed = {0.21f, 0.67f, 0.13f, 0.89f}};
	for (unsigned axis = 0; ok && axis < 2; axis++) {
		frame.axis[0] = axis == 0;
		frame.axis[1] = axis == 1;
		for (unsigned reverse = 0; ok && reverse < 2; reverse++) {
			frame.direction = reverse ? -1 : 1;
			{
				frame.progress = 0;
				ok &= render(fixture, program, target, buffer, &frame, pair, pixels) &&
					check(same_pixels(pixels, from), "exact outgoing endpoint");
				frame.progress = 1;
				ok &= render(fixture, program, target, buffer, &frame, pair, pixels) &&
					check(same_pixels(pixels, to), "exact destination endpoint");
			}
			frame.progress = 0.45f;
			ok &= render(fixture, program, target, buffer, &frame, pair, held);
			ok &= check(!same_pixels(held, from) && !same_pixels(held, to),
				"intermediate progress combines independent workspace inputs");
		}
	}
  const struct fx_scene_sources role_sources = {
    .common = "vec4 sample_role(vec2 uv){return umbriel_role==0 ? umbriel_sample_from(uv) : umbriel_sample_to(uv);}",
    .fragment = "vec4 transition(vec2 uv){return sample_role(uv);}",
  };
  struct fx_scene_program *role_program = fx_scene_program_create(fixture->renderer, &role_sources, NULL, 0);
  ok &= check(role_program && fx_scene_program_reads_role(role_program), "role-aware common shader requires independent outputs");
  for (int role = 0; ok && role < 2; role++) {
    frame.role = role;
    ok &= render(fixture, role_program, target, buffer, &frame, pair, pixels)
        && check(same_pixels(pixels, role ? to : from), "role-aware shader preserves distinct capture output");
  }
  fx_scene_program_unref(role_program);
	if (from_texture) {
		wlr_texture_destroy(from_texture);
	}
	if (to_texture) {
		wlr_texture_destroy(to_texture);
	}
	fx_scene_target_destroy(target);
	if (buffer) {
		wlr_buffer_drop(buffer);
	}
	fx_scene_program_unref(program);
	return ok;
}

int main(void) {
	struct fixture fixture;
	if (!fixture_init(&fixture)) {
		fixture_finish(&fixture);
		return 77;
	}
	bool ok = experiment(&fixture, "wipe/shader.glsl") && experiment(&fixture, "melt/shader.glsl") && experiment(&fixture, "iris/shader.glsl");
	fixture_finish(&fixture);
	return ok ? 0 : 1;
}
