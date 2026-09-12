#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// wlroots uses C99 static array bounds in its color declarations.

#define static
#include <wlr/render/color.h>
#undef static

#ifdef __cplusplus
}
#endif
