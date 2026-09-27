#pragma once

#include "config/config.h"
#include "config/effects.h"

#include <string>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_node;

namespace umbriel {

  // Preset names selected for one window; empty when the selector is off.
  struct ViewEffectNames {
    std::string border;
    std::string window;
  };
  [[nodiscard]] ViewEffectNames resolveViewEffectNames(const Effects& effects, const ResolvedWindowRule& rule);
  // The screen preset for an output: its `screen_effect` replaces the default; empty when off.
  [[nodiscard]] std::string resolveScreenEffectName(const Effects& effects, const OutputRule* rule);

  struct BorderEffectGate {
    bool focused = false;
    bool decorated = false;
    bool urgent = false;
    bool fullscreen = false;
  };
  [[nodiscard]] bool borderEffectApplies(const BorderEffectGate& gate);
  // A border preset's padding, 0 unless its program compiled.
  [[nodiscard]] int borderPresetPadding(const EffectPreset* preset, bool compiled);
  // Removes the window and overlay slots from `node`; null is a no-op.
  void clearWindowEffectSlots(wlr_scene_node* node);

  // Persistent effects of one view. Ledger instances are keyed by the scene node carrying the slot, so the live
  // window and its overview card track visibility and output separately.
  class ViewEffects {
  public:
    void resolve(const Effects& effects, const ResolvedWindowRule& rule);
    [[nodiscard]] const std::string& borderName() const { return m_border; }
    [[nodiscard]] const std::string& windowName() const { return m_window; }
    // Preset padding, 0 when no border preset with a compiled program is selected.
    [[nodiscard]] int borderPadding() const;
    struct ApplyInput {
      wlr_scene_node* surface = nullptr;
      wlr_scene_node* border = nullptr;
      wlr_scene_node* captureSurface = nullptr; // the isolated capture scene's surface tree node
      BorderEffectGate gate;
      float seconds = 0.0F; // the output's effect time; only read when an effect is configured
      bool clockAdvancing = true;
      const void* output = nullptr; // the output driving this instance's frames
      wlr_box outputBox{};          // that output's layout box: an instance is visible only inside it
    };
    // True when a border or window preset is selected for this view: the caller reads the clock only then.
    [[nodiscard]] bool configured() const { return !m_border.empty() || !m_window.empty(); }
    // True when apply() reads `surface`: a window preset or a border preset with an overlay is selected, or a node of
    // this view is in the ledger.
    [[nodiscard]] bool needsSurface() const { return !m_window.empty() || m_overlay || !m_owners.empty(); }
    void apply(const ApplyInput& input);
    // Ledger removal for every node this object registered; slots are cleared by the caller.
    void detach();
    // Ledger removal for one set of nodes (an overview card being destroyed).
    void detachNodes(wlr_scene_node* surface, wlr_scene_node* border);

  private:
    void track(const void* owner);   // remember a ledger owner so detach() can drop it
    void untrack(const void* owner); // remove from the ledger and forget it; null is a no-op
    void
    applyWindowSlots(const ApplyInput& input, const EffectPreset* border, float borderSeconds, bool borderAdvancing);
    std::string m_border;
    std::string m_window;
    bool m_overlay = false;            // the border preset names an overlay
    std::vector<const void*> m_owners; // nodes registered in the ledger
  };

} // namespace umbriel
