#include "render/fx_renderer/scene_resources.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); return false; \
} } while (0)

static bool test_resources(void) {
 struct fx_scene_resource_request r = {.width = 1920, .height = 1080,
   .color_bytes = 4, .roles = 2, .scratch_images = 3, .texture_limit = 16384,
   .held_bytes = 8192, .allocation_overhead = 4096};
 struct fx_scene_resource_plan p;
 const uint64_t expected = UINT64_C(1920) * 1080 * 4 * 2 * 5 + 12288;
 CHECK(fx_scene_plan_resources(&r, expected, expected, &p) == FX_SCENE_ADMITTED);
 CHECK(p.total_bytes == expected);
 CHECK(fx_scene_plan_resources(&r, expected - 1, UINT64_MAX, &p) == FX_SCENE_RESOURCE_BUDGET);
 CHECK(p.total_bytes == 0);
 CHECK(fx_scene_plan_resources(&r, UINT64_MAX, expected - 1, &p) == FX_SCENE_RESOURCE_BUDGET);
 r.color_bytes = 8;
 CHECK(fx_scene_plan_resources(&r, expected, expected, &p) == FX_SCENE_RESOURCE_BUDGET);
 r.width = r.height = r.texture_limit = UINT32_MAX;
 CHECK(fx_scene_plan_resources(&r, UINT64_MAX, UINT64_MAX, &p) == FX_SCENE_RESOURCE_BUDGET);
 r.width = 20000; r.texture_limit = 16384;
 CHECK(fx_scene_plan_resources(&r, UINT64_MAX, UINT64_MAX, &p) == FX_SCENE_UNSUPPORTED);
 r.roles = 3;
 CHECK(fx_scene_plan_resources(&r, UINT64_MAX, UINT64_MAX, &p) == FX_SCENE_INVALID);
 return true;
}

static bool test_reservations(void) {
	struct fx_scene_resource_pool aggregate = {500, 0}, a = {256, 0}, b = {256, 0};
	struct fx_scene_reservation first = {0}, second = {0}, rejected = {0};
	CHECK(fx_scene_reserve(&first, &a, &aggregate, 240));
	CHECK(!fx_scene_reserve(&rejected, &a, &aggregate, 32));
	CHECK(a.used == 240 && aggregate.used == 240 && rejected.bytes == 0);
	CHECK(!fx_scene_reserve(&first, &b, &aggregate, 1));
	CHECK(fx_scene_reserve(&second, &b, &aggregate, 256));
	CHECK(!fx_scene_reserve(&rejected, &a, &aggregate, 8));
	CHECK(a.used == 240 && b.used == 256 && aggregate.used == 496);
	fx_scene_release(&first);
	CHECK(a.used == 0 && b.used == 256 && aggregate.used == 256);
	fx_scene_release(&first);
	fx_scene_release(&second);
	CHECK(aggregate.used == 0 && b.used == 0);
	return true;
}

int main(void) {
	return test_resources() && test_reservations() ? 0 : 1;
}
