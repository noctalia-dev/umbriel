#ifndef TRACY_H
#define TRACY_H

#ifdef TRACY_ENABLE
#define TRACY_FN(...) __VA_ARGS__
#else
#define TRACY_FN(...) // noop
#endif  // TRACY_ENABLE

#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>

struct fx_renderer;

struct tracy_data;
struct tracy_gpu_zone_context {
	struct tracy_data *tracy_data;
	bool is_active;
};

// Private functions: Don't use these outside of this file!

void tracy_gpu_zone_begin(struct tracy_data *tracy_data, struct tracy_gpu_zone_context *out_ctx,
		const int line, const char *source, const char *func, const char *name);
void tracy_gpu_zone_end(struct tracy_gpu_zone_context *ctx);

void tracy_gpu_context_collect(struct tracy_data *tracy_data);
void tracy_gpu_context_destroy(struct tracy_data *tracy_data);
struct tracy_data *tracy_gpu_context_new(struct fx_renderer *renderer);
#endif  // TRACY_ENABLE

#define TRACY_WHEN_CONNECTED(fn) \
	TRACY_FN(if (TracyCIsConnected) { fn })

/**
 * Frame
 */

#define TRACY_MARK_FRAME TRACY_FN(TracyCFrameMark)

#define TRACY_RESCHEDULE TRACY_FN(TracyCFrameMark)

/**
 * Messaging
 */

#define TRACY_MESSAGE(...) \
	TRACY_FN({ \
		int len = snprintf(NULL, 0, __VA_ARGS__) + 1; \
		char str[len]; \
		snprintf(str, len, __VA_ARGS__); \
		TracyCMessage(str, len); \
	})
#define TRACY_MESSAGE_COLOR(color, ...) \
	TRACY_FN({ \
		int len = snprintf(NULL, 0, __VA_ARGS__) + 1; \
		char str[len]; \
		snprintf(str, len, __VA_ARGS__); \
		TracyCMessageC(str, len, color); \
	})
#define TRACY_MESSAGE_ERROR(...) \
	TRACY_FN( \
		TRACY_MESSAGE_COLOR(0xFF0000, __VA_ARGS__) \
	)

/**
 * Zone
 */

#define TRACY_ZONE_START \
	TRACY_FN( \
		TracyCZone(ctx, true) \
	)
#define TRACY_ZONE_START_N(name) \
	TRACY_FN( \
		TracyCZoneN(ctx, name, true) \
	)
#define TRACY_ZONE_END \
	TRACY_FN( \
		TRACY_ZONE_TEXT_f("Success On Line: %d", __LINE__) \
		TracyCZoneEnd(ctx); \
	)
// Ends the zone without text, for zones too short to absorb the formatting.
#define TRACY_ZONE_END_QUIET \
	TRACY_FN( \
		TracyCZoneEnd(ctx); \
	)
#define TRACY_ZONE_END_FAIL \
	TRACY_FN( \
		TRACY_ZONE_TEXT_f("Fail On Line: %d", __LINE__) \
		TracyCZoneEnd(ctx); \
	)

/**
 * Zone Helpers
 */

#define TRACY_ZONE_TEXT(txt, size) \
	TRACY_FN(TracyCZoneText(ctx, txt, size);)
#define TRACY_ZONE_TEXT_f(...) \
	TRACY_FN({ \
		int len = snprintf(NULL, 0, __VA_ARGS__) + 1; \
		char str[len]; \
		snprintf(str, len, __VA_ARGS__); \
		TracyCZoneText(ctx, str, len); \
	})
#define TRACY_ZONE_NAME(txt, size) \
	TRACY_FN( \
		TracyCZoneName(ctx, txt, size); \
	)
#define TRACY_ZONE_NAME_f(...) \
	TRACY_FN({ \
		int len = snprintf(NULL, 0, __VA_ARGS__) + 1; \
		char str[len]; \
		snprintf(str, len, __VA_ARGS__); \
		TracyCZoneName(ctx, str, len); \
	})
#define TRACY_ZONE_COLOR(color) \
	TRACY_FN( \
		TracyCZoneColor(ctx, color); \
	)
#define TRACY_ZONE_VALUE(value) \
	TRACY_FN( \
		TracyCZoneValue(ctx, value); \
	)

/**
 * GPU Zone
 */

#define TRACY_GPU_ZONE_START_NAME(renderer, name) \
	TRACY_FN( \
		struct tracy_gpu_zone_context gpu_ctx; \
		tracy_gpu_zone_begin(renderer->tracy_data, &gpu_ctx, __LINE__, __FILE__, __func__, name) \
	)
#define TRACY_GPU_ZONE_START(renderer) \
	TRACY_FN( \
		struct tracy_gpu_zone_context gpu_ctx; \
		tracy_gpu_zone_begin(renderer->tracy_data, &gpu_ctx, __LINE__, __FILE__, __func__, __func__) \
	)
#define TRACY_GPU_ZONE_END \
	TRACY_FN( \
		tracy_gpu_zone_end(&gpu_ctx) \
	)
#define TRACY_GPU_ZONE_COLLECT(renderer) \
	TRACY_FN( \
		tracy_gpu_context_collect(renderer->tracy_data) \
	)

/**
 * GPU Context
 */

#define TRACY_GPU_CONTEXT_NEW(renderer) \
	TRACY_FN( \
		tracy_gpu_context_new(renderer) \
	)
#define TRACY_GPU_CONTEXT_DESTROY(tracy_data) \
	TRACY_FN( \
		tracy_gpu_context_destroy(tracy_data); \
	)

/**
 * GPU/CPU Zones Helpers
 */

#define TRACY_BOTH_ZONES_START(renderer) \
	TRACY_FN( \
		TRACY_ZONE_START \
		TRACY_GPU_ZONE_START(renderer) \
	)
#define TRACY_BOTH_ZONES_END \
	TRACY_FN( \
		TRACY_ZONE_END \
		TRACY_GPU_ZONE_END \
	)
#define TRACY_BOTH_ZONES_END_FAIL \
	TRACY_FN( \
		TRACY_ZONE_END_FAIL \
		TRACY_GPU_ZONE_END \
	)

#endif  // !TRACY_H
