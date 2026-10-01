#ifndef UMBRIELFX_SCENE_RESOURCES_H
#define UMBRIELFX_SCENE_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Internal resource accounting. These are not a released shader or compositor ABI.
#define FX_SCENE_OUTPUT_BUDGET (UINT64_C(256) * 1024 * 1024)
#define FX_SCENE_TOTAL_BUDGET (UINT64_C(512) * 1024 * 1024)

enum fx_scene_admission {
	FX_SCENE_ADMITTED,
	FX_SCENE_INVALID,
	FX_SCENE_UNSUPPORTED,
	FX_SCENE_RESOURCE_BUDGET,
};

struct fx_scene_resource_request {
	uint32_t width, height;
	uint32_t color_bytes; // RGBA8 = 4, RGBA16F = 8
	uint32_t roles; // 1 when display/capture alias; 2 when distinct
	uint32_t scratch_images; // full resolution, per role
	uint32_t texture_limit;
	uint64_t held_bytes; // existing retained resources
	uint64_t allocation_overhead; // allocator padding/metadata, for ALL images
};

struct fx_scene_resource_plan {
	uint64_t face_bytes, landing_bytes, scratch_bytes, total_bytes;
};

// Budgets one source image, its landing image, and capture scratch per role.
// On failure, out is zeroed. Limits are available bytes, not total arena sizes.
enum fx_scene_admission fx_scene_plan_resources(
	const struct fx_scene_resource_request *request, uint64_t output_available,
	uint64_t aggregate_available, struct fx_scene_resource_plan *out);

struct fx_scene_resource_pool {
	uint64_t limit, used;
};

struct fx_scene_reservation {
	struct fx_scene_resource_pool *output, *aggregate;
	uint64_t bytes;
};

// Single-threaded compositor reservation: both pools change or neither does.
// The caller releases on allocation failure before acquiring presentation.
bool fx_scene_reserve(struct fx_scene_reservation *reservation,
	struct fx_scene_resource_pool *output, struct fx_scene_resource_pool *aggregate,
	uint64_t bytes);
void fx_scene_release(struct fx_scene_reservation *reservation);


#endif
