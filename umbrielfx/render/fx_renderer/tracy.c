#ifdef TRACY_ENABLE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tracy/TracyC.h>
#include <wayland-util.h>
#include <wlr/util/log.h>

#include "render/fx_renderer/fx_renderer.h"
#include "render/tracy.h"

// Docs used in the implementation:
// - https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
// - https://registry.khronos.org/OpenGL/extensions/EXT/EXT_disjoint_timer_query.txt
// - https://github.com/wolfpld/tracy/blob/7388a476587c93f4e8e3e50d64c1400fc8c5e47e/public/tracy/TracyOpenGL.hpp

#define QUERY_QUEUE_LEN (64 * 1024)

// Colors:
//	From https://github.com/wolfpld/tracy/blob/7388a476587c93f4e8e3e50d64c1400fc8c5e47e/public/common/TracyColor.hpp
#define RED4 0x8b0000

static atomic_int id_counter = 0;

struct tracy_data {
	struct fx_renderer *renderer;
	PFNGLGETQUERYOBJECTUIVEXTPROC get_query_available;

	uint8_t context_id;

	struct {
		uint32_t queries[QUERY_QUEUE_LEN];
		uint32_t head;
		uint32_t tail;
	} queue;
};

/*
 * GPU Zone
 */

static inline unsigned int get_next_query_index(struct tracy_data *tracy_data) {
	const unsigned int id = tracy_data->queue.head;
	tracy_data->queue.head = (tracy_data->queue.head + 1) % QUERY_QUEUE_LEN;
	assert(tracy_data->queue.head != tracy_data->queue.tail);
	return id;
}

void tracy_gpu_zone_begin(struct tracy_data *tracy_data, struct tracy_gpu_zone_context *out_ctx,
		const int line, const char *source, const char *func, const char *name) {
	if (out_ctx == NULL) {
		return;
	}
	*out_ctx = (struct tracy_gpu_zone_context){0};
	if (tracy_data == NULL) {
		return;
	}

	out_ctx->tracy_data = tracy_data;
#ifdef TRACY_ON_DEMAND
	out_ctx->is_active = TracyCIsConnected;
#else
	out_ctx->is_active = true;
#endif

	if (!out_ctx->is_active) {
		return;
	}

	// Create the query object
	const unsigned int query = get_next_query_index(tracy_data);
	tracy_data->renderer->procs.glQueryCounterEXT(tracy_data->queue.queries[query], GL_TIMESTAMP_EXT);

	// Label the zone
	uint64_t srcloc = ___tracy_alloc_srcloc_name(line,
			source, strlen(source),
			func, strlen(func),
			name, strlen(name),
			0);

	// Begin the Tracy GPU zone
	const struct ___tracy_gpu_zone_begin_data data = {
		.context = tracy_data->context_id,
		.queryId = (uint16_t) query,
		.srcloc = srcloc,
	};
	___tracy_emit_gpu_zone_begin_alloc(data);
}

void tracy_gpu_zone_end(struct tracy_gpu_zone_context *ctx) {
	if (ctx == NULL || ctx->tracy_data == NULL || !ctx->is_active) {
		return;
	}
	struct tracy_data *tracy_data = ctx->tracy_data;

	// Create the query object
	const unsigned int query = get_next_query_index(tracy_data);
	tracy_data->renderer->procs.glQueryCounterEXT(tracy_data->queue.queries[query], GL_TIMESTAMP_EXT);

	// End the Tracy GPU zone
	const struct ___tracy_gpu_zone_end_data data = {
		.context = tracy_data->context_id,
		.queryId = (uint16_t) query,
	};
	___tracy_emit_gpu_zone_end(data);
}

void tracy_gpu_context_collect(struct tracy_data *tracy_data) {
	if (tracy_data == NULL) {
		TracyCMessageL("tracy_data == NULL");
		return;
	}

	TracyCZoneC(ctx, RED4, true);

	if (tracy_data->queue.tail == tracy_data->queue.head) {
		goto done;
	}

#ifdef TRACY_ON_DEMAND
	if (!TracyCIsConnected) {
		tracy_data->queue.head = 0;
		tracy_data->queue.tail = 0;
		goto done;
	}
#endif

	while (tracy_data->queue.tail != tracy_data->queue.head) {
		// The signed EXT getter can be a no-op GLES driver stub. The
		// extension's unsigned availability query is the portable path.
		GLuint available = 0;
		tracy_data->get_query_available(
				tracy_data->queue.queries[tracy_data->queue.tail],
				GL_QUERY_RESULT_AVAILABLE_EXT, &available);
		if (!available) {
			goto done;
		}

		GLuint64 time;
		tracy_data->renderer->procs.glGetQueryObjectui64vEXT(
				tracy_data->queue.queries[tracy_data->queue.tail],
				GL_QUERY_RESULT_EXT, &time);

		const struct ___tracy_gpu_time_data data = {
			.context = tracy_data->context_id,
			.gpuTime = time,
			.queryId = (uint16_t) tracy_data->queue.tail,
		};
		___tracy_emit_gpu_time(data);

		tracy_data->queue.tail = (tracy_data->queue.tail + 1) % QUERY_QUEUE_LEN;
	}

done:
	TracyCZoneEnd(ctx);
}

void tracy_gpu_context_destroy(struct tracy_data *tracy_data) {
	if (tracy_data == NULL) {
		return;
	}
	id_counter--;

	tracy_data->renderer->procs.glDeleteQueriesEXT(QUERY_QUEUE_LEN, tracy_data->queue.queries);
	free(tracy_data);
}

struct tracy_data *tracy_gpu_context_new(struct fx_renderer *renderer) {
	assert(renderer);

	struct tracy_data *tracy_data = calloc(1, sizeof(*tracy_data));
	if (tracy_data == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	tracy_data->renderer = renderer;
	tracy_data->get_query_available =
		(PFNGLGETQUERYOBJECTUIVEXTPROC)eglGetProcAddress("glGetQueryObjectuivEXT");
	if (tracy_data->get_query_available == NULL) {
		wlr_log(WLR_ERROR, "Tracy requires GL_EXT_disjoint_timer_query availability queries");
		free(tracy_data);
		return NULL;
	}
	tracy_data->context_id = ++id_counter;
	tracy_data->queue.head = 0;
	tracy_data->queue.tail = 0;

	// Create the query objects
	renderer->procs.glGenQueriesEXT(QUERY_QUEUE_LEN, tracy_data->queue.queries);

	// Calibrate against a completed timestamp query. Some GLES drivers expose
	// glGetInteger64vEXT but leave its destination untouched for GL_TIMESTAMP_EXT
	// without reporting an error. Reading a completed query uses the same clock
	// as subsequent GPU zones. This wait happens only at context creation; the
	// completed slot is safe to reuse when the empty query queue starts.
	GLuint64 gl_gpu_time = 0;
	renderer->procs.glQueryCounterEXT(tracy_data->queue.queries[0], GL_TIMESTAMP_EXT);
	renderer->procs.glGetQueryObjectui64vEXT(
		tracy_data->queue.queries[0], GL_QUERY_RESULT_EXT, &gl_gpu_time);

	// Create the query objects
	GLint bits;
	renderer->procs.glGetQueryivEXT(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);

	const struct ___tracy_gpu_new_context_data data = {
		.context = tracy_data->context_id,
		.gpuTime = gl_gpu_time,
		.period = 1.0f,
		.flags = 0,
		.type = 1, // `enum class GpuContextType` in TracyQueue.hpp
	};
	___tracy_emit_gpu_new_context(data);

	// Set the custom name
	int len = snprintf(NULL, 0, "FX Renderer (GLESV2): %s",
		 glGetString(GL_RENDERER)) + 1; \
	char ctx_name[len]; \
	snprintf(ctx_name, len, "FX Renderer (GLESV2): %s",
		glGetString(GL_RENDERER)); \
	const struct ___tracy_gpu_context_name_data name_data = {
		.context = tracy_data->context_id,
		.name = ctx_name,
		.len = strlen(ctx_name),
	};
	___tracy_emit_gpu_context_name(name_data);
	return tracy_data;
}
#endif /* ifdef TRACY_ENABLE */
