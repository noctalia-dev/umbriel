#pragma once

#include "workspace/mirror_fit.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_node;
struct wl_event_source;
struct wlr_surface;
struct wlr_scene_buffer;

namespace umbriel {
  class ViewPreview;
  class Workspace;
  class View;

  // Presents each window of the source through a ViewPreview, so its frame, shadow, blur and effects show. The surfaces
  // a preview does not hold, such as popups, are passive committed buffers. Neither reparents a source view or drives a
  // timer.
  class WorkspaceMirror {
  public:
    WorkspaceMirror(Workspace& workspace, wlr_scene_tree* parent);
    ~WorkspaceMirror();
    WorkspaceMirror(const WorkspaceMirror&) = delete;
    WorkspaceMirror& operator=(const WorkspaceMirror&) = delete;
    void schedule();
    void clear();
    // Drops the preview of `view`, which is about to leave the source or die.
    void forget(View* view);
    // Mirrors the dialogs attached to `view` again.
    void refreshAttachments(View* view);
    // Where this mirror presents its source on the receiving output, from the outputs' current layout boxes. Empty
    // while the mirror has no source or an output has no size.
    std::optional<MirrorMapping> mapping() const;

  private:
    struct Surface;
    struct Presented {
      std::unique_ptr<ViewPreview> preview;
      bool seen = false;
    };
    static void onIdle(void* data);
    void sync();
    // The tree a view's presentation belongs under: its own scene layer, including dialogs and floats raised over
    // fullscreen.
    wlr_scene_tree* targetFor(const View& view) const;
    void walkUnmanaged(wlr_scene_node* node);
    void walkViews(wlr_scene_node* node);
    void present(View& view);
    // Places the preview of `view` with the last sync's mapping; false when the view has nothing to show.
    bool layoutPreview(View& view, ViewPreview& preview);
    // Copies every surface under `node` that `preview` does not show.
    void copySurfaces(wlr_scene_node* node, View& view, wlr_scene_tree* target, const ViewPreview* preview, bool root);
    Workspace& m_workspace;
    wlr_scene_tree* m_tree;
    // Below every preview: the shadows of windows whose shadow sits under all tiles.
    wlr_scene_tree* m_shadowPool;
    wlr_scene_tree* m_fullscreenTree;
    wl_event_source* m_idle = nullptr;
    std::unordered_map<View*, Presented> m_previews;
    std::vector<std::unique_ptr<Surface>> m_surfaces;
    // The mapping the last sync laid the presentations out with.
    MirrorMapping m_mapping;
  };
} // namespace umbriel
