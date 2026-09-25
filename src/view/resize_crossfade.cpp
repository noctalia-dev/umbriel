#include "view/resize_crossfade.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {

    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

    int scaled(int value, double factor) { return static_cast<int>(std::lround(value * factor)); }

  } // namespace

  ResizeCrossfade::~ResizeCrossfade() { discard(); }

  void ResizeCrossfade::capture(
      wlr_scene_tree* viewTree, wlr_scene_node* above, wlr_surface* root, int width, int height, int radius
  ) {
    discard();
    if (viewTree == nullptr || above == nullptr || root == nullptr || width <= 0 || height <= 0) {
      return;
    }
    m_tree = wlr_scene_tree_create(viewTree);
    if (m_tree == nullptr) {
      return;
    }
    m_treeDestroy.notify = onTreeDestroy;
    wl_signal_add(&m_tree->node.events.destroy, &m_treeDestroy);
    wlr_scene_node_place_above(&m_tree->node, above);
    wlr_scene_node_set_enabled(&m_tree->node, false);

    struct Ctx {
      ResizeCrossfade* self;
      wlr_surface* root;
      int originX;
      int originY;
    } ctx{this, root, viewTree->node.x, viewTree->node.y};
    wlr_scene_node_for_each_buffer(
        &viewTree->node,
        [](wlr_scene_buffer* source, int sx, int sy, void* data) {
          auto& ctx = *static_cast<Ctx*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(source);
          if (source->buffer == nullptr
              || sceneSurface == nullptr
              || wlr_surface_get_root_surface(sceneSurface->surface) != ctx.root) {
            return;
          }
          const int width = source->dst_width > 0 ? source->dst_width : source->buffer->width;
          const int height = source->dst_height > 0 ? source->dst_height : source->buffer->height;
          if (width <= 0 || height <= 0) {
            return;
          }
          wlr_scene_buffer* copy = wlr_scene_buffer_create(ctx.self->m_tree, source->buffer);
          if (copy == nullptr) {
            return;
          }
          if (source->src_box.width > 0 && source->src_box.height > 0) {
            wlr_scene_buffer_set_source_box(copy, &source->src_box);
          }
          wlr_scene_buffer_set_transform(copy, source->transform);
          wlr_scene_buffer_set_transfer_function(copy, source->transfer_function);
          wlr_scene_buffer_set_primaries(copy, source->primaries);
          wlr_scene_buffer_set_luminance_multiplier(copy, source->luminance_multiplier);
          wlr_scene_buffer_set_color_encoding(copy, source->color_encoding);
          wlr_scene_buffer_set_color_range(copy, source->color_range);
          copy->point_accepts_input = rejectInput;
          ctx.self->m_buffers.push_back({
              .node = copy,
              .x = sx - ctx.originX,
              .y = sy - ctx.originY,
              .width = width,
              .height = height,
          });
        },
        &ctx
    );
    if (m_buffers.empty()) {
      discard();
      return;
    }
    m_capturedWidth = width;
    m_capturedHeight = height;
    m_radius = radius;
    present(width, height);
  }

  void ResizeCrossfade::start(int durationMs, const AnimationCurve& curve) {
    if (!pending()) {
      return;
    }
    m_fading = true;
    m_fade.snap(1.0);
    m_fade.retarget(0.0, durationMs, curve);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    applyBufferOpacity();
  }

  bool ResizeCrossfade::tick(uint64_t nowMsec) {
    if (!active() || !m_fade.tick(nowMsec)) {
      return false;
    }
    if (!m_fade.animating()) {
      discard();
      return false;
    }
    applyBufferOpacity();
    return true;
  }

  void ResizeCrossfade::present(int width, int height) {
    if (m_tree == nullptr || width <= 0 || height <= 0) {
      return;
    }
    const double scaleX = static_cast<double>(width) / m_capturedWidth;
    const double scaleY = static_cast<double>(height) / m_capturedHeight;
    const wlr_box clip{0, 0, width, height};
    wlr_scene_tree_set_clip(m_tree, &clip);
    for (const Buffer& buffer : m_buffers) {
      const int x = scaled(buffer.x, scaleX);
      const int y = scaled(buffer.y, scaleY);
      wlr_scene_node_set_position(&buffer.node->node, x, y);
      wlr_scene_buffer_set_dest_size(
          buffer.node, std::max(1, scaled(buffer.width, scaleX)), std::max(1, scaled(buffer.height, scaleY))
      );
      // Round against the presented content box, which sits at the tree origin, like the live buffers.
      const wlr_box cornerBox{-x, -y, width, height};
      wlr_scene_buffer_set_corner_radii(buffer.node, corner_radii_all(m_radius));
      wlr_scene_buffer_set_corner_box(buffer.node, m_radius > 0 ? &cornerBox : nullptr);
    }
  }

  void ResizeCrossfade::applyOpacity(float opacity) {
    m_opacity = opacity;
    applyBufferOpacity();
  }

  void ResizeCrossfade::applyBufferOpacity() {
    if (!active()) {
      return;
    }
    const float fade = std::clamp(static_cast<float>(m_fade.current()), 0.0F, 1.0F);
    const float opacity = std::clamp(m_opacity * fade, 0.0F, 1.0F);
    for (const Buffer& buffer : m_buffers) {
      wlr_scene_buffer_set_opacity(buffer.node, opacity);
    }
  }

  void ResizeCrossfade::discard() {
    m_buffers.clear();
    m_fading = false;
    m_fade.snap(1.0);
    if (m_tree == nullptr) {
      return;
    }
    wl_list_remove(&m_treeDestroy.link);
    wlr_scene_tree* tree = m_tree;
    m_tree = nullptr;
    wlr_scene_node_destroy(&tree->node);
  }

  void ResizeCrossfade::onTreeDestroy(wl_listener* listener, void* /*data*/) {
    ResizeCrossfade* self = wl_container_of(listener, self, m_treeDestroy);
    // The view tree is being torn down with this child in it; forget the nodes without touching them again.
    wl_list_remove(&self->m_treeDestroy.link);
    self->m_tree = nullptr;
    self->m_buffers.clear();
    self->m_fading = false;
    self->m_fade.snap(1.0);
  }

} // namespace umbriel
