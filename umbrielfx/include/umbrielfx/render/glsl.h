#ifndef UMBRIELFX_GLSL_H
#define UMBRIELFX_GLSL_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// GLSL ES 1.00 user identifiers. ASCII rules are independent of the locale.
static inline bool fx_glsl_identifier(const char *name) {
  if (!name || !*name || strncmp(name, "gl_", 3) == 0 || strstr(name, "__")) {
    return false;
  }
  for (size_t i = 0; name[i]; ++i) {
    const char c = name[i];
    if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (i > 0 && c >= '0' && c <= '9'))) {
      return false;
    }
  }
  static const char *const reserved[] = {
    "attribute", "const", "uniform", "varying", "break", "continue", "do", "for",
    "while", "if", "else", "in", "out", "inout", "float", "int",
    "void", "bool", "true", "false", "lowp", "mediump", "highp", "precision",
    "invariant", "discard", "return", "mat2", "mat3", "mat4", "vec2", "vec3",
    "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4", "sampler2D",
    "samplerCube", "struct", "asm", "class", "union", "enum", "typedef", "template",
    "this", "packed", "goto", "switch", "default", "inline", "noinline", "volatile",
    "public", "static", "extern", "external", "interface", "long", "short", "double",
    "half", "fixed", "unsigned", "superp", "input", "output", "hvec2", "hvec3",
    "hvec4", "dvec2", "dvec3", "dvec4", "fvec2", "fvec3", "fvec4", "sampler1D",
    "sampler3D", "sampler1DShadow", "sampler2DShadow", "sampler2DRect", "sampler3DRect", "sampler2DRectShadow", "sizeof", "cast",
    "namespace", "using",
  };
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    if (strcmp(name, reserved[i]) == 0) {
      return false;
    }
  }
  return true;
}

#endif
