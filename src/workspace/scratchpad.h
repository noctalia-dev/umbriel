#pragma once

#include "core/animation.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_blur;

namespace umbriel {

  class Output;
  class Server;
  class View;
  class Workspace;
  struct DisplacedWorkspace;
  struct WorkspaceLaunchAnchor;
  class ScratchpadManager : public Animatable {
  public:
    ScratchpadManager(Server& server, wlr_scene_tree* root);
    ~ScratchpadManager() override;

    AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    bool hasActiveAnimations() const override;
    bool animatesOn(const Output* output) const override;

    bool contains(const View* view) const;
    Output* outputFor(const View* view) const;
    std::string_view nameFor(const View* view) const;
    bool hasScratchpad(std::string_view name) const;
    // The workspace that arranges scratchpad `name`'s windows, or null while the pad has no output.
    Workspace* workspaceOf(std::string_view name) const;
    // Every scratchpad workspace that exists. None is in an output's inventory, so passes over those add these.
    std::vector<Workspace*> workspaces() const;
    // Re-resolve each scratchpad workspace's layout from [layout] and its [[scratchpad]] `layout` keys.
    void refreshLayouts();
    struct AutomaticAdmission {
      Output* restoreOutput = nullptr;
      Workspace* restoreWorkspace = nullptr;
      Output* focusOrigin = nullptr;
      std::optional<bool> restoreTiled;
      bool updateRestoreLocation = false;
    };
    Output* presentationOutput(std::string_view name, Output* fallback) const;
    Output* restoreOutputFor(const View* view) const;
    [[nodiscard]] bool moveToScratchpad(View* view, std::string_view name, Output* invokingOutput);
    [[nodiscard]] bool
    assignAutomatic(View* view, std::string_view name, Output* placementOutput, const AutomaticAdmission& options);
    [[nodiscard]] bool assignFromParent(View* view, const View* parent, const AutomaticAdmission& options);
    // Launch context survives an empty pad. Explicit workspace focus relinquishes it on that output.
    std::string_view activeOn(const Output* output) const;
    void deactivateOn(const Output* output);
    // Whether the scratchpad shown on `output` is modal: it owns the output's input, so windows behind it get neither
    // pointer nor focus, and it draws the backdrop.
    bool modalOn(const Output* output) const;
    // Whether the scratchpad shown on `output` stays up when the output switches workspace.
    bool stickyOn(const Output* output) const;
    // Hide the scratchpad shown on `output`, for a press on its backdrop. False when none is shown there.
    bool dismissOn(Output* output);
    // Give the keyboard to the scratchpad shown on `output`, making it the launch destination again. False when none is
    // shown there.
    bool focusOn(Output* output);
    bool refocusActive(Output* output);
    // Show a scratchpad on the invoking output without changing keyboard focus. Naming `member` while the pad shows a
    // single window shows that member instead.
    bool summon(std::string_view name, Output* invokingOutput, View* member = nullptr);
    bool toggle(std::string_view name, Output* invokingOutput);
    void hideAll();
    bool restoreFocused(std::string_view name);
    bool restoreToWorkspace(View* view, Workspace* workspace);
    bool focusNext(std::string_view name);
    // Show one window of the scratchpad at a time: the one after (`direction` > 0) or before the window on show,
    // wrapping at the ends. A hidden pad shows its most recently focused window.
    bool showStep(std::string_view name, int direction, Output* invokingOutput);
    // Show and hide on one key: hide the pad while `focus`, the seat's focused window, is one of its windows, bring it
    // here and focus it while another window has focus, and otherwise show only one window. That is the window after
    // (`direction` > 0) or before the one focused last when cycle() hid the pad, else the one focused last.
    bool cycle(std::string_view name, int direction, Output* invokingOutput, const View* focus);
    View* focused(std::string_view name) const;
    bool hasFocus(std::string_view name) const;
    // The most recently focused window when that focus went to a scratchpad
    // and the window is still visible on `output`.
    View* focusedOn(const Output* output) const;
    void noteFocus(View* view);
    void finishMove(View* view, Output* output);
    void remove(View* view);
    void moveOutput(Output* from, Output* to);
    void releaseOutput(Output* output);
    // Keep assigned scratchpads in output-local coordinates after the output
    // layout or usable area changes.
    void refreshOutputGeometry(Output* output);
    // Lift every scratchpad above fullscreen windows, below pinned ones, while a pad shown or fading on an output
    // shares it with a fullscreen window or holds one; otherwise keep them below panels. Pads share one tree, so the
    // lift is global.
    void updateStacking();
    void applyConfig();
    // Reconcile runtime scratchpads after a successful [[scratchpad]] reload.
    // Windows belonging to removed definitions return to their saved workspaces.
    void reconcileConfig();
    // Return entries to the output they were parked on. Entries whose output is still gone park on `fallback`.
    size_t restoreDisplaced(Output* fallback);

  private:
    struct PendingSpawn {
      std::chrono::steady_clock::time_point expiresAt;

      bool live() const { return std::chrono::steady_clock::now() < expiresAt; }
    };

    struct Scratchpad {
      Output* output = nullptr;
      bool visible = false;
      bool active = false;
      View* lastFocused = nullptr;
      // Where cycle() resumes once the window focused last has left the pad.
      View* resumeAt = nullptr;
      // Set while cycle() hid the pad, so its next show moves on from the window focused last.
      bool hiddenByCycle = false;
      // While visible, the one member shown in place of all of them; null when every member shows.
      View* solo = nullptr;
      std::optional<PendingSpawn> pendingSpawn;
      // Set only while an output disappearance has temporarily parked this
      // scratchpad elsewhere.
      std::string displacedOutput;
      std::optional<wlr_box> usableArea;
      // Exists exactly while `output` is set; the windows of a pad without an output have no workspace.
      std::unique_ptr<Workspace> workspace;
    };

    struct Entry {
      View* view = nullptr;
      std::string scratchpad;
      std::string returnOutput;
      // Full-output-relative x/y fractions retained until the displaced output returns.
      std::optional<std::array<double, 2>> displacedPosition;
      std::string returnWorkspace;
      size_t returnWorkspaceIndex = 0;
      bool returnWorkspaceNamed = false;
      // A refuge can share its name with a native workspace and can be pruned while its last view is in a scratchpad.
      std::weak_ptr<WorkspaceLaunchAnchor> returnWorkspaceAnchor;
      std::shared_ptr<const DisplacedWorkspace> returnDisplacedWorkspace;
      bool returnTiled = false;
    };

    Scratchpad* findScratchpad(std::string_view name);
    const Scratchpad* findScratchpad(std::string_view name) const;
    Entry* findEntry(const View* view);
    const Entry* findEntry(const View* view) const;
    bool hasEntries(std::string_view name) const;
    // Whether `view`, a member of `scratchpad`, is on screen.
    static bool presents(const Scratchpad& scratchpad, const View* view);
    // Whether `view` is a dialog of another member of scratchpad `name`, shown along with that member.
    bool ridesWithParent(const View* view, std::string_view name) const;
    // The regular member of scratchpad `name` that `view` belongs to: `view` itself, or for a dialog the member it was
    // opened from.
    View* memberFor(View* view, std::string_view name) const;
    // Whether `entry` is a mapped regular member of scratchpad `name`; dialogs show with their member instead.
    bool isMember(const Entry& entry, std::string_view name) const;
    // The regular members of scratchpad `name`, in the order they joined.
    std::vector<View*> members(std::string_view name) const;
    // The member of scratchpad `name` after (`direction` > 0) or before `view`, wrapping at the ends; null when there
    // is no other.
    View* memberBeside(std::string_view name, const View* view, int direction) const;
    // Show only `member` of scratchpad `name` on the invoking output and focus it.
    bool showOnly(std::string_view name, Scratchpad& scratchpad, View* member, Output* invokingOutput);
    // Called as `view` leaves scratchpad `name`: if it was the window focused last, a dialog hands that over to the
    // nearest window of the scratchpad it was opened from, and any other window clears it and leaves cycle() to resume
    // at the member after it.
    void forgetLastFocused(Scratchpad& scratchpad, std::string_view name, const View* view) const;
    enum class Admission { Interactive, Automatic };
    bool admit(
        View* view, std::string_view name, Output* invokingOutput, Admission admission,
        const AutomaticAdmission& options
    );
    // Create, move, or drop the scratchpad's workspace so it follows the pad's output.
    void syncWorkspace(std::string_view name, Scratchpad& scratchpad);
    void setVisible(std::string_view name, bool visible, bool animateTransition = true);
    void moveScratchpad(
        std::string_view name, Output* output, View* alreadyPositioned = nullptr, bool clearDisplacement = true
    );
    bool usesProportionalSize(std::string_view name) const;
    void remapViewGeometry(
        View* view, const wlr_box& previousArea, const wlr_box& targetArea, bool proportionalSize,
        bool reposition = true
    );
    bool restoreView(View* view, Output* fallback, bool focus, Workspace* target = nullptr);
    // Retarget the backdrop dim/blur fade for `output` and refresh its scene nodes.
    void retargetBackdrop(Output* output, bool visible, bool animateTransition = true);
    wlr_scene_rect* dimRectFor(Output* output);
    wlr_scene_blur* blurNodeFor(Output* output);
    void updateDimAndBlur(Output* output);

    Server* m_server = nullptr;
    wlr_scene_tree* m_root = nullptr;
    std::map<std::string, Scratchpad, std::less<>> m_scratchpads;
    std::vector<Entry> m_entries;
    // Views mid fade-out on hide, still enabled until tickAnimations disables the node once the fade completes.
    std::vector<View*> m_hidingViews;
    std::unordered_map<Output*, wlr_scene_rect*> m_dimRects;
    std::unordered_map<Output*, wlr_scene_blur*> m_blurNodes;
    std::unordered_map<Output*, AnimatedValue> m_backdropFades;
    View* m_focusedView = nullptr;
  };

} // namespace umbriel
