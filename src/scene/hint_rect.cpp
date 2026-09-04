#include "scene/hint_rect.h"

#include "config/config.h"
#include "output/output.h"
#include "scene/color.h"
#include "server/server.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr int kFadeMs = 100;
    constexpr int kMorphMs = 75;

    bool sameBox(const wlr_box& left, const wlr_box& right) {
      return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
    }
  } // namespace

  HintRect::HintRect(Server& server, wlr_scene_tree* parent) : m_server(&server), m_parent(parent) {
    m_server->registerAnimatable(this);
  }

  HintRect::~HintRect() {
    m_server->unregisterAnimatable(this);
    hideImmediate();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_rect = nullptr;
    }
  }

  void HintRect::ensureScene() {
    if (m_tree != nullptr) {
      return;
    }
    m_tree = wlr_scene_tree_create(m_parent);
    float color[4]{};
    premultiplied(color, config().colors.insertHint, 0.0F);
    m_rect = wlr_scene_rect_create(m_tree, 1, 1, color);
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void HintRect::show(Output* output, const wlr_box& box, int cornerRadius) {
    if (box.width <= 0 || box.height <= 0) {
      hide();
      return;
    }

    ensureScene();
    wlr_scene_rect_set_corner_radius(m_rect, cornerRadius);

    if (!m_visible) {
      wlr_scene_node_set_enabled(&m_tree->node, true);
      m_x.snap(box.x);
      m_y.snap(box.y);
      m_w.snap(box.width);
      m_h.snap(box.height);
      m_targetBox = box;
      m_alpha.snap(0.0);
      m_alpha.retarget(1.0, kFadeMs, Easing::EaseOutCubic);
      m_visible = true;
    } else if (output != m_output) {
      m_x.snap(box.x);
      m_y.snap(box.y);
      m_w.snap(box.width);
      m_h.snap(box.height);
      m_targetBox = box;
      if (m_alpha.target() < 1.0) {
        m_alpha.retarget(1.0, kFadeMs, Easing::EaseOutCubic);
      }
    } else if (!sameBox(box, m_targetBox)) {
      retargetGeometry(box);
      m_targetBox = box;
    }

    if (m_alpha.target() == 0.0) {
      m_alpha.retarget(1.0, kFadeMs, Easing::EaseOutCubic);
    }

    m_output = output;
    applyState();
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_output_schedule_frame(output->wlr());
  }

  void HintRect::hide() {
    if (!m_visible || m_alpha.target() == 0.0) {
      return;
    }
    m_alpha.retarget(0.0, kFadeMs, Easing::EaseOutCubic);
    if (m_output != nullptr) {
      wlr_output_schedule_frame(m_output->wlr());
    }
  }

  void HintRect::hideImmediate() {
    m_alpha.snap(0.0);
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    m_visible = false;
    m_output = nullptr;
  }

  bool HintRect::tickAnimations(uint64_t nowMsec) {
    const bool alphaTicked = m_alpha.tick(nowMsec);
    const bool xTicked = m_x.tick(nowMsec);
    const bool yTicked = m_y.tick(nowMsec);
    const bool widthTicked = m_w.tick(nowMsec);
    const bool heightTicked = m_h.tick(nowMsec);
    const bool geometryTicked = xTicked || yTicked || widthTicked || heightTicked;
    if (alphaTicked || geometryTicked) {
      applyState();
    }
    if (alphaTicked && !m_alpha.animating() && m_alpha.current() == 0.0) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
      m_output = nullptr;
      m_visible = false;
    }
    return hasActiveAnimations();
  }

  bool HintRect::hasActiveAnimations() const {
    return m_alpha.animating() || m_x.animating() || m_y.animating() || m_w.animating() || m_h.animating();
  }

  void HintRect::retargetGeometry(const wlr_box& box) {
    m_x.retarget(box.x, kMorphMs, Easing::EaseOutCubic);
    m_y.retarget(box.y, kMorphMs, Easing::EaseOutCubic);
    m_w.retarget(box.width, kMorphMs, Easing::EaseOutCubic);
    m_h.retarget(box.height, kMorphMs, Easing::EaseOutCubic);
  }

  void HintRect::applyState() {
    wlr_scene_node_set_position(&m_tree->node, std::lround(m_x.current()), std::lround(m_y.current()));
    wlr_scene_rect_set_size(m_rect, std::max(1L, std::lround(m_w.current())), std::max(1L, std::lround(m_h.current())));
    float color[4]{};
    premultiplied(color, config().colors.insertHint, static_cast<float>(m_alpha.current()));
    wlr_scene_rect_set_color(m_rect, color);
  }

} // namespace umbriel
