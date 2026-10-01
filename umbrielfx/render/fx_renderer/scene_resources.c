#include "render/fx_renderer/scene_resources.h"

#include <assert.h>

static bool add(uint64_t *sum, uint64_t value) {
	if (UINT64_MAX - *sum < value) {
		return false;
	}
	*sum += value;
	return true;
}

static bool image_bytes(uint32_t width, uint32_t height, uint32_t bytes,
		uint32_t count, uint64_t *out) {
	uint64_t pixels = (uint64_t)width * height;
	uint64_t multiplier = (uint64_t)bytes * count;
	if (multiplier && pixels > UINT64_MAX / multiplier) {
		return false;
	}
	*out = pixels * multiplier;
	return true;
}

enum fx_scene_admission fx_scene_plan_resources(
		const struct fx_scene_resource_request *r, uint64_t output_available,
		uint64_t aggregate_available, struct fx_scene_resource_plan *out) {
	if (!out) {
		return FX_SCENE_INVALID;
	}
	*out = (struct fx_scene_resource_plan){0};
	if (!r || !r->width || !r->height || (r->roles != 1 && r->roles != 2) ||
			(r->color_bytes != 4 && r->color_bytes != 8) || r->scratch_images > 128) {
		return FX_SCENE_INVALID;
	}
	if (r->width > r->texture_limit || r->height > r->texture_limit) {
		return FX_SCENE_UNSUPPORTED;
	}
	struct fx_scene_resource_plan p = {.total_bytes = r->held_bytes};
	bool valid = image_bytes(r->width, r->height, r->color_bytes, r->roles, &p.face_bytes) &&
		image_bytes(r->width, r->height, r->color_bytes, r->roles, &p.landing_bytes) &&
		image_bytes(r->width, r->height, r->color_bytes,
			r->scratch_images * r->roles, &p.scratch_bytes) &&
		add(&p.total_bytes, p.face_bytes) && add(&p.total_bytes, p.landing_bytes) &&
		add(&p.total_bytes, p.scratch_bytes) &&
		add(&p.total_bytes, r->allocation_overhead);
	if (valid && p.total_bytes <= output_available && p.total_bytes <= aggregate_available) {
		*out = p;
		return FX_SCENE_ADMITTED;
	}
	return FX_SCENE_RESOURCE_BUDGET;
}

bool fx_scene_reserve(struct fx_scene_reservation *reservation,
		struct fx_scene_resource_pool *output, struct fx_scene_resource_pool *aggregate,
		uint64_t bytes) {
	if (!reservation || reservation->bytes || !output || !aggregate || output == aggregate ||
			!bytes || output->used > output->limit || aggregate->used > aggregate->limit ||
			bytes > output->limit - output->used || bytes > aggregate->limit - aggregate->used) {
		return false;
	}
	output->used += bytes;
	aggregate->used += bytes;
	*reservation = (struct fx_scene_reservation){output, aggregate, bytes};
	return true;
}

void fx_scene_release(struct fx_scene_reservation *reservation) {
	if (!reservation || !reservation->bytes) {
		return;
	}
	assert(reservation->output->used >= reservation->bytes);
	assert(reservation->aggregate->used >= reservation->bytes);
	reservation->output->used -= reservation->bytes;
	reservation->aggregate->used -= reservation->bytes;
	*reservation = (struct fx_scene_reservation){0};
}
