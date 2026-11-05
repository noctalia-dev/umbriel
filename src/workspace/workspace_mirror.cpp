#include "workspace/workspace_mirror.h"

#include "output/output.h"
#include "scene/node.h"
#include "scene/surface_mirror.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>

namespace umbriel {
  struct WorkspaceMirror::Surface : SceneNode {
    WorkspaceMirror* owner;
    wlr_surface* surface;
    wlr_scene_buffer* buffer;
    bool seen = false;
    wl_listener commit{};
    wl_listener destroy{};
    wl_listener sample{};
    wl_listener frame{};

    Surface(WorkspaceMirror& renderer, wlr_surface* original)
        : SceneNode(SceneNodeKind::MirrorSurface), owner(&renderer), surface(original) {
      buffer = wlr_scene_buffer_create(renderer.m_tree, nullptr);
      wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
      buffer->node.data = sceneNodeData(this);
      buffer->point_accepts_input = [](wlr_scene_buffer* buffer, double* sx, double* sy) {
        auto* self = static_cast<Surface*>(sceneNodeFrom(buffer->node.data));
        if (self->surface == nullptr || buffer->dst_width <= 0 || buffer->dst_height <= 0) {
          return false;
        }
        const double x = *sx * self->surface->current.width / buffer->dst_width;
        const double y = *sy * self->surface->current.height / buffer->dst_height;
        if (!wlr_surface_point_accepts_input(self->surface, x, y)) {
          return false;
        }
        *sx = x;
        *sy = y;
        return true;
      };
      commit.notify = onCommit;
      destroy.notify = onDestroy;
      sample.notify = onSample;
      frame.notify = onFrame;
      wl_signal_add(&surface->events.commit, &commit);
      wl_signal_add(&surface->events.destroy, &destroy);
      wl_signal_add(&buffer->events.output_sample, &sample);
      wl_signal_add(&buffer->events.frame_done, &frame);
    }
    ~Surface() {
      if (surface != nullptr) {
        wl_list_remove(&commit.link);
        wl_list_remove(&destroy.link);
      }
      wl_list_remove(&sample.link);
      wl_list_remove(&frame.link);
      wlr_scene_node_destroy(&buffer->node);
    }

    static void onCommit(wl_listener* listener, void* /*data*/) {
      Surface* self;
      self = wl_container_of(listener, self, commit);
      self->handleCommit();
    }
    static void onDestroy(wl_listener* listener, void* /*data*/) {
      Surface* self;
      self = wl_container_of(listener, self, destroy);
      self->handleDestroy();
    }
    static void onSample(wl_listener* listener, void* data) {
      Surface* self;
      self = wl_container_of(listener, self, sample);
      self->handleSample(*static_cast<wlr_scene_output_sample_event*>(data));
    }
    static void onFrame(wl_listener* listener, void* data) {
      Surface* self;
      self = wl_container_of(listener, self, frame);
      self->handleFrame(*static_cast<wlr_scene_frame_done_event*>(data));
    }

    void handleCommit() { owner->schedule(); }
    void handleDestroy() {
      wl_list_remove(&commit.link);
      wl_list_remove(&destroy.link);
      surface = nullptr;
      wlr_scene_node_set_enabled(&buffer->node, false);
      owner->schedule();
    }
    void handleSample(const wlr_scene_output_sample_event& event) {
      if (surface != nullptr) {
        sampleSurfaceMirror(surface, event);
      }
    }
    void handleFrame(const wlr_scene_frame_done_event& event) {
      if (surface != nullptr) {
        frameSurfaceMirror(surface, event);
      }
    }
  };

  bool WorkspaceMirror::resolveBuffer(wlr_scene_buffer* buffer, wlr_surface** surface, View** view) {
    auto* tag = sceneNodeFrom(buffer->node.data);
    if (tag == nullptr || tag->kind != SceneNodeKind::MirrorSurface) {
      return false;
    }
    auto* entry = static_cast<Surface*>(tag);
    *surface = entry->surface;
    *view = View::fromSurface(entry->surface);
    if (*view != nullptr && (*view)->blocked()) {
      *surface = nullptr;
    }
    return true;
  }

  WorkspaceMirror::WorkspaceMirror(Workspace& workspace, wlr_scene_tree* parent)
      : m_workspace(workspace), m_tree(wlr_scene_tree_create(parent)),
        m_fullscreenTree(wlr_scene_tree_create(workspace.fullscreenTree())) {}

  WorkspaceMirror::~WorkspaceMirror() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    clear();
    wlr_scene_node_destroy(&m_tree->node);
    wlr_scene_node_destroy(&m_fullscreenTree->node);
  }

  void WorkspaceMirror::clear() { m_surfaces.clear(); }

  void WorkspaceMirror::schedule() {
    if (m_idle == nullptr) {
      auto* loop = wl_display_get_event_loop(m_workspace.group()->server()->display());
      m_idle = wl_event_loop_add_idle(loop, onIdle, this);
    }
  }

  void WorkspaceMirror::onIdle(void* data) {
    auto& self = *static_cast<WorkspaceMirror*>(data);
    self.m_idle = nullptr;
    self.sync();
  }

  void WorkspaceMirror::walkViews(wlr_scene_node* node) {
    if (auto* tag = sceneNodeFrom(node->data); tag != nullptr && tag->kind == SceneNodeKind::View) {
      auto* view = static_cast<View*>(tag);
      if (view->mapped() && !view->pinned() && !view->presentationSuppressed()) {
        walk(node, *view, true);
      }
      return;
    }
    if (!node->enabled || node->type != WLR_SCENE_NODE_TREE) {
      return;
    }
    auto* tree = wlr_scene_tree_from_node(node);
    wlr_scene_node* child;
    wl_list_for_each(child, &tree->children, link) { walkViews(child); }
  }

  void WorkspaceMirror::walk(wlr_scene_node* node, View& view, bool root, wlr_scene_tree* target) {
    if (target == nullptr) {
      target = m_tree;
      // Preserve the original scene layer, including dialogs and floats raised over fullscreen.
      const auto* fullscreen = &m_workspace.mirrorSource()->fullscreenTree()->node;
      for (auto* current = &view.sceneTree()->node; current != nullptr;
           current = current->parent != nullptr ? &current->parent->node : nullptr) {
        if (current == fullscreen) {
          target = m_fullscreenTree;
          break;
        }
      }
    }
    if (!root && !node->enabled) {
      auto* tag = sceneNodeFrom(node->data);
      if (tag == nullptr || tag->kind != SceneNodeKind::View) {
        return;
      }
      auto* attached = static_cast<View*>(tag);
      if (!attached->mapped() || attached->presentationSuppressed()) {
        return;
      }
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
      auto* tree = wlr_scene_tree_from_node(node);
      wlr_scene_node* child;
      wl_list_for_each(child, &tree->children, link) { walk(child, view, false, target); }
      return;
    }
    if (node->type != WLR_SCENE_NODE_BUFFER) {
      return;
    }
    auto* source = wlr_scene_buffer_from_node(node);
    auto* sceneSurface = wlr_scene_surface_try_from_buffer(source);
    if (sceneSurface == nullptr || sceneSurface->surface == nullptr) {
      return;
    }
    auto* surface = sceneSurface->surface;
    auto found = std::ranges::find_if(m_surfaces, [&](const auto& entry) { return entry->surface == surface; });
    if (found == m_surfaces.end()) {
      m_surfaces.push_back(std::make_unique<Surface>(*this, surface));
      found = std::prev(m_surfaces.end());
    }
    Surface& entry = **found;
    if (entry.buffer->node.parent != target) {
      wlr_scene_node_reparent(&entry.buffer->node, target);
    }
    entry.seen = true;
    int x = 0;
    int y = 0;
    for (auto* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      x += current->x;
      y += current->y;
    }
    int width = 0;
    int height = 0;
    wlr_scene_surface_get_size(sceneSurface, &width, &height);
    syncSurfaceMirror(entry.buffer, surface, source, width, height, m_workspace.group()->server()->colorManager());
    (void)umbrielfx_scene_node_share_capture_policy(&entry.buffer->node, &view.sceneTree()->node);
    wlr_scene_buffer_set_dest_size(entry.buffer, std::lround(width * m_scale), std::lround(height * m_scale));
    wlr_scene_node_set_position(
        &entry.buffer->node, m_targetX + std::lround((x - m_sourceX) * m_scale),
        m_targetY + std::lround((y - m_sourceY) * m_scale)
    );
    // Conservatively mark scaled edges translucent rather than reusing an unscaled region.
    if (m_scale != 1) {
      pixman_region32_t empty;
      pixman_region32_init(&empty);
      wlr_scene_buffer_set_opaque_region(entry.buffer, &empty);
      pixman_region32_fini(&empty);
    }
    wlr_scene_node_set_enabled(&entry.buffer->node, true);
    wlr_scene_node_raise_to_top(&entry.buffer->node);
  }

  void WorkspaceMirror::walkUnmanaged(wlr_scene_node* node) {
    if (!node->enabled) {
      return;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
      auto* tree = wlr_scene_tree_from_node(node);
      wlr_scene_node* child;
      wl_list_for_each(child, &tree->children, link) { walkUnmanaged(child); }
      return;
    }
    if (node->type != WLR_SCENE_NODE_BUFFER) {
      return;
    }
    auto* buffer = wlr_scene_buffer_from_node(node);
    auto* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
    auto* xs = sceneSurface != nullptr ? wlr_xwayland_surface_try_from_wlr_surface(sceneSurface->surface) : nullptr;
    if (xs == nullptr) {
      return;
    }
    auto* parent = xs->parent;
    for (unsigned depth = 0; parent != nullptr && depth < 64; ++depth, parent = parent->parent) {
      for (View* view : m_workspace.mirrorSource()->allViews()) {
        if (view != nullptr && view->mapped() && !view->presentationSuppressed() && view->xwaylandSurface() == parent) {
          walk(node, *view);
          return;
        }
      }
    }
  }

  void WorkspaceMirror::sync() {
    Workspace* source = m_workspace.mirrorSource();
    if (!m_workspace.active() || source == nullptr) {
      clear();
      return;
    }
    auto* server = m_workspace.group()->server();
    wlr_box from{};
    wlr_box to{};
    wlr_output_layout_get_box(server->outputLayout(), source->group()->output()->wlr(), &from);
    wlr_output_layout_get_box(server->outputLayout(), m_workspace.group()->output()->wlr(), &to);
    if (from.width <= 0 || to.width <= 0) {
      clear();
      return;
    }
    m_scale = static_cast<double>(to.width) / from.width;
    m_sourceX = from.x;
    m_sourceY = from.y;
    m_targetX = to.x;
    m_targetY = to.y + std::lround((to.height - from.height * m_scale) / 2);
    for (auto& entry : m_surfaces) {
      entry->seen = false;
    }
    // Follow scene stacking, not insertion order: focusing an older floating
    // window changes its sibling order without changing allViews().
    walkViews(&source->viewLayer(true)->node.parent->node);
    walkViews(&source->fullscreenTree()->node);
    walkUnmanaged(&server->xwaylandUnmanagedTree()->node);
    std::erase_if(m_surfaces, [](const auto& entry) { return !entry->seen; });
    wlr_output_schedule_frame(m_workspace.group()->output()->wlr());
  }
} // namespace umbriel
