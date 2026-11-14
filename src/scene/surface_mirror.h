#pragma once

struct wlr_surface;
struct wlr_scene_buffer;
struct wlr_scene_output_sample_event;
struct wlr_scene_frame_done_event;

namespace umbriel {
  class ColorManager;

  // Copy committed surface state, independent of visibility and scene clipping.
  void syncSurfaceMirror(
      wlr_scene_buffer* target, wlr_surface* surface, wlr_scene_buffer* source, int width, int height,
      ColorManager* colorManager
  );
  void sampleSurfaceMirror(wlr_surface* surface, const wlr_scene_output_sample_event& event);
  void frameSurfaceMirror(wlr_surface* surface, const wlr_scene_frame_done_event& event);
} // namespace umbriel
