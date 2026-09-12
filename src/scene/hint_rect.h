#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
}

#include <cstdint>

struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  class Output;
  class Server;

  // Animated drop-target indicator: a rounded rect that fades in on appear and morphs between target boxes. Hiding is
  // immediate, so the hint never lingers over the window a released drag settles into. Alpha and geometry are written
  // only from tickAnimations (plus snap points), never ad hoc.
  class HintRect : public Animatable {
  public:
    HintRect(Server& server, wlr_scene_tree* parent);
    ~HintRect();
    HintRect(const HintRect&) = delete;
    HintRect& operator=(const HintRect&) = delete;

    // `box` in parent-tree coordinates. A zero-sized box hides the hint.
    void show(Output* output, const wlr_box& box, int cornerRadius);
    void hideImmediate();
    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override { return m_output == output; }
    [[nodiscard]] Output* output() const { return m_output; }

  private:
    void ensureScene();
    void retargetGeometry(const wlr_box& box);
    void applyState();

    Server* m_server = nullptr;
    wlr_scene_tree* m_parent = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_rect* m_rect = nullptr;
    Output* m_output = nullptr;
    AnimatedValue m_alpha;
    AnimatedValue m_x;
    AnimatedValue m_y;
    AnimatedValue m_w;
    AnimatedValue m_h;
    wlr_box m_targetBox{};
    bool m_visible = false;
  };

} // namespace umbriel
