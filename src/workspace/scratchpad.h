#pragma once

#include "core/animation.h"

#include <string>
#include <unordered_map>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_rect;

namespace umbriel {

  class Output;
  class Server;
  class View;
  class Workspace;
  class ScratchpadManager : public Animatable {
  public:
    ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot);
    ~ScratchpadManager() override;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return !m_hidingViews.empty(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override;

    [[nodiscard]] bool contains(const View* view) const;
    [[nodiscard]] bool moveToScratchpad(View* view, Output* output);
    bool toggle(Output* output);
    bool restoreFocused(Output* output);
    bool focusNext(Output* output);
    [[nodiscard]] View* focused(Output* output) const;
    [[nodiscard]] bool hasFocus(Output* output) const;
    void noteFocus(View* view);
    void finishMove(View* view, Output* output);
    // Restore the manager-owned scene parents after a temporary global drag.
    void restorePresentation(View* view);
    void remove(View* view);
    void moveOutput(Output* from, Output* to);
    void releaseOutput(Output* output);

  private:
    struct Entry {
      View* view = nullptr;
      Output* output = nullptr;
      std::string returnOutput;
      std::string returnWorkspace;
      bool returnTiled = false;
      bool lastFocused = false;
    };

    void setVisible(Output* output, bool visible);
    wlr_scene_rect* dimRectFor(Output* output);
    void updateDim(Output* output);

    Server* m_server = nullptr;
    wlr_scene_tree* m_root = nullptr;
    wlr_scene_tree* m_shadowRoot = nullptr;
    std::vector<Entry> m_entries;
    std::vector<Output*> m_visibleOutputs;
    // Views mid fade-out on hide, still enabled until tickAnimations disables the node once the fade completes.
    std::vector<View*> m_hidingViews;
    // One backdrop rect per output, dimming everything behind a shown scratchpad window (Hyprland's dim_special).
    std::unordered_map<Output*, wlr_scene_rect*> m_dimRects;
    View* m_focusedView = nullptr;
  };

} // namespace umbriel
