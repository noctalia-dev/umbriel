#ifndef UMBRIELFX_EFFECT_H
#define UMBRIELFX_EFFECT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wlr/util/box.h>

struct wlr_output_state;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_node;
struct wlr_scene_output;
struct wlr_scene_shadow;
struct wlr_scene_tree;
struct fx_effect_shader;

// Composition slots. Descendants compose before ancestors; on one node the
// slots compose in ascending order. Slots 0..2 are persistent effects and
// never count as running animations.
#define FX_ANIMATION_SLOTS 13
#define FX_ANIMATION_DEPTH 24
#define FX_SLOT_WINDOW 0
#define FX_SLOT_OVERLAY 1
#define FX_SLOT_BORDER_EFFECT 2
#define FX_SLOT_BORDER 3
#define FX_SLOT_DIM_UNFOCUSED 4
#define FX_SLOT_WINDOWS_MOVE 5
#define FX_SLOT_DRAG 6
#define FX_SLOT_WINDOWS_IN 7
#define FX_SLOT_WINDOWS_OUT 8
#define FX_SLOT_SCRATCHPAD 9
#define FX_SLOT_LAYERS 10
#define FX_SLOT_WORKSPACES 11
#define FX_SLOT_OVERVIEW 12

static inline bool fx_slot_persistent(unsigned slot) { return slot <= FX_SLOT_BORDER_EFFECT; }
static inline bool fx_slot_in_place(unsigned slot) { return slot <= FX_SLOT_OVERLAY; }
static inline bool fx_slot_expands(unsigned slot) { return slot == FX_SLOT_BORDER_EFFECT || slot == FX_SLOT_DRAG; }

// Each kind has its own entry point and preamble; see effect_shader.c.
enum fx_effect_kind {
  FX_EFFECT_ANIMATION,
  FX_EFFECT_BORDER,
  FX_EFFECT_WINDOW,
  FX_EFFECT_SCREEN,
  FX_EFFECT_CURSOR,
};

enum fx_uniform_type {
  FX_UNIFORM_FLOAT,
  FX_UNIFORM_VEC2,
  FX_UNIFORM_VEC3,
  FX_UNIFORM_VEC4,
  FX_UNIFORM_INT,
  FX_UNIFORM_BOOL,
};

static inline unsigned fx_uniform_components(enum fx_uniform_type type) {
  switch (type) {
  case FX_UNIFORM_VEC2:
    return 2;
  case FX_UNIFORM_VEC3:
    return 3;
  case FX_UNIFORM_VEC4:
    return 4;
  default:
    return 1;
  }
}

#define FX_UNIFORM_NAME_MAX 32
#define FX_UNIFORM_FLOATS_MAX 32
#define FX_UNIFORMS_MAX 8

// A named uniform value. Locations are resolved once per program and cached;
// a name the program does not read is ignored, a type mismatch is logged once
// and skipped, and elements past the program's active array size are dropped.
struct fx_uniform {
  char name[FX_UNIFORM_NAME_MAX];
  enum fx_uniform_type type;
  unsigned count;                      // array elements; 1 for a scalar or vector
  float floats[FX_UNIFORM_FLOATS_MAX]; // FLOAT/VEC*: count * components values
  int32_t ints[4];                     // INT/BOOL: count values, count <= 4
};

struct fx_effect_light {
  bool enabled;
  float spread;    // logical px
  float intensity; // 0-4
  float threshold; // 0-1
};

struct fx_animation_parameters {
  float progress;
  float linear_progress;
  float direction;
  // Nonzero and unique for each logical transition, stable while it runs.
  uint64_t transition_id;
  // Stable values in [0, 1) for the lifetime of transition_id.
  float random_seed[4];
  // Logical pixels the drawn rectangle grows past the node bounds. Honoured
  // by FX_SLOT_BORDER_EFFECT and FX_SLOT_DRAG only.
  int expand;
  unsigned uniform_count;
  struct fx_uniform uniforms[FX_UNIFORMS_MAX];
  // Emission settings for FX_SLOT_BORDER_EFFECT.
  struct fx_effect_light light;
};

// Appends a zeroed uniform entry; NULL when the table is full or the name is
// too long. Callers fill floats[] or ints[] on the returned entry.
static inline struct fx_uniform* fx_parameters_add_uniform(
    struct fx_animation_parameters* parameters, const char* name, enum fx_uniform_type type, unsigned count
) {
  if (parameters->uniform_count >= FX_UNIFORMS_MAX || strlen(name) >= FX_UNIFORM_NAME_MAX || count == 0
      || count * fx_uniform_components(type) > FX_UNIFORM_FLOATS_MAX
      || ((type == FX_UNIFORM_INT || type == FX_UNIFORM_BOOL) && count > 4)) {
    return NULL;
  }
  struct fx_uniform* uniform = &parameters->uniforms[parameters->uniform_count++];
  memset(uniform, 0, sizeof(*uniform));
  strcpy(uniform->name, name);
  uniform->type = type;
  uniform->count = count;
  return uniform;
}

// Compilation happens with the renderer's context current. Sources provide the
// kind's entry point (vec4 animation(vec2 uv), border, window, screen, cursor),
// not a main function, version, or precision declaration.
struct fx_effect_shader* fx_effect_shader_create(
    struct wlr_renderer* renderer, enum fx_effect_kind kind, const char* source, const char* label
);
struct fx_effect_shader* fx_effect_shader_ref(struct fx_effect_shader* shader);
void fx_effect_shader_unref(struct fx_effect_shader* shader);
// A shape-preserving shader only scales its input's alpha uniformly. Shadows
// keep their analytic fast path under it instead of capturing a silhouette.
void fx_effect_shader_set_shape_preserving(struct fx_effect_shader* shader, bool shape_preserving);
// True when the linked program has an active uniform of that name.
bool fx_effect_shader_reads(const struct fx_effect_shader* shader, const char* uniform);
enum fx_effect_kind fx_effect_shader_kind(const struct fx_effect_shader* shader);

// Slots compose in ascending order, then through effect-bearing ancestors.
// A NULL shader removes a slot. Nodes hold their own reference to the program.
void wlr_scene_node_set_animation(
    struct wlr_scene_node* node, unsigned slot, struct fx_effect_shader* shader,
    const struct fx_animation_parameters* parameters
);
void wlr_scene_node_clear_animations(struct wlr_scene_node* node);
// Limit only the final animation composite in node-local coordinates. The
// complete subtree remains available to the shader and feedback history.
// A NULL box removes the clip. An empty box keeps the shader and feedback
// history running without compositing pixels. Returns false when the node has
// no animation composite, so the caller can apply an ordinary scene-tree clip.
bool wlr_scene_node_set_animation_output_clip(struct wlr_scene_node* node, const struct wlr_box* box);
// Freeze current parameters into a snapshot and transfer feedback history from
// the source that is about to be retired. Outer lifecycle effects become inner
// opening effects so the new close transition can use its normal slot.
// Persistent slots copy as they are with their time uniforms frozen; light
// never copies.
void wlr_scene_node_copy_animations_for_snapshot(struct wlr_scene_node* destination, struct wlr_scene_node* source);

// The extents of the enabled leaves under `node` in node-local coordinates: the box a slot on `node` draws over
// before its expand. False, with an empty box, when nothing under it is drawn.
bool wlr_scene_node_effect_bounds(struct wlr_scene_node* node, struct wlr_box* box);
// The largest expand among the slots on `node` itself; 0 without any.
int wlr_scene_node_animation_expand(struct wlr_scene_node* node);

// Border slots with light enabled screen-blend their emission into `layer`,
// which must be a child of the scene root. Borders stacked above the layer
// emit nothing. NULL removes the layer and every light in it.
void wlr_scene_set_effect_light_layer(struct wlr_scene* scene, struct wlr_scene_tree* layer);

// True when an enabled leaf under `node` has part of its visible region inside
// `box` (layout coordinates). Leaves under a disabled ancestor, clipped away, or
// fully occluded have none. NULL is not visible.
bool wlr_scene_node_visible_in_box(struct wlr_scene_node* node, const struct wlr_box* box);

// Keep the shadow in its stacking layer, but derive its animated silhouette
// from source. Color is the unattenuated shadow color; source alpha supplies
// opacity. The association is automatically cleared when either node dies.
void wlr_scene_shadow_set_animation_source(
    struct wlr_scene_shadow* shadow, struct wlr_scene_node* source, const float color[4]
);

// With in_capture false (the default), dmabuf imports of a frame rendered with
// a capture pending read a composition without the output's in-place effects.
void wlr_scene_output_set_effect_capture_policy(struct wlr_scene_output* output, bool in_capture);

// Output effects shade the output in place after the scene, the screen slot
// over the whole output, then the cursor slot over the square `radius` logical
// px around the pointer (0: the whole output); software cursors draw above
// both. A NULL shader removes the slot; NULL parameters are zeroed. Changing
// the program resets its feedback history. A cursor program draws nothing
// until wlr_scene_output_set_effect_pointer runs after it is set: pointer
// updates while no cursor program is set are dropped.
void wlr_scene_output_set_screen_effect(
    struct wlr_scene_output* output, struct fx_effect_shader* shader, const struct fx_animation_parameters* parameters
);
void wlr_scene_output_set_cursor_effect(
    struct wlr_scene_output* output, struct fx_effect_shader* shader, const struct fx_animation_parameters* parameters,
    int radius
);
// Layout coordinates. A hidden pointer, or one outside the output, draws no
// cursor effect and resets its history.
void wlr_scene_output_set_effect_pointer(struct wlr_scene_output* output, double lx, double ly, bool visible);

// Exist for tests/effects.c: damage the whole output, and acknowledge a built
// state's damage the way a commit of its buffer does.
void wlr_scene_output_damage_whole_for_test(struct wlr_scene_output* scene_output);
void wlr_scene_output_acknowledge_damage_for_test(
    struct wlr_scene_output* scene_output, const struct wlr_output_state* state
);
// Exists for tests/effects.c: makes fx_render_pass_read_to_buffer fail as if its source could not be sampled.
void fx_renderer_fail_target_copies_for_test(struct wlr_renderer* renderer, bool fail);
// Exists for tests/effects.c: makes saving an unfiltered effect capture fail.
void fx_renderer_fail_effect_capture_for_test(struct wlr_renderer* renderer, bool fail);

#endif
