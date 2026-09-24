#pragma once
#include "config/nine_rect.h"

#include <array>
#include <memory>

struct wlr_scene_tree;
struct wlr_scene_buffer;
namespace umbriel {
  // Scene trees own nodes. Immutable source buffers survive reload through scene-buffer locks.
  class NineRectDecoration {
  public:
    void update(
        wlr_scene_tree* parent, std::shared_ptr<const NineRectAsset> asset, int width, int height, float scale = 1.0F
    );
    void snapshot(wlr_scene_tree* parent, int x, int y) const;
    void clear();
    void setEnabled(bool enabled);
    void setAlpha(float alpha);
    void setTint(const std::array<float, 4>& color);
    [[nodiscard]] bool stale(int width, int height, const std::shared_ptr<const NineRectAsset>& asset) const;
    [[nodiscard]] wlr_scene_tree* tree() const { return m_tree; }

  private:
    wlr_scene_tree* m_tree = nullptr;
    std::array<wlr_scene_buffer*, 9> m_pieces{};
    std::shared_ptr<const NineRectAsset> m_asset;
    int m_width = 0, m_height = 0;
    bool m_enabled = true;
    std::array<float, 4> m_tint{1, 1, 1, 1};
    float m_alpha = 1.0F;
  };
} // namespace umbriel
