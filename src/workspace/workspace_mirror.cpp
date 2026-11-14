#include "workspace/workspace_mirror.h"

#include "config/config.h"
#include "output/output.h"
#include "scene/node.h"
#include "scene/surface_mirror.h"
#include "server/server.h"
#include "view/view.h"
#include "view/view_preview.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>

namespace umbriel {
  struct WorkspaceMirror::Surface : PreviewSurfaceNode {
    WorkspaceMirror* owner;
    wlr_scene_buffer* buffer;
    bool seen = false;
    wl_listener commit{};
    wl_listener destroy{};
    wl_listener sample{};
    wl_listener frame{};

    Surface(WorkspaceMirror& renderer, wlr_surface* original) : owner(&renderer) {
      surface = original;
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

  WorkspaceMirror::WorkspaceMirror(Workspace& workspace, wlr_scene_tree* parent)
      : m_workspace(workspace), m_tree(wlr_scene_tree_create(parent)), m_shadowPool(wlr_scene_tree_create(m_tree)),
        m_fullscreenTree(wlr_scene_tree_create(workspace.fullscreenTree())) {}

  WorkspaceMirror::~WorkspaceMirror() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    clear();
    wlr_scene_node_destroy(&m_tree->node);
    wlr_scene_node_destroy(&m_fullscreenTree->node);
  }

  void WorkspaceMirror::clear() {
    m_previews.clear();
    m_surfaces.clear();
  }

  void WorkspaceMirror::forget(View* view) { m_previews.erase(view); }

  void WorkspaceMirror::refreshAttachments(View* view) {
    const auto found = m_previews.find(view);
    if (found == m_previews.end()) {
      return;
    }
    found->second.preview->dropAttachedSurfaces();
    found->second.preview->syncSurfaces();
    if (m_workspace.active()) {
      (void)layoutPreview(*view, *found->second.preview);
    }
    schedule();
  }

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

  wlr_scene_tree* WorkspaceMirror::targetFor(const View& view) const {
    const auto* fullscreen = &m_workspace.mirrorSource()->fullscreenTree()->node;
    for (auto* current = &view.sceneTree()->node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == fullscreen) {
        return m_fullscreenTree;
      }
    }
    return m_tree;
  }

  void WorkspaceMirror::walkViews(wlr_scene_node* node) {
    if (auto* tag = sceneNodeFrom(node->data); tag != nullptr && tag->kind == SceneNodeKind::View) {
      auto* view = static_cast<View*>(tag);
      if (view->mapped() && !view->pinned() && !view->presentationSuppressed()) {
        present(*view);
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

  void WorkspaceMirror::present(View& view) {
    wlr_scene_tree* target = targetFor(view);
    // A fullscreen window draws no frame, and its buffer may sit centered in the output with a backdrop around it: the
    // live scene places it, so copy what it shows.
    if (view.currentFullscreen()) {
      m_previews.erase(&view);
      copySurfaces(&view.sceneTree()->node, view, target, nullptr, true);
      return;
    }
    Presented& entry = m_previews[&view];
    if (entry.seen) {
      return;
    }
    Server& server = *m_workspace.group()->server();
    if (entry.preview == nullptr) {
      // Lay the refreshed buffers out before the next frame, then let the idle sync pick up any move or restack.
      entry.preview = ViewPreview::create(
          server, &view, target,
          [this, presented = &view] {
            if (const auto found = m_previews.find(presented); found != m_previews.end() && m_workspace.active()) {
              (void)layoutPreview(*presented, *found->second.preview);
            }
            schedule();
          },
          true
      );
      if (entry.preview == nullptr) {
        m_previews.erase(&view);
        return;
      }
    }
    entry.seen = true;
    ViewPreview& preview = *entry.preview;
    if (preview.tree()->node.parent != target) {
      wlr_scene_node_reparent(&preview.tree()->node, target);
    }
    // The walk meets views in stacking order, so each raise keeps the window above those below it.
    wlr_scene_node_raise_to_top(&preview.tree()->node);
    if (layoutPreview(view, preview)) {
      copySurfaces(&view.sceneTree()->node, view, target, &preview, true);
    }
  }

  bool WorkspaceMirror::layoutPreview(View& view, ViewPreview& preview) {
    const double scale = m_mapping.fit.scale;
    const ViewPreview::EffectContext effects{
        .gate =
            {
                .focused = view.borderFocused(),
                .allowUnfocused = config().effects.borderUnfocused,
                .urgent = view.urgent(),
                .fullscreen = view.scheduledFullscreen(),
            },
        .output = m_workspace.group()->output(),
        .scale = static_cast<float>(scale),
    };
    if (!preview.prepare(effects)) {
      return false;
    }
    // The scene node, not presentedBox(), holds where a window moved without a presentation sync, as a floating one
    // does while its workspace shows only through the mirror.
    int x = 0;
    int y = 0;
    (void)wlr_scene_node_coords(&view.sceneTree()->node, &x, &y);
    const wlr_box content = preview.contentSize(scale);
    preview.layout({
        .box =
            {.x = m_mapping.toReceiverX(x),
             .y = m_mapping.toReceiverY(y),
             .width = content.width,
             .height = content.height},
        .zoom = scale,
        .opacity = 1.0F,
        .innerBorderColor = view.currentBorderColor(),
        .shadowAlpha = 1.0F,
        .shadowPool = m_shadowPool,
        .effects = effects,
    });
    return true;
  }

  void WorkspaceMirror::copySurfaces(
      wlr_scene_node* node, View& view, wlr_scene_tree* target, const ViewPreview* preview, bool root
  ) {
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
      wl_list_for_each(child, &tree->children, link) { copySurfaces(child, view, target, preview, false); }
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
    if (preview != nullptr && std::ranges::any_of(preview->surfaces(), [surface](const auto& shown) {
          return shown->surface == surface;
        })) {
      return;
    }
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
    wlr_scene_buffer_set_dest_size(entry.buffer, m_mapping.toReceiverLength(width), m_mapping.toReceiverLength(height));
    wlr_scene_node_set_position(&entry.buffer->node, m_mapping.toReceiverX(x), m_mapping.toReceiverY(y));
    // Conservatively mark scaled edges translucent rather than reusing an unscaled region.
    if (m_mapping.fit.scale != 1) {
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
          copySurfaces(node, *view, targetFor(*view), nullptr, false);
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
    const std::optional<MirrorMapping> current = mapping();
    if (!current) {
      clear();
      return;
    }
    m_mapping = *current;
    for (auto& entry : m_surfaces) {
      entry->seen = false;
    }
    for (auto& [view, entry] : m_previews) {
      entry.seen = false;
    }
    // Follow scene stacking, not insertion order: focusing an older floating
    // window changes its sibling order without changing allViews().
    walkViews(&source->viewLayer(true)->node.parent->node);
    walkViews(&source->fullscreenTree()->node);
    walkUnmanaged(&m_workspace.group()->server()->xwaylandUnmanagedTree()->node);
    std::erase_if(m_surfaces, [](const auto& entry) { return !entry->seen; });
    std::erase_if(m_previews, [](const auto& entry) { return !entry.second.seen; });
    wlr_output_schedule_frame(m_workspace.group()->output()->wlr());
  }

  std::optional<MirrorMapping> WorkspaceMirror::mapping() const {
    const Workspace* source = m_workspace.mirrorSource();
    if (source == nullptr) {
      return std::nullopt;
    }
    wlr_output_layout* layout = m_workspace.group()->server()->outputLayout();
    wlr_box from{};
    wlr_box to{};
    wlr_output_layout_get_box(layout, source->group()->output()->wlr(), &from);
    wlr_output_layout_get_box(layout, m_workspace.group()->output()->wlr(), &to);
    return mirrorMapping(from, to);
  }
} // namespace umbriel
