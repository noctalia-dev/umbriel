// wlroots' multi-GPU renderer reads umbrielfx's EGL objects directly. Exercise
// that shared layout through the linked library without opening a GPU. Missing
// BGRA support deliberately stops initialization after its EGL import check.
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>

#include "render/egl.h"

static bool import_rejected;
static bool bgra_rejected;

static void capture_log(enum wlr_log_importance importance,
		const char *format, va_list args) {
	if (importance != WLR_ERROR) {
		return;
	}
	char message[512];
	vsnprintf(message, sizeof(message), format, args);
	import_rejected |= strstr(message,
		"EGL_EXT_image_dma_buf_import not supported") != NULL;
	bgra_rejected |= strstr(message,
		"BGRA8888 format not supported by GLES2") != NULL;
}

bool wlr_egl_make_current(struct wlr_egl *egl,
		struct wlr_egl_context *save_context) {
	(void)egl;
	(void)save_context;
	return true;
}

const GLubyte *glGetString(GLenum name) {
	(void)name;
	return (const GLubyte *)"";
}

static bool check_import_flag(bool supported) {
	struct wlr_egl egl = {0};
	egl.exts.EXT_image_dma_buf_import = supported;
	import_rejected = false;
	bgra_rejected = false;
	struct wlr_renderer *renderer = wlr_gles2_renderer_create(&egl);
	if (renderer != NULL || import_rejected == supported ||
			bgra_rejected != supported) {
		fprintf(stderr, "FAIL: linked wlroots misread EGL import flag %d "
			"(import rejected: %d, BGRA rejected: %d)\n",
			supported, import_rejected, bgra_rejected);
		return false;
	}
	return true;
}

int main(void) {
	wlr_log_init(WLR_ERROR, capture_log);
	return check_import_flag(false) && check_import_flag(true)
		? EXIT_SUCCESS : EXIT_FAILURE;
}
