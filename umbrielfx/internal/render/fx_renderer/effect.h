#ifndef FX_EFFECT_PRIVATE_H
#define FX_EFFECT_PRIVATE_H

#include <GLES2/gl2.h>
#include <pixman.h>
#include <stdbool.h>
#include <umbrielfx/render/effect.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

struct fx_animation_history;
struct fx_gles_render_pass;
struct fx_renderer;
struct wlr_output;

#define FX_EFFECT_UNIFORM_CACHE 48

struct fx_effect_uniform {
  char name[FX_UNIFORM_NAME_MAX];
  GLint location;
  GLenum type;
  GLint size;
  bool warned;
};

struct fx_effect_shader {
  struct fx_renderer* renderer;
  unsigned references;
  struct wl_listener destroy;
  enum fx_effect_kind kind;
  char label[128]; // for diagnostics; truncated
  GLuint program;
  GLint proj, tex_proj, position, tex, sample_matrix;
  GLint previous_tex, previous_sample_matrix;
  GLint progress, linear_progress, direction, random_seed;
  GLint size, scale, expand;
  bool shape_preserving;
  unsigned uniform_count;
  struct fx_effect_uniform uniforms[FX_EFFECT_UNIFORM_CACHE];
};

const struct fx_effect_uniform* fx_effect_shader_uniform(const struct fx_effect_shader* shader, const char* name);
// The program must be in use. A name the program lacks is ignored; a type
// mismatch or a count past the entry's storage is ignored. A count above the
// program's active array size binds the active elements. Each case is logged
// once per program and name.
void fx_effect_shader_bind_uniform(struct fx_effect_shader* shader, const struct fx_uniform* uniform);
void fx_effect_shader_bind_parameters(
    struct fx_effect_shader* shader, const struct fx_animation_parameters* parameters
);

// Border node geometry in logical px, relative to the composite's logical_box origin.
struct fx_effect_geometry {
  struct wlr_box hole;
  float radius[4]; // tl, tr, br, bl logical px
};

#define FX_LIGHT_LEVELS 6

// A border slot's emission and its blur pyramid, rebuilt each time the slot composites.
struct fx_effect_light_cache {
  struct fx_renderer* renderer;
  struct wl_listener renderer_destroy;
  GLuint emission_texture, emission_framebuffer;
  int emission_width, emission_height;
  GLuint textures[FX_LIGHT_LEVELS + 1], framebuffers[FX_LIGHT_LEVELS + 1];
  int widths[FX_LIGHT_LEVELS + 1], heights[FX_LIGHT_LEVELS + 1], levels;
  int margin; // buffer px around the emission
  bool valid, failed;
};

struct fx_effect_light_cache* fx_effect_light_cache_create(struct fx_renderer* renderer);
void fx_effect_light_cache_destroy(struct fx_effect_light_cache* cache);
// Screen-blends the blurred emission over `box` (the proxy's buffer box), clipped.
void fx_render_pass_add_effect_light(
    struct fx_gles_render_pass* pass, struct fx_effect_light_cache* cache, const struct fx_effect_light* light,
    const struct wlr_box* box, const pixman_region32_t* clip
);

struct fx_effect_composite {
  struct fx_effect_shader* shader;
  const struct fx_animation_parameters* parameters;
  struct wlr_box box;         // node box, buffer px
  struct wlr_box logical_box; // node box, logical
  enum wl_output_transform transform;
  int expand;
  const pixman_region32_t* capture_clip;
  const pixman_region32_t* output_clip;
  struct fx_animation_history* history;
  struct wlr_output* output;
  bool update_history;
  const struct fx_effect_geometry* geometry; // NULL unless a border slot composites a border node
  struct fx_effect_light_cache* light;       // NULL unless this composite emits light
  bool replace;                              // write without blending; set by the in-place path
  unsigned role;                             // selects the history: 0 display, 1 unfiltered capture
  const float* corner_radius;                // tl, tr, br, bl logical px for umbriel_corner_radius; may be NULL
  const float* pointer;                      // umbriel_pointer, uv in the drawn box; NULL unless a cursor kind
};

// Pops the capture begun by fx_render_pass_begin_animation and draws it
// through the composite's program.
void fx_render_pass_end_effect(struct fx_gles_render_pass* pass, const struct fx_effect_composite* composite);
// Renders `composite->shader` over the current target's pixels under `box`,
// writing back with blending off through the rounded mask. The subtree must
// already be drawn. Reads and promotes history like a capture composite.
// `expand` is ignored: the mask rounds the node box.
void fx_render_pass_effect_in_place(struct fx_gles_render_pass* pass, const struct fx_effect_composite* composite);
// Copies the target, encoded as a dmabuf import of the output buffer reads it,
// into the output buffer's effect capture. Imports read the copy once the pass
// submits. The target stays bound.
bool fx_render_pass_save_effect_capture(struct fx_gles_render_pass* pass);

#endif
