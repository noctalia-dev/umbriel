#include "scene/nine_rect.h"

#include <algorithm>
#include <cmath>
#include <drm_fourcc.h>
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
}
#include "wlr.h"

namespace umbriel {
  namespace {
    struct AssetBuffer {
      wlr_buffer base;
      std::shared_ptr<const NineRectAsset> asset;
    };
    void destroyBuffer(wlr_buffer* base) {
      AssetBuffer* b = nullptr;
      b = wl_container_of(base, b, base);
      wlr_buffer_finish(base);
      delete b;
    }
    bool access(wlr_buffer* base, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
      if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
      AssetBuffer* b = nullptr;
      b = wl_container_of(base, b, base);
      *data = const_cast<uint32_t*>(b->asset->pixels.data());
      *format = DRM_FORMAT_ARGB8888;
      *stride = static_cast<size_t>(base->width) * 4;
      return true;
    }
    void endAccess(wlr_buffer*) {}
    const wlr_buffer_impl implementation{
        .destroy = destroyBuffer,
        .get_dmabuf = nullptr,
        .get_shm = nullptr,
        .begin_data_ptr_access = access,
        .end_data_ptr_access = endAccess
    };
    wlr_buffer* buffer(std::shared_ptr<const NineRectAsset> asset) {
      auto* b = new AssetBuffer{{}, std::move(asset)};
      wlr_buffer_init(&b->base, &implementation, b->asset->width, b->asset->height);
      return &b->base;
    }
    // Owned by the node, not the View; no callbacks into destroyed decoration owners.
    struct Input {
      wl_listener destroy{};
      int width = 0, height = 0;
    };
    void destroyInput(wl_listener* listener, void*) {
      Input* input = nullptr;
      input = wl_container_of(listener, input, destroy);
      wl_list_remove(&input->destroy.link);
      delete input;
    }
    bool accepts(wlr_scene_buffer* b, double* x, double* y) {
      const auto* input = static_cast<Input*>(b->node.data);
      if (*x < 0 || *y < 0 || *x >= b->dst_width || *y >= b->dst_height)
        return false;
      const double cx = *x + b->node.x, cy = *y + b->node.y;
      return cx < 0 || cy < 0 || cx >= input->width || cy >= input->height;
    }
    void
    geometry(wlr_scene_buffer* b, int x, int y, int width, int height, const wlr_fbox& source, float rx, float ry) {
      b->slice_repeat[0] = rx;
      b->slice_repeat[1] = ry;
      wlr_scene_buffer_set_source_box(b, &source);
      wlr_scene_buffer_set_dest_size(b, width, height);
      wlr_scene_node_set_position(&b->node, x, y);
    }
  } // namespace
  bool NineRectDecoration::stale(int width, int height, const std::shared_ptr<const NineRectAsset>& asset) const {
    return m_width != width || m_height != height || m_asset != asset;
  }
  void NineRectDecoration::update(
      wlr_scene_tree* parent, std::shared_ptr<const NineRectAsset> asset, int width, int height, float scale
  ) {
    if (!asset) {
      clear();
      return;
    }
    if (!m_tree) {
      m_tree = wlr_scene_tree_create(parent);
    }
    if (m_asset != asset) {
      for (size_t i = 0; i < 9; ++i) {
        if (m_pieces[i])
          wlr_scene_node_destroy(&m_pieces[i]->node);
      }
      auto* artwork = buffer(asset);
      for (size_t i = 0; i < 9; ++i) {
        m_pieces[i] = wlr_scene_buffer_create(m_tree, artwork);
        auto* input = new Input{};
        input->destroy.notify = destroyInput;
        wl_signal_add(&m_pieces[i]->node.events.destroy, &input->destroy);
        m_pieces[i]->node.data = input;
        m_pieces[i]->point_accepts_input = accepts;
        wlr_scene_buffer_set_filter_mode(m_pieces[i], WLR_SCALE_FILTER_NEAREST);
      }
      wlr_buffer_drop(artwork);
      m_asset = std::move(asset);
    }
    m_width = width;
    m_height = height;
    const auto& a = *m_asset;
    const auto& s = a.slices;
    const int sx[4]{0, s.left, a.width - s.right, a.width};
    const int sy[4]{0, s.top, a.height - s.bottom, a.height};
    const auto dx = a.destinationCuts(width, true, scale);
    const auto dy = a.destinationCuts(height, false, scale);
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x) {
        const int i = y * 3 + x;
        const int sw = sx[x + 1] - sx[x], sh = sy[y + 1] - sy[y];
        const int dw = dx[x + 1] - dx[x], dh = dy[y + 1] - dy[y];
        const bool valid = sw > 0 && sh > 0 && dw > 0 && dh > 0;
        wlr_scene_node_set_enabled(&m_pieces[i]->node, valid);
        auto* input = static_cast<Input*>(m_pieces[i]->node.data);
        input->width = width;
        input->height = height;
        if (!valid)
          continue;
        const float rx = x == 1 && a.tileX ? dw / (sw * scale) : 1;
        const float ry = y == 1 && a.tileY ? dh / (sh * scale) : 1;
        const wlr_fbox source{
            static_cast<double>(sx[x]), static_cast<double>(sy[y]), static_cast<double>(sw), static_cast<double>(sh)
        };
        geometry(m_pieces[i], dx[x], dy[y], dw, dh, source, rx, ry);
      }
    setEnabled(m_enabled);
    setAlpha(m_alpha);
    setTint(m_tint);
  }
  void NineRectDecoration::snapshot(wlr_scene_tree* parent, int x, int y) const {
    if (!m_tree || !m_tree->node.enabled)
      return;
    for (const auto* source : m_pieces) {
      if (!source || !source->node.enabled || source->opacity == 0)
        continue;
      auto* copy = wlr_scene_buffer_create(parent, source->buffer);
      if (!copy)
        continue;
      wlr_scene_buffer_set_slice_tint(copy, source->slice_tint);
      copy->slice_repeat[0] = source->slice_repeat[0];
      copy->slice_repeat[1] = source->slice_repeat[1];
      wlr_scene_buffer_set_source_box(copy, &source->src_box);
      wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      wlr_scene_node_set_position(&copy->node, x + source->node.x, y + source->node.y);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_NEAREST);
      copy->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };
    }
  }
  void NineRectDecoration::clear() {
    if (m_tree)
      wlr_scene_node_destroy(&m_tree->node);
    m_tree = nullptr;
    m_pieces = {};
    m_asset.reset();
    m_width = m_height = 0;
  }
  void NineRectDecoration::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (m_tree)
      wlr_scene_node_set_enabled(&m_tree->node, enabled);
  }
  void NineRectDecoration::setTint(const std::array<float, 4>& color) {
    for (size_t i = 0; i < m_tint.size(); ++i)
      m_tint[i] = std::clamp(color[i], 0.0F, 1.0F);
    const std::array<float, 4> white{1, 1, 1, 1};
    const auto& resolved = m_asset && m_asset->tintWithBorderColor ? m_tint : white;
    for (auto* piece : m_pieces)
      if (piece)
        wlr_scene_buffer_set_slice_tint(piece, resolved.data());
  }
  void NineRectDecoration::setAlpha(float alpha) {
    m_alpha = std::clamp(alpha, 0.0F, 1.0F);
    for (size_t i = 0; i < 9; ++i)
      if (m_pieces[i])
        wlr_scene_buffer_set_opacity(m_pieces[i], i == 4 && !m_asset->overlay ? 0.0F : m_alpha);
  }
} // namespace umbriel
