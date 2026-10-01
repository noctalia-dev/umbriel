#ifndef UMBRIELFX_INTERNAL_SCENE_SOURCE_H
#define UMBRIELFX_INTERNAL_SCENE_SOURCE_H

#include <stddef.h>
#include <stdint.h>
#include <umbrielfx/render/effect.h>
#include <wlr/types/wlr_scene.h>

// Private workspace source contract; no author-facing ABI or configuration surface.
// No operation mutates native node enabled/geometry/clips/output membership.
#define FX_SCENE_SOURCE_ROOT_LIMIT 1024

enum fx_scene_source_visibility {
  FX_SCENE_SOURCE_INHERIT,
  FX_SCENE_SOURCE_VISIBLE,
  FX_SCENE_SOURCE_HIDDEN,
};

struct fx_scene_source_root_override {
  struct wlr_scene_node *root;
  enum fx_scene_source_visibility visibility;
  // Additive logical translation of this root and descendants. Nested
  // overrides compose. Every sum is checked for integer overflow.
  int offset_x, offset_y;

};

struct fx_scene_source_session;
struct fx_scene_capture_plan;

struct fx_scene_source_view {
  struct wlr_scene_node *first, *last;
  const struct fx_scene_capture_plan *plan;
  struct fx_scene_scratch *scratch;
  const struct fx_scene_source_root_override *roots;
  size_t root_count;
  // Bounded face canvas, in layout coordinates. Uses the output viewport.
  struct wlr_box extent;
  float scale;
  // One live occurrence owns independent role histories; never shared by faces.
  struct fx_scene_source_session *session;
};

// Read-only source working-space selection, before reserving any role targets.
bool fx_scene_source_working_space(struct wlr_scene_output *output);
bool fx_scene_source_floating_point(struct wlr_scene_output *output);
// Validate the complete range and every override before allocation. Duplicate,
// foreign, missing, unbounded, and unsupported roots cause explicit failure.
// Lighting occurrences are synthesized from admitted border stages even when
// no native proxy survives for a hidden owner. The native light stratum and
// screen blending remain authoritative.
// Inventory budgeting: reserve all retained images plus the maximum capture
// peak when sources are captured sequentially. Old images stay charged during
// atomic refresh. Native-resolution landing storage is separate.
struct fx_scene_source_view_plan {
  uint64_t retained_bytes, capture_bytes, total_bytes;
  uint64_t history_bytes; // Persistent session reservation, separate from total_bytes.
  unsigned scratch_signature;
  bool roles_identical;
  uint64_t scratch_bytes; // Transient GPU images; capture_bytes also includes CPU/import overhead.
  int width, height;
  bool working_space; // Linear values; independent of storage precision.
  bool floating_point;
};
// One scratch set serves sequential captures; it owns no source image or history.
struct fx_scene_scratch *fx_scene_source_scratch_create(struct wlr_scene_output *output);
void fx_scene_source_scratch_destroy(struct fx_scene_scratch *scratch);
// A plan borrows the view's roots and scene nodes for one synchronous refresh.
// Rebuild it after any scene/configuration change; session state may change within the refresh.
struct fx_scene_capture_plan *fx_scene_source_prepare(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, struct fx_scene_source_view_plan *result);
void fx_scene_source_plan_destroy(struct fx_scene_capture_plan *plan);
uint64_t fx_scene_source_view_history_bytes(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view);
struct fx_scene_source_session *fx_scene_source_view_session_create(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, uint64_t reserved_history_bytes);
bool fx_scene_source_view_session_matches(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, struct fx_scene_source_session *session);
bool fx_scene_source_view_plan_for_test(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, struct fx_scene_source_view_plan *plan);
uint64_t fx_scene_source_view_bytes_for_test(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view);
bool fx_scene_capture_view_for_test(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, struct wlr_buffer *target,
    bool unfiltered, uint64_t reserved_bytes);

// Allocates both complete role images before publishing the result. Uses the
// same peak reservation query; finish with fx_scene_source_pair_finish_for_test.
struct fx_scene_source_pair_for_test;
bool fx_scene_source_view_pair_capture_for_test(struct wlr_scene_output *output,
    const struct fx_scene_source_view *view, uint64_t reserved_bytes,
    struct fx_scene_source_pair_for_test *pair);

#endif
