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
    View* focused(std::string_view name) const;
    bool hasFocus(std::string_view name) const;
    // The most recently focused window when that focus went to a scratchpad
    // and the window is still visible on `output`.
    View* focusedOn(const Output* output) const;
    void noteFocus(View* view);
    void finishMove(View* view, Output* output);
    // Apply the manager-owned floating presentation after a commit or while a
    // detached view animation advances.
    void syncViewPresentation(View* view, bool refreshMaximized = false);
    // Restore the manager-owned scene parents after a temporary global drag.
    void restorePresentation(View* view);
    void remove(View* view);
    void moveOutput(Output* from, Output* to);
    void releaseOutput(Output* output);
    // Keep assigned scratchpads in output-local coordinates after the output
    // layout or usable area changes.
    void refreshOutputGeometry(Output* output);
    void applyConfig();
    // Refit the maximized windows of visible scratchpads to the margin their settings give them now.
    void refitMaximized();
    // Reconcile runtime scratchpads after a successful [[scratchpad]] reload.
    // Windows belonging to removed definitions return to their saved workspaces.
    void reconcileConfig();
    // Return entries to the output they were parked on. Entries whose output is still gone park on `fallback`.
    size_t restoreDisplaced(Output* fallback);
    // A close snapshot of a member fades with the pad's own backdrop fade, so it must draw above the dim rect and blur.
    // Returns the layer to reparent the snapshot (and its shadow) into, or nullptr to leave it where it was captured.
    wlr_scene_tree* createCloseSnapshotLayer(Output* output);
    // Hand a reparented snapshot to the manager; it drops the layer once the server reaps the snapshot.
    void trackCloseSnapshot(CloseSnapshotId id, Output* output, wlr_scene_tree* layer);
    // Drop the wrapper layer of a snapshot the server has just reaped. Called synchronously from the reap so the
    // tracked list never outlives its snapshot.
    void forgetCloseSnapshot(CloseSnapshotId id);

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
      // While visible, the one member shown in place of all of them; null when every member shows.
      View* solo = nullptr;
      std::optional<PendingSpawn> pendingSpawn;
      // Set only while an output disappearance has temporarily parked this
      // scratchpad elsewhere.
      std::string displacedOutput;
      std::optional<wlr_box> usableArea;
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
    bool visibleOn(Output* output) const;
    // Whether `view`, a member of `scratchpad`, is on screen.
    static bool presents(const Scratchpad& scratchpad, const View* view);
    // Whether `view` is a dialog of another member of scratchpad `name`, shown along with that member.
    bool ridesWithParent(const View* view, std::string_view name) const;
    // The regular member of scratchpad `name` that `view` belongs to: `view` itself, or for a dialog the member it was
    // opened from.
    View* memberFor(View* view, std::string_view name) const;
    // Called as `view` leaves scratchpad `name`: if it was the window focused last, a dialog hands that over to the
    // nearest window of the scratchpad it was opened from, and any other window clears it.
    void forgetLastFocused(Scratchpad& scratchpad, std::string_view name, const View* view) const;
    enum class Admission { Interactive, Automatic };
    bool admit(
        View* view, std::string_view name, Output* invokingOutput, Admission admission,
        const AutomaticAdmission& options
    );
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
    struct TrackedCloseSnapshot {
      CloseSnapshotId id = kInvalidCloseSnapshot;
      Output* output = nullptr;
      wlr_scene_tree* layer = nullptr;
    };
    std::vector<TrackedCloseSnapshot> m_trackedCloseSnapshots;
    std::unordered_map<Output*, wlr_scene_rect*> m_dimRects;
    std::unordered_map<Output*, wlr_scene_blur*> m_blurNodes;
    std::unordered_map<Output*, AnimatedValue> m_backdropFades;
    View* m_focusedView = nullptr;
  };

} // namespace umbriel
