// The downsample source-box predicate: a scene surface always carries a source
// box (the full buffer when the client set no viewport), so "cropped" must be
// judged by the extents. Only a cropped minified source takes the sampling
// clamp variant; a whole-buffer source keeps the taps that leave it inside the
// texture's own edge, which GL_CLAMP_TO_EDGE already duplicates. No renderer or
// running desktop is required.
#undef NDEBUG
#include "umbrielfx/types/wlr_scene.h"

#include <stdio.h>

static int failures;
#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "%d: %s\n", __LINE__, #condition);                                                               \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

int main(void) {
  const int width = 800, height = 600;

  // wlr_surface_get_buffer_source_box() reports the whole buffer when the
  // client set no viewport, so this is the common case, not an edge case.
  CHECK(!wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0, 0, width, height}, width, height));
  // No source box at all means the whole buffer too.
  CHECK(!wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0}, width, height));

  // A transparent shadow margin cropped off every edge.
  CHECK(wlr_scene_source_box_is_cropped(&(struct wlr_fbox){4, 4, width - 8, height - 8}, width, height));
  // One cropped edge is enough.
  CHECK(wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0, 0, width - 1, height}, width, height));
  CHECK(wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0, 0, width, height - 1}, width, height));
  CHECK(wlr_scene_source_box_is_cropped(&(struct wlr_fbox){1, 0, width - 1, height}, width, height));
  // wp_viewport source rectangles are fractional.
  CHECK(wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0.5, 0, width - 0.5, height}, width, height));

  // Reaching past the texture is not a crop: nothing of the client's own
  // content is left outside the sample.
  CHECK(!wlr_scene_source_box_is_cropped(&(struct wlr_fbox){0, 0, width + 2, height + 2}, width, height));

  return failures ? 1 : 0;
}
