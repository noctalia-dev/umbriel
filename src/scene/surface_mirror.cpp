#include "scene/surface_mirror.h"

#include "server/color_manager.h"
#include "wlr.h"

namespace umbriel {
  void syncSurfaceMirror(
      wlr_scene_buffer* target, wlr_surface* surface, wlr_scene_buffer* source, int width, int height,
      ColorManager* colorManager
  ) {
    if (target == nullptr || surface == nullptr) {
      return;
    }
    wlr_scene_buffer_set_buffer_options options{
        .damage = &surface->buffer_damage,
        .wait_timeline = nullptr,
        .wait_point = 0,
    };
    if (auto* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
      options.wait_timeline = sync->acquire_timeline;
      options.wait_point = sync->acquire_point;
    }
    // Hidden scene buffers can release their imported buffer and retain a cropped
    // presentation. The surface's committed buffer and source box are authoritative.
    wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
    if (committed == nullptr) {
      options.damage = nullptr;
    }
    wlr_scene_buffer_set_buffer_with_options(target, committed, &options);
    wlr_fbox src{};
    wlr_surface_get_buffer_source_box(surface, &src);
    wlr_scene_buffer_set_source_box(target, &src);
    wlr_scene_buffer_set_dest_size(target, width, height);
    wlr_scene_buffer_set_transform(target, surface->current.transform);
    wlr_scene_buffer_set_opaque_region(target, &surface->opaque_region);
    if (source != nullptr) {
      wlr_scene_buffer_set_opacity(target, source->opacity);
      wlr_scene_buffer_copy_color(target, source);
    }
    if (colorManager != nullptr) {
      colorManager->applySurfaceDescriptionToBuffer(surface, target);
    }
  }

  void sampleSurfaceMirror(wlr_surface* surface, const wlr_scene_output_sample_event& event) {
    wlr_output* output = event.output->output;
    if (event.direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(surface, output);
    } else {
      wlr_presentation_surface_textured_on_output(surface, output);
    }
    if (auto* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
        sync != nullptr && event.release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event.release_timeline, event.release_point, output->event_loop
      );
    }
  }

  void frameSurfaceMirror(wlr_surface* surface, const wlr_scene_frame_done_event& event) {
    wlr_surface_send_frame_done(surface, &event.when);
  }
} // namespace umbriel
