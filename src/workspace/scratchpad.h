#pragma once

#include "config/config.h"
#include "core/animation.h"
#include "layout/layout.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_blur;

namespace umbriel {

  class Output;
  class Server;
  class View;
  class Workspace;
  struct ScratchpadSlotConfig;

  class ScratchpadManager : public Animatable {
  public:
    struct PendingCapture {
      Output* output = nullptr;
      std::string slotName;
      pid_t pid = -1;
      std::chrono::steady_clock::time_point timestamp;
    };

    ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot);
    ~ScratchpadManager() override;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;

    [[nodiscard]] bool contains(const View* view) const;
    [[nodiscard]] bool isSlotVisible(Output* output, std::string_view name = "") const;
    [[nodiscard]] bool isOutputVisible(Output* output) const;
    [[nodiscard]] bool slotHasWindows(Output* output, std::string_view name = "") const;
    // Name of the slot currently shown on `output`, or nullopt when none is.
    [[nodiscard]] std::optional<std::string> visibleSlotName(Output* output) const;
    [[nodiscard]] ScratchpadSlotConfig effectiveConfig(std::string_view name = "") const;
    void recordPendingCapture(Output* output, std::string_view slotName, pid_t pid);
    // Checks if `view` descends from a pending on_empty spawn without consuming it.
    [[nodiscard]] std::optional<PendingCapture> peekPendingCapture(const View* view) const;
    // Claims `view` only when its client descends from a pending on_empty spawn.
    [[nodiscard]] std::optional<PendingCapture> consumePendingCapture(const View* view);
    [[nodiscard]] std::optional<std::string> slotForView(const View* view) const;
    void arrangeSlot(Output* output, std::string_view name, bool animate);

    [[nodiscard]] bool moveToScratchpad(View* view, Output* output, std::string_view name = "");
    bool toggle(Output* output, std::string_view name = "");
    void hideAll();
    bool restoreFocused(Output* output, std::string_view name = "");
    bool focusNext(Output* output, std::string_view name = "");
    [[nodiscard]] View* focused(Output* output, std::string_view name = "") const;
    [[nodiscard]] bool hasFocus(Output* output) const;
    void noteFocus(View* view);
    void finishMove(View* view, Output* output);
    // Restore the manager-owned scene parents after a temporary global drag.
    void restorePresentation(View* view);
    void remove(View* view);
    void moveOutput(Output* from, Output* to);
    void releaseOutput(Output* output);
    void applyConfig();
    // Return entries to the output they were parked on. Entries whose output is still gone park on `fallback`.
    size_t restoreDisplaced(Output* fallback);

  private:
    struct Entry {
      View* view = nullptr;
      Output* output = nullptr;
      std::string name; // slot name ("" or "special" for default, or "term", "music", etc.)
      std::string returnOutput;
      std::string displacedOutput;
      // Full-output-relative x/y fractions retained until the displaced output returns.
      std::optional<std::array<double, 2>> displacedPosition;
      std::string returnWorkspace;
      bool returnTiled = false;
      bool lastFocused = false;
    };

    struct VisibleSlot {
      Output* output = nullptr;
      std::string name;
      bool operator==(const VisibleSlot&) const = default;
    };

    // A slot tiles with the same layout engine a workspace uses, so windows
    // parked in a scratchpad keep the arrangement rules the user configured.
    struct SlotLayout {
      std::unique_ptr<Layout> layout;
      ResolvedLayoutConfig config;
      std::vector<View*> members;
    };

    // Sync `slot`'s layout membership with its entries and arrange it into `box`.
    SlotLayout& layoutForSlot(std::string_view name, std::span<Entry* const> entries);

    [[nodiscard]] std::vector<std::string> visibleSlotsOn(Output* output) const;
    [[nodiscard]] std::vector<std::string> otherVisibleSlots(Output* output, std::string_view keep) const;
    // Drop a slot that is both hidden and empty.
    void pruneSlot(Output* output, std::string_view name);
    void setSlotVisible(Output* output, std::string_view name, bool visible, bool animateTransition = true);
    void retargetBackdrop(Output* output, bool visible, bool animateTransition = true);
    wlr_scene_rect* dimRectFor(Output* output);
    wlr_scene_blur* blurNodeFor(Output* output);
    void updateDimAndBlur(Output* output);

    Server* m_server = nullptr;
    wlr_scene_tree* m_root = nullptr;
    wlr_scene_tree* m_shadowRoot = nullptr;
    std::vector<Entry> m_entries;
    std::vector<VisibleSlot> m_visibleSlots;
    // Views mid animation on hide, still enabled until tickAnimations disables the node.
    std::vector<View*> m_hidingViews;
    std::unordered_map<Output*, wlr_scene_rect*> m_dimRects;
    std::unordered_map<Output*, wlr_scene_blur*> m_blurNodes;
    std::unordered_map<Output*, AnimatedValue> m_backdropFades;
    View* m_focusedView = nullptr;
    std::vector<PendingCapture> m_pendingCaptures;
    // One layout per slot name. A slot lives on a single output at a time, so
    // the name alone identifies it.
    std::unordered_map<std::string, SlotLayout> m_slotLayouts;
  };

} // namespace umbriel
