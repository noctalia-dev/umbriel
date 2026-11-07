#pragma once

#include "workspace/mirror_fit.h"

#include <memory>
#include <optional>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_node;
struct wl_event_source;
struct wlr_surface;
struct wlr_scene_buffer;

namespace umbriel {
  class Workspace;
  class View;

  // Passive committed buffers: never reparent a source view or drive a timer.
  class WorkspaceMirror {
  public:
    WorkspaceMirror(Workspace& workspace, wlr_scene_tree* parent);
    ~WorkspaceMirror();
    WorkspaceMirror(const WorkspaceMirror&) = delete;
    WorkspaceMirror& operator=(const WorkspaceMirror&) = delete;
    void schedule();
    static bool resolveBuffer(wlr_scene_buffer* buffer, wlr_surface** surface, View** view);
    void clear();
    // Where this mirror presents its source on the receiving output, from the outputs' current layout boxes. Empty
    // while the mirror has no source or an output has no size.
    std::optional<MirrorMapping> mapping() const;

  private:
    struct Surface;
    static void onIdle(void* data);
    void sync();
    void walkUnmanaged(wlr_scene_node* node);
    void walkViews(wlr_scene_node* node);
    void walk(wlr_scene_node* node, View& view, bool root = false, wlr_scene_tree* target = nullptr);
    Workspace& m_workspace;
    wlr_scene_tree* m_tree;
    wlr_scene_tree* m_fullscreenTree;
    wl_event_source* m_idle = nullptr;
    std::vector<std::unique_ptr<Surface>> m_surfaces;
    // The mapping the last sync laid the copies out with.
    MirrorMapping m_mapping;
  };
} // namespace umbriel
