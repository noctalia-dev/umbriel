#include "render/fx_renderer/effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/fx_renderer/shaders.h"

// Shared by every kind. Its names are the shared preamble contract.
static const char kPreamble[] =
    "precision highp float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D umbriel_texture;\n"
    "uniform mat3 umbriel_sample_matrix;\n"
    "uniform sampler2D umbriel_previous_texture;\n"
    "uniform mat3 umbriel_previous_sample_matrix;\n"
    "uniform vec2 umbriel_size;\n"
    "uniform float umbriel_scale;\n"
    "uniform float umbriel_time;\n"
    "uniform vec2 umbriel_expand;\n"
    "uniform vec4 umbriel_palette[4];\n"
    "uniform int umbriel_palette_count;\n"
    "vec4 umbriel_sample(vec2 uv) {\n"
    "  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);\n"
    "  vec2 p = (vec3(uv, 1.0) * umbriel_sample_matrix).xy;\n"
    "  if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0)))) return vec4(0.0);\n"
    "  return texture2D(umbriel_texture, p);\n"
    "}\n"
    "vec4 umbriel_sample_previous(vec2 uv) {\n"
    "  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);\n"
    "  vec2 p = (vec3(uv, 1.0) * umbriel_previous_sample_matrix).xy;\n"
    "  if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0)))) return vec4(0.0);\n"
    "  return texture2D(umbriel_previous_texture, p);\n"
    "}\n"
    // GLSL ES 1.00 indexes uniform arrays by constant expressions only, so the loop counter is the index.
    "vec4 umbriel_palette_at(float t) {\n"
    "  if (umbriel_palette_count <= 0) return vec4(0.0);\n"
    "  float span = float(umbriel_palette_count);\n"
    "  float scaled = fract(t) * span;\n"
    "  float index = floor(scaled);\n"
    "  float next = mod(index + 1.0, span);\n"
    "  vec4 from = umbriel_palette[0];\n"
    "  vec4 to = umbriel_palette[0];\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    if (i >= umbriel_palette_count) break;\n"
    "    if (float(i) == index) from = umbriel_palette[i];\n"
    "    if (float(i) == next) to = umbriel_palette[i];\n"
    "  }\n"
    "  return mix(from, to, scaled - index);\n"
    "}\n";

static const char kAnimationSection[] =
    "uniform float umbriel_progress;\n"
    "uniform float umbriel_linear_progress;\n"
    "uniform float umbriel_direction;\n"
    "uniform vec4 umbriel_random_seed;\n"
    "#define umbriel_clamped_progress clamp(umbriel_progress, 0.0, 1.0)\n";
static const char kAnimationSuffix[] = "\nvoid main() { gl_FragColor = animation(v_texcoord); }\n";

// Hole and radii describe the client rectangle inside the drawn rectangle. The
// distance is signed logical pixels, negative inside the window.
static const char kBorderSection[] =
    "uniform vec4 umbriel_border_hole;\n"
    "uniform vec4 umbriel_border_radius;\n"
    "float umbriel_border_distance(vec2 uv) {\n"
    "  vec2 half_size = umbriel_border_hole.zw * umbriel_size * 0.5;\n"
    "  vec2 p = (uv - umbriel_border_hole.xy) * umbriel_size - half_size;\n"
    "  float r = p.y < 0.0 ? (p.x < 0.0 ? umbriel_border_radius.x : umbriel_border_radius.y)\n"
    "                      : (p.x < 0.0 ? umbriel_border_radius.w : umbriel_border_radius.z);\n"
    "  r = min(r, min(half_size.x, half_size.y));\n"
    "  vec2 q = abs(p) - half_size + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n";
// The client hole is always cut out of the result, half a logical pixel soft.
static const char kBorderSuffix[] =
    "\nvoid main() {\n"
    "  vec4 c = border(v_texcoord);\n"
    "  gl_FragColor = c * smoothstep(-0.5, 0.5, umbriel_border_distance(v_texcoord));\n"
    "}\n";

// In-place kinds write back through the rounded mask of the drawn rectangle:
// outside the corner arcs the original pixel is restored. The edge is one
// buffer pixel soft, so straight edges keep every buffer pixel whole.
static const char kMaskSection[] =
    "uniform vec4 umbriel_corner_radius;\n"
    "float umbriel_mask(vec2 uv) {\n"
    "  vec2 half_size = umbriel_size * 0.5;\n"
    "  vec2 p = uv * umbriel_size - half_size;\n"
    "  float r = p.y < 0.0 ? (p.x < 0.0 ? umbriel_corner_radius.x : umbriel_corner_radius.y)\n"
    "                      : (p.x < 0.0 ? umbriel_corner_radius.w : umbriel_corner_radius.z);\n"
    "  r = min(r, min(half_size.x, half_size.y));\n"
    "  vec2 q = abs(p) - half_size + r;\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "  return 1.0 - smoothstep(-0.5, 0.5, d * umbriel_scale);\n"
    "}\n";
static const char kWindowSuffix[] =
    "\nvoid main() { gl_FragColor = mix(umbriel_sample(v_texcoord), window(v_texcoord), umbriel_mask(v_texcoord)); }\n";
static const char kScreenSuffix[] = "\nvoid main() { gl_FragColor = screen(v_texcoord); }\n";
// The cursor kind is in place too, so it needs the mask helper before its own uniform.
static const char kCursorSection[] = "uniform vec2 umbriel_pointer;\n";
static const char kCursorSuffix[] =
    "\nvoid main() { gl_FragColor = mix(umbriel_sample(v_texcoord), cursor(v_texcoord), umbriel_mask(v_texcoord)); }\n";

static const char* kind_name(enum fx_effect_kind kind) {
  switch (kind) {
  case FX_EFFECT_ANIMATION:
    return "animation";
  case FX_EFFECT_BORDER:
    return "border";
  case FX_EFFECT_WINDOW:
    return "window";
  case FX_EFFECT_SCREEN:
    return "screen";
  case FX_EFFECT_CURSOR:
    return "cursor";
  }
  return "effect";
}

// Two sections per kind: the second lets the cursor kind stack the mask helper and its pointer uniform.
static void kind_sections(enum fx_effect_kind kind, const char** section, const char** extra, const char** suffix) {
  *extra = "";
  switch (kind) {
  case FX_EFFECT_ANIMATION:
    *section = kAnimationSection;
    *suffix = kAnimationSuffix;
    return;
  case FX_EFFECT_BORDER:
    *section = kBorderSection;
    *suffix = kBorderSuffix;
    return;
  case FX_EFFECT_WINDOW:
    *section = kMaskSection;
    *suffix = kWindowSuffix;
    return;
  case FX_EFFECT_SCREEN:
    *section = "";
    *suffix = kScreenSuffix;
    return;
  case FX_EFFECT_CURSOR:
    *section = kMaskSection;
    *extra = kCursorSection;
    *suffix = kCursorSuffix;
    return;
  }
  *section = "";
  *suffix = "";
}

static void effect_renderer_destroy(struct wl_listener* listener, void* data) {
  struct fx_effect_shader* shader = wl_container_of(listener, shader, destroy);
  // Context destruction releases the GL program. Scene and config references
  // may outlive that context, but may never use its object names again.
  shader->renderer = NULL;
  shader->program = 0;
  wl_list_remove(&shader->destroy.link);
}

struct fx_effect_shader* fx_effect_shader_ref(struct fx_effect_shader* shader) {
  if (shader != NULL) {
    shader->references++;
  }
  return shader;
}

void fx_effect_shader_set_shape_preserving(struct fx_effect_shader* shader, bool shape_preserving) {
  if (shader != NULL) {
    shader->shape_preserving = shape_preserving;
  }
}

enum fx_effect_kind fx_effect_shader_kind(const struct fx_effect_shader* shader) { return shader->kind; }

void fx_effect_shader_unref(struct fx_effect_shader* shader) {
  if (shader == NULL || --shader->references != 0) {
    return;
  }
  if (shader->renderer != NULL) {
    struct wlr_egl_context previous;
    if (wlr_egl_make_current(shader->renderer->egl, &previous)) {
      glDeleteProgram(shader->program);
      wlr_egl_restore_context(&previous);
    }
    wl_list_remove(&shader->destroy.link);
  }
  free(shader);
}

// Enumerates the linked program's active uniforms once. Array names come back
// as "name[0]"; the cache stores the bare name so lookups match the config.
static void cache_uniforms(struct fx_effect_shader* shader, const char* label) {
  GLint active = 0;
  glGetProgramiv(shader->program, GL_ACTIVE_UNIFORMS, &active);
  // glGetActiveUniform truncates a name into the buffer and reports the
  // truncated length, never bufSize or more, so a name that does not fit the
  // cache entry must be measured in a larger scratch buffer first.
  char scratch[256];
  GLint i = 0;
  for (; i < active && shader->uniform_count < FX_EFFECT_UNIFORM_CACHE; i++) {
    struct fx_effect_uniform* uniform = &shader->uniforms[shader->uniform_count];
    GLsizei length = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(shader->program, (GLuint)i, sizeof(scratch), &length, &size, &type, scratch);
    if (length <= 0) {
      continue;
    }
    char* bracket = strchr(scratch, '[');
    if (bracket != NULL) {
      *bracket = '\0';
    }
    if (strlen(scratch) >= sizeof(uniform->name)) {
      continue;
    }
    uniform->size = size;
    uniform->type = type;
    strcpy(uniform->name, scratch);
    uniform->location = glGetUniformLocation(shader->program, uniform->name);
    if (uniform->location < 0) {
      continue;
    }
    shader->uniform_count++;
  }
  if (i < active) {
    wlr_log(
        WLR_ERROR, "Effect shader '%s' has more than %d active uniforms; %d are not cached and cannot be set", label,
        FX_EFFECT_UNIFORM_CACHE, (int)(active - i)
    );
  }
}

const struct fx_effect_uniform* fx_effect_shader_uniform(const struct fx_effect_shader* shader, const char* name) {
  for (unsigned i = 0; i < shader->uniform_count; i++) {
    if (strcmp(shader->uniforms[i].name, name) == 0) {
      return &shader->uniforms[i];
    }
  }
  return NULL;
}

bool fx_effect_shader_reads(const struct fx_effect_shader* shader, const char* uniform) {
  return shader != NULL && fx_effect_shader_uniform(shader, uniform) != NULL;
}

static GLenum gl_type(enum fx_uniform_type type) {
  switch (type) {
  case FX_UNIFORM_FLOAT:
    return GL_FLOAT;
  case FX_UNIFORM_VEC2:
    return GL_FLOAT_VEC2;
  case FX_UNIFORM_VEC3:
    return GL_FLOAT_VEC3;
  case FX_UNIFORM_VEC4:
    return GL_FLOAT_VEC4;
  case FX_UNIFORM_INT:
    return GL_INT;
  case FX_UNIFORM_BOOL:
    return GL_BOOL;
  }
  return 0;
}

void fx_effect_shader_bind_uniform(struct fx_effect_shader* shader, const struct fx_uniform* uniform) {
  struct fx_effect_uniform* cached = (struct fx_effect_uniform*)fx_effect_shader_uniform(shader, uniform->name);
  if (cached == NULL) {
    return;
  }
  const bool integer = uniform->type == FX_UNIFORM_INT || uniform->type == FX_UNIFORM_BOOL;
  // count is bounded by the entry's own arrays.
  const unsigned capacity = integer ? sizeof(uniform->ints) / sizeof(uniform->ints[0])
                                    : FX_UNIFORM_FLOATS_MAX / fx_uniform_components(uniform->type);
  if (cached->type != gl_type(uniform->type) || uniform->count == 0 || uniform->count > capacity) {
    if (!cached->warned) {
      cached->warned = true;
      wlr_log(
          WLR_ERROR, "Effect uniform '%s' does not match the program's declaration or its own storage; ignoring it",
          uniform->name
      );
    }
    return;
  }
  // Drivers drop array elements a program cannot reach, so the active size may be below the declared one.
  GLsizei count = (GLsizei)uniform->count;
  if (count > cached->size) {
    if (!cached->warned) {
      cached->warned = true;
      wlr_log(
          WLR_INFO, "Effect shader '%s': uniform '%s' supplies %u elements, the program reads %d; binding %d",
          shader->label, uniform->name, uniform->count, (int)cached->size, (int)cached->size
      );
    }
    count = cached->size;
  }
  switch (uniform->type) {
  case FX_UNIFORM_FLOAT:
    glUniform1fv(cached->location, count, uniform->floats);
    break;
  case FX_UNIFORM_VEC2:
    glUniform2fv(cached->location, count, uniform->floats);
    break;
  case FX_UNIFORM_VEC3:
    glUniform3fv(cached->location, count, uniform->floats);
    break;
  case FX_UNIFORM_VEC4:
    glUniform4fv(cached->location, count, uniform->floats);
    break;
  case FX_UNIFORM_INT:
  case FX_UNIFORM_BOOL:
    glUniform1iv(cached->location, count, uniform->ints);
    break;
  }
}

// Uniform state persists on a program between draws. Optional inputs a caller
// leaves out must not inherit the previous instance's values, so the ones the
// preamble declares (and that fillTimeUniforms may omit) are reset first.
static void reset_optional_uniforms(struct fx_effect_shader* shader) {
  static const struct fx_uniform defaults[] = {
      {.name = "umbriel_palette_count", .type = FX_UNIFORM_INT, .count = 1},
      {.name = "umbriel_time", .type = FX_UNIFORM_FLOAT, .count = 1},
  };
  for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
    fx_effect_shader_bind_uniform(shader, &defaults[i]);
  }
}

void fx_effect_shader_bind_parameters(struct fx_effect_shader* shader, const struct fx_animation_parameters* parameters) {
  reset_optional_uniforms(shader);
  for (unsigned i = 0; i < parameters->uniform_count && i < FX_UNIFORMS_MAX; i++) {
    fx_effect_shader_bind_uniform(shader, &parameters->uniforms[i]);
  }
}

struct fx_effect_shader*
fx_effect_shader_create(struct wlr_renderer* renderer, enum fx_effect_kind kind, const char* source, const char* label) {
  if (source == NULL || !wlr_renderer_is_fx(renderer)) {
    return NULL;
  }
  const char* section = NULL;
  const char* extra = NULL;
  const char* suffix = NULL;
  kind_sections(kind, &section, &extra, &suffix);
  struct fx_renderer* fx = fx_get_renderer(renderer);
  struct wlr_egl_context previous;
  if (!wlr_egl_make_current(fx->egl, &previous)) {
    return NULL;
  }
  struct fx_effect_shader* shader = calloc(1, sizeof(*shader));
  static const char line[] = "#line 1\n";
  const size_t length =
      sizeof(kPreamble) + strlen(section) + strlen(extra) + sizeof(line) + strlen(source) + strlen(suffix) + 1;
  char* fragment = malloc(length);
  if (shader == NULL || fragment == NULL) {
    free(shader);
    free(fragment);
    wlr_egl_restore_context(&previous);
    return NULL;
  }
  snprintf(fragment, length, "%s%s%s%s%s%s", kPreamble, section, extra, line, source, suffix);
  wlr_log(WLR_DEBUG, "Compiling %s shader: %s", kind_name(kind), label);
  shader->program = link_program(fragment);
  free(fragment);
  if (shader->program == 0) {
    wlr_log(WLR_ERROR, "Animation effect '%s' [%s] rejected; using built-in animation", label, kind_name(kind));
    free(shader);
    wlr_egl_restore_context(&previous);
    return NULL;
  }
  shader->renderer = fx;
  snprintf(shader->label, sizeof(shader->label), "%s", label != NULL ? label : "");
  shader->kind = kind;
  shader->references = 1;
  shader->destroy.notify = effect_renderer_destroy;
  wl_signal_add(&renderer->events.destroy, &shader->destroy);
  shader->proj = glGetUniformLocation(shader->program, "proj");
  shader->tex_proj = glGetUniformLocation(shader->program, "tex_proj");
  shader->position = glGetAttribLocation(shader->program, "pos");
  shader->tex = glGetUniformLocation(shader->program, "umbriel_texture");
  shader->sample_matrix = glGetUniformLocation(shader->program, "umbriel_sample_matrix");
  shader->previous_tex = glGetUniformLocation(shader->program, "umbriel_previous_texture");
  shader->previous_sample_matrix = glGetUniformLocation(shader->program, "umbriel_previous_sample_matrix");
  shader->progress = glGetUniformLocation(shader->program, "umbriel_progress");
  shader->linear_progress = glGetUniformLocation(shader->program, "umbriel_linear_progress");
  shader->direction = glGetUniformLocation(shader->program, "umbriel_direction");
  shader->random_seed = glGetUniformLocation(shader->program, "umbriel_random_seed");
  shader->size = glGetUniformLocation(shader->program, "umbriel_size");
  shader->scale = glGetUniformLocation(shader->program, "umbriel_scale");
  shader->expand = glGetUniformLocation(shader->program, "umbriel_expand");
  cache_uniforms(shader, label);
  wlr_egl_restore_context(&previous);
  return shader;
}
