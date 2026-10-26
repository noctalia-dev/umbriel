#include "server/actions.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/layout.h"
#include "layout/scrolling.h"
#include "output/direction.h"
#include "output/identity.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    // Forward declarations: the composite focus/move-or-output actions below
    // are defined earlier in the file than the plain output actions they fall
    // through to.
    template <wlr_direction D> bool actionOutputFocus(Server& server, const Keybind& bind, std::string* error);
    template <wlr_direction D> bool actionColumnMoveToOutput(Server& server, const Keybind& bind, std::string* error);
    void finishWorkspaceTransfer(Server& server, View& focused);

    Workspace* activeWorkspace(Server& server) {
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->active();
    }

    std::optional<std::string_view> scratchpadName(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<ScratchpadArg>(bind);
      ScratchpadManager* manager = server.scratchpadManager();
      if (arg == nullptr || manager == nullptr) {
        if (error != nullptr) {
          *error = "scratchpad action carries no scratchpad selector";
        }
        return std::nullopt;
      }

      const std::string_view name = arg->name.empty() ? std::string_view("default") : std::string_view(arg->name);
      if (arg->name.empty() && !config().scratchpads.empty()) {
        if (error != nullptr) {
          *error = "scratchpad name required";
        }
        return std::nullopt;
      }
      if (!manager->hasScratchpad(name)) {
        if (error != nullptr) {
          *error = "unknown scratchpad: " + std::string(name);
        }
        return std::nullopt;
      }
      return name;
    }

    // The action was consumed, but with a message for the caller.
    bool reject(std::string* error, std::string message) {
      if (error != nullptr) {
        *error = std::move(message);
      }
      return true;
    }

    // Positions stay on the selected output; names follow a displaced workspace's original output when qualified.
    std::expected<Workspace*, std::string> resolveWorkspaceSelector(Server& server, const Keybind& bind) {
      const auto* selector = payloadIf<WorkspaceArg>(bind);
      if (selector == nullptr) {
        return std::unexpected(std::string("action carries no workspace selector"));
      }
      Output* preferred = server.outputFromWlr(server.preferredOutput());
      WorkspaceGroup* group = preferred != nullptr ? preferred->workspaceGroup() : nullptr;
      if (const auto* name = std::get_if<WorkspaceName>(&selector->reference)) {
        return server.resolveNamedWorkspace(name->value, group, selector->output);
      }
      if (!selector->output.empty()) {
        Output* output = server.outputFromName(selector->output);
        if (output == nullptr) {
          return std::unexpected("unknown output: " + selector->output);
        }
        group = output->workspaceGroup();
        if (group == nullptr) {
          return std::unexpected("output has no workspace group: " + selector->output);
        }
      }
      const auto* index = std::get_if<WorkspaceIndex>(&selector->reference);
      if (index == nullptr) {
        return std::unexpected(std::string("action carries an invalid workspace selector"));
      }
      Workspace* target = index->value > 0 && group != nullptr ? group->workspaceAtClamped(index->value - 1) : nullptr;
      if (target == nullptr) {
        return std::unexpected(
            selector->output.empty()
                ? "unknown workspace position: " + std::to_string(index->value)
                : "unknown workspace on output " + selector->output + ": " + std::to_string(index->value)
        );
      }
      return target;
    }

    View* seatFocusedWindow(Server& server) {
      return View::fromSurface(server.seat()->wlr()->keyboard_state.focused_surface);
    }

    // Scratchpad focus follows the seat, not the pointer-selected output. A
    // cursor on another monitor must not expose that monitor's remembered
    // workspace focus to window-relative actions.
    View* focusedScratchpadWindow(Server& server) {
      View* view = seatFocusedWindow(server);
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return view != nullptr && scratchpad != nullptr && scratchpad->contains(view) ? view : nullptr;
    }

    bool scratchpadHoldsFocus(Server& server) {
      if (focusedScratchpadWindow(server) != nullptr) {
        return true;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return seatFocusedWindow(server) == nullptr
          && scratchpad != nullptr
          && !scratchpad->activeOn(server.outputFromWlr(server.preferredOutput())).empty();
    }

    // Layout-relative window actions have no workspace target while a detached
    // scratchpad owns the seat. Explicit workspace and layout actions continue
    // to use activeWorkspace(), which intentionally follows the pointer output.
    Workspace* windowActionWorkspace(Server& server) {
      return scratchpadHoldsFocus(server) ? nullptr : activeWorkspace(server);
    }

    View* focusedWindow(Server& server) {
      if (View* view = seatFocusedWindow(server)) {
        return view;
      }
      if (scratchpadHoldsFocus(server)) {
        return nullptr;
      }
      Workspace* workspace = activeWorkspace(server);
      return workspace != nullptr ? workspace->focusedView() : nullptr;
    }

    // Window IDs are ext-foreign-toplevel identifiers, the same strings
    // clients receive from the protocol and the IPC surface reuses.
    std::expected<View*, std::string> viewByForeignIdentifier(Server& server, std::string_view id) {
      for (const auto& view : server.views()) {
        if (!view->mapped()) {
          continue;
        }
        const char* identifier = view->extForeignIdentifier();
        if (identifier != nullptr && id == identifier) {
          return view.get();
        }
      }
      return std::unexpected("unknown window: " + std::string(id));
    }

    // The window id an action carries, or empty when it names none.
    std::string_view windowIdArg(const Keybind& bind) {
      const auto* arg = payloadIf<WindowIdArg>(bind);
      return arg != nullptr ? std::string_view(arg->id) : std::string_view{};
    }

    // Which focused window a window action addresses when it names none.
    enum class FocusSource : uint8_t {
      Seat,       // the keyboard focus, else the active workspace's focused window
      Scratchpad, // the keyboard focus while it is a scratchpad window, else none
      Workspace,  // the active workspace's focused window, whatever holds the keyboard
      Layout,     // the same, but none while a scratchpad window holds the keyboard
      Recent,     // the most recently focused mapped window on any active workspace
    };

    // The window whose place an action changes: the one `id` names, else the focused window `source` picks, which
    // may be null. An attached dialog's place is its parent's, so an action aimed at it acts on the window it lives
    // in. An `id` naming no mapped window is an error.
    std::expected<View*, std::string>
    placementTarget(Server& server, std::string_view id, FocusSource source = FocusSource::Seat) {
      const auto root = [](View* view) { return view != nullptr ? view->placementRoot() : nullptr; };
      if (!id.empty()) {
        return viewByForeignIdentifier(server, id).transform(root);
      }
      switch (source) {
      case FocusSource::Seat:
        return root(focusedWindow(server));
      case FocusSource::Scratchpad:
        return root(focusedScratchpadWindow(server));
      case FocusSource::Workspace: {
        if (seatFocusedWindow(server) == nullptr && scratchpadHoldsFocus(server)) {
          return nullptr;
        }
        Workspace* workspace = activeWorkspace(server);
        return workspace != nullptr ? workspace->focusedView() : nullptr;
      }
      case FocusSource::Layout: {
        Workspace* workspace = windowActionWorkspace(server);
        return workspace != nullptr ? workspace->focusedView() : nullptr;
      }
      case FocusSource::Recent:
        for (const auto& view : server.registry().placementRoots()) {
          if (view->mapped() && !view->minimized() && view->onActiveWorkspace()) {
            return view.get();
          }
        }
        return nullptr;
      }
      return nullptr;
    }

    // The window an action addresses itself, not its place: its client (close, kill, screencast) or its own effects.
    // The one `id` names, else the focused window that FocusSource::Seat picks, which may be null. An `id` naming no
    // mapped window is an error.
    std::expected<View*, std::string> clientTarget(Server& server, std::string_view id) {
      if (!id.empty()) {
        return viewByForeignIdentifier(server, id);
      }
      return focusedWindow(server);
    }

    const std::vector<double>& floatingPresetsFor(View& view) {
      if (Output* output = view.currentOutput(); output != nullptr && output->workspaceGroup() != nullptr) {
        if (Workspace* workspace = output->workspaceGroup()->active()) {
          return workspace->layoutConfig().extentPresets;
        }
      }
      return config().layout.extentPresets;
    }

    template <int Direction> void cycleScratchpadSize(View& view, bool width) {
      const auto axis = view.floatingAxisBasis(width);
      if (!axis) {
        return;
      }
      const auto presets = boundedPresets(floatingPresetsFor(view), 1.0);
      const double current = presetSnappedFraction(presets, (*axis)[0], (*axis)[1]);
      const double next = nextFractionPreset(presets, current, Direction);
      view.resizeFloatingFractions(
          width ? std::optional(next) : std::nullopt, width ? std::nullopt : std::optional(next)
      );
    }

    // Move `view` to `target` (possibly on another output), activate the target workspace, and focus the view. Floats
    // land proportionally via their remembered usable-area fraction. With `follow` clear the window lands on `target`
    // while the seat stays on the source workspace and the cursor stays where it was.
    void moveViewToWorkspace(Server& server, View& view, Workspace& target, bool follow = true) {
      Workspace* source = view.workspace();
      const bool workspaceChanged = source != &target;
      ScratchpadManager* scratchpad = server.scratchpadManager();
      const bool fromScratchpad = scratchpad != nullptr && scratchpad->contains(&view);
      Output* sourceOutput = fromScratchpad                 ? scratchpad->outputFor(&view)
          : source != nullptr && source->group() != nullptr ? source->group()->output()
                                                            : nullptr;
      if (fromScratchpad && !scratchpad->restoreToWorkspace(&view, &target)) {
        return;
      }
      const bool floating = view.floating();
      std::optional<double> widthFrac;
      bool fullWidth = false;
      if (!floating && target.scrollingLayout() != nullptr) {
        const ScrollingLayout* sourceLayout = source != nullptr ? source->scrollingLayout() : nullptr;
        if (sourceLayout != nullptr) {
          const int column = sourceLayout->columnOf(&view);
          const auto& columns = sourceLayout->columns();
          if (column >= 0 && column < static_cast<int>(columns.size())) {
            const Column& sourceColumn = columns[static_cast<size_t>(column)];
            widthFrac = sourceColumn.savedWidthFrac > 0.0 ? sourceColumn.savedWidthFrac : sourceColumn.widthFrac;
            fullWidth = sourceLayout->isFullWidth(column);
          }
        }
      }
      if (floating) {
        view.rememberFloatingPosition();
      }
      if (!fromScratchpad) {
        view.moveToWorkspace(&target, true, LayoutAttachOrigin::MovedView); // layoutAttach self-guards on tiled()
      }
      if (workspaceChanged && !view.tiled()) {
        // layoutAttach only handles tiled arrivals.
        target.exitFullscreenForIncomingView(&view);
      }
      if (widthFrac.has_value()) {
        ScrollingLayout* targetLayout = target.scrollingLayout();
        const int column = targetLayout != nullptr ? targetLayout->columnOf(&view) : -1;
        if (column >= 0) {
          targetLayout->setWidthFraction(column, *widthFrac);
          if (fullWidth && !targetLayout->isFullWidth(column)) {
            targetLayout->toggleFullWidth(column);
          }
          target.markArrange(true);
        }
      }
      if (!follow) {
        if (floating) {
          view.restoreFloatingPosition();
        }
        // Detaching the view already handed the source workspace's focus to a replacement, so the seat only follows
        // that replacement; a source left empty clears the keyboard focus, the way an empty workspace does elsewhere.
        // The cursor deliberately stays put: nothing travelled with the window.
        if (workspaceChanged) {
          server.refocus(sourceOutput);
        }
        return;
      }
      target.group()->activate(&target);
      if (floating) {
        view.restoreFloatingPosition();
      }
      server.focusView(&view, FocusReason::Directional);
      if (workspaceChanged) {
        finishWorkspaceTransfer(server, view);
      }
    }

    void rebuildTransferredColumn(Workspace& target, const Column& column, int targetIndex) {
      if (column.views.empty()) {
        return;
      }
      if (target.layoutMode() == LayoutMode::Floating) {
        for (View* view : column.views) {
          view->moveToWorkspace(&target);
        }
        return;
      }
      View* first = column.views.front();
      first->moveToWorkspace(&target, /*attachToLayout=*/false);
      target.layout().insertView(first, targetIndex);
      View* insertionAnchor = first;
      for (size_t row = 1; row < column.views.size(); ++row) {
        View* view = column.views[row];
        view->moveToWorkspace(&target, /*attachToLayout=*/false);
        target.layout().insertViewIntoColumn(view, target.layout().columnOf(insertionAnchor), static_cast<int>(row));
        if (target.layout().columnOf(view) != target.layout().columnOf(first)) {
          // Splitting layouts flatten a source stack. Advance the insertion
          // anchor so three or more members retain their original order.
          insertionAnchor = view;
        }
      }

      if (ScrollingLayout* scrolling = target.scrollingLayout()) {
        const int targetColumn = scrolling->columnOf(first);
        const double normalWidth = column.savedWidthFrac > 0.0 ? column.savedWidthFrac : column.widthFrac;
        scrolling->setWidthFraction(targetColumn, normalWidth);
        if (column.savedWidthFrac > 0.0) {
          scrolling->toggleFullWidth(targetColumn);
        }
        scrolling->adoptTabs(targetColumn, column.tabs);
        for (size_t row = 0; row < column.heightWeights.size(); row = column.tabs.unitEnd(row)) {
          scrolling->setHeightWeight(targetColumn, static_cast<int>(row), column.heightWeights[row]);
        }
        scrolling->setTopGapWeight(targetColumn, column.topGapWeight);
        scrolling->setBottomGapWeight(targetColumn, column.bottomGapWeight);
      } else if (TabbedContainers* tabs = target.layout().tabbedContainers()) {
        const int targetColumn = target.layout().columnOf(first);
        const auto& targetColumns = target.layout().columns();
        if (targetColumn < 0
            || targetColumn >= static_cast<int>(targetColumns.size())
            || targetColumns[static_cast<size_t>(targetColumn)].views != column.views) {
          // A group merged with existing area rows cannot keep its own display.
          return;
        }
        // A master area can show one group spanning all its rows, not separate
        // groups and standalone rows. Its new-area default cannot change the source display.
        const auto& groups = column.tabs.groups();
        const bool wholeGroup =
            groups.size() == 1 && groups.front().first == 0 && groups.front().count == column.views.size();
        tabs->setTabbed(first, wholeGroup);
        if (wholeGroup) {
          const TabGroup& group = groups.front();
          tabs->selectTab(column.views[group.active]);
          if (group.bar.has_value()) {
            tabs->setTabBar(first, group.bar);
          }
        }
      }
    }

    // Move the column of the window FocusSource::Layout picks to `target` as one structural unit. Rebuild it only
    // after snapshotting because every view transfer mutates the source layout. A floating window has no column, so
    // it follows the single-window behavior used by the directional output actions.
    void moveFocusedColumnToWorkspace(Server& server, Workspace& target) {
      Workspace* source = windowActionWorkspace(server);
      View* focused = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr);
      if (source == nullptr || source == &target || focused == nullptr) {
        return;
      }
      const int columnIndex = source->layout().columnOf(focused);
      const auto& sourceColumns = source->layout().columns();
      if (columnIndex < 0 || columnIndex >= static_cast<int>(sourceColumns.size())) {
        moveViewToWorkspace(server, *focused, target);
        return;
      }

      const Column column = sourceColumns[static_cast<size_t>(columnIndex)];
      if (column.views.empty()) {
        return;
      }

      target.exitFullscreenForIncomingView(focused);

      const int focusedTargetColumn = target.layout().columnOf(target.focusedView());
      const int targetIndex =
          focusedTargetColumn >= 0 ? focusedTargetColumn + 1 : static_cast<int>(target.layout().columns().size());
      rebuildTransferredColumn(target, column, targetIndex);

      target.markArrange();
      target.group()->activate(&target);
      server.focusView(focused, FocusReason::Directional);
      finishWorkspaceTransfer(server, *focused);
    }

    // Adjacent output in `direction` from the focused (cursor) output; null with
    // a message when none exists. No wrap-around.
    Output* adjacentOutput(Server& server, wlr_direction direction, std::string* error) {
      Output* reference = server.outputFromWlr(server.preferredOutput());
      if (reference == nullptr) {
        if (error != nullptr) {
          *error = "no outputs";
        }
        return nullptr;
      }

      OutputDirection outputDirection;
      switch (direction) {
      case WLR_DIRECTION_LEFT:
        outputDirection = OutputDirection::Left;
        break;
      case WLR_DIRECTION_RIGHT:
        outputDirection = OutputDirection::Right;
        break;
      case WLR_DIRECTION_UP:
        outputDirection = OutputDirection::Up;
        break;
      case WLR_DIRECTION_DOWN:
        outputDirection = OutputDirection::Down;
        break;
      default:
        if (error != nullptr) {
          *error = "no output in that direction";
        }
        return nullptr;
      }

      std::vector<Output*> outputs;
      std::vector<OutputBox> boxes;
      size_t referenceIndex = 0;
      bool foundReference = false;
      for (const auto& output : server.outputs()) {
        wlr_box box{};
        wlr_output_layout_get_box(server.outputLayout(), output->wlr(), &box);
        if (box.width <= 0 || box.height <= 0) {
          continue;
        }
        if (output.get() == reference) {
          referenceIndex = boxes.size();
          foundReference = true;
        }
        outputs.push_back(output.get());
        boxes.push_back({box.x, box.y, box.width, box.height});
      }

      const std::optional<size_t> adjacent = foundReference
          ? adjacentOutputIndex(
                boxes, referenceIndex, outputDirection, server.cursor()->wlr()->x, server.cursor()->wlr()->y
            )
          : std::nullopt;
      if (!adjacent) {
        if (error != nullptr) {
          const char* name = nullptr;
          switch (direction) {
          case WLR_DIRECTION_LEFT:
            name = "left";
            break;
          case WLR_DIRECTION_RIGHT:
            name = "right";
            break;
          case WLR_DIRECTION_UP:
            name = "above";
            break;
          case WLR_DIRECTION_DOWN:
            name = "below";
            break;
          default:
            name = "that direction";
            break;
          }
          *error = std::string("no output to the ") + name;
        }
        return nullptr;
      }
      return outputs[*adjacent];
    }

    // The output `step` places away from the focused (cursor) output in layout order, wrapping at both ends. Null with
    // a message when this session has only one output.
    Output* cycledOutput(Server& server, int step, std::string* error) {
      Output* reference = server.outputFromWlr(server.preferredOutput());
      if (reference == nullptr) {
        if (error != nullptr) {
          *error = "no outputs";
        }
        return nullptr;
      }

      std::vector<Output*> outputs;
      std::vector<OutputBox> boxes;
      size_t referenceIndex = 0;
      bool foundReference = false;
      for (const auto& output : server.outputs()) {
        wlr_box box{};
        wlr_output_layout_get_box(server.outputLayout(), output->wlr(), &box);
        if (box.width <= 0 || box.height <= 0) {
          continue;
        }
        if (output.get() == reference) {
          referenceIndex = boxes.size();
          foundReference = true;
        }
        outputs.push_back(output.get());
        boxes.push_back({box.x, box.y, box.width, box.height});
      }

      const std::optional<size_t> next = foundReference ? cyclicOutputIndex(boxes, referenceIndex, step) : std::nullopt;
      if (!next) {
        if (error != nullptr) {
          *error = "no other output";
        }
        return nullptr;
      }
      return outputs[*next];
    }

    // Warp the cursor to the center of `output`'s usable area so subsequent
    // actions resolve against the target monitor (focus is cursor-defined).
    void warpToOutputCenter(Server& server, Output& output) {
      const wlr_box usable = output.usableArea();
      server.cursor()->warpTo(usable.x + usable.width / 2.0, usable.y + usable.height / 2.0);
    }

    // Session
    bool actionSpawn(Server& server, const Keybind& bind, std::string* /*error*/) {
      const auto* arg = payloadIf<SpawnArg>(bind);
      server.spawn(arg != nullptr ? arg->command.c_str() : "", nullptr, true);
      return true;
    }

    bool actionSessionQuit(Server& server, const Keybind& bind, std::string* /*error*/) {
      const auto* arg = payloadIf<QuitArg>(bind);
      const bool skip = arg != nullptr && arg->skipConfirmation;
      QuitConfirm* confirm = server.quitConfirm();
      // While locked the dialog would be hidden behind the lock surface, so quit
      // directly; the lock client's own UI is the confirmation there.
      if (!skip && !server.sessionLocked() && confirm != nullptr && !confirm->visible()) {
        confirm->show(QuitConfirm::Kind::SessionQuit);
        return true;
      }
      server.requestSessionExit();
      return true;
    }

    bool actionConfigReload(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      const ConfigReloadResult result = reloadConfig();
      server.handleConfigReload(result);
      return true;
    }

    bool actionCursorHide(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.cursor()->hideCursor();
      return true;
    }

    bool actionCursorShow(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.cursor()->noteActivity();
      return true;
    }

    template <bool Powered> bool actionDpms(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<OutputArg>(bind);
      const std::string requested = arg != nullptr ? arg->output : std::string{};
      bool found = false;
      bool changed = false;
      for (const auto& output : server.outputs()) {
        const char* name = output->wlr()->name;
        if (!requested.empty() && outputNameMatch(output->identity(), requested) == OutputNameMatch::None) {
          continue;
        }
        found = true;
        if (!output->desktopEnabled()) {
          if (!requested.empty()) {
            return reject(error, "output is disabled: " + requested);
          }
          continue;
        }
        if (!output->setPowered(Powered)) {
          return reject(error, "failed to change output power: " + std::string(name != nullptr ? name : "unknown"));
        }
        changed = true;
      }
      if (!found) {
        return reject(error, "unknown output: " + requested);
      }
      if (!changed) {
        return reject(error, "no enabled outputs");
      }
      return true;
    }

    enum class OutputEnableAction {
      Disable,
      Enable,
      Toggle,
    };

    template <OutputEnableAction Action>
    bool actionOutputEnablement(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<OutputArg>(bind);
      if (arg == nullptr || arg->output.empty()) {
        return reject(error, "output name is required");
      }

      Output* target = nullptr;
      OutputNameMatch targetMatch = OutputNameMatch::None;
      for (const auto& output : server.outputs()) {
        const OutputNameMatch match = outputNameMatch(output->identity(), arg->output);
        if (match == OutputNameMatch::None) {
          continue;
        }
        if (match == OutputNameMatch::Connector) {
          target = output.get();
          targetMatch = match;
          break;
        }
        if (target != nullptr && targetMatch == OutputNameMatch::Descriptor) {
          return reject(error, "output descriptor is ambiguous: " + arg->output);
        }
        target = output.get();
        targetMatch = match;
      }
      if (target == nullptr) {
        return reject(error, "unknown output: " + arg->output);
      }

      bool enabled = Action == OutputEnableAction::Enable;
      if constexpr (Action == OutputEnableAction::Toggle) {
        enabled = !target->desktopEnabled();
      }
      if (!server.setOutputEnabled(*target, enabled)) {
        return reject(error, "failed to change logical output state: " + arg->output);
      }
      return true;
    }

    bool actionOutputSdrWhite(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<SdrWhiteArg>(bind);
      if (arg == nullptr) {
        return reject(error, "action carries no sdr white level");
      }

      Output* output = nullptr;
      if (arg->output.empty()) {
        output = server.outputFromWlr(server.preferredOutput());
      } else {
        for (const auto& candidate : server.outputs()) {
          if (outputNameMatch(candidate->identity(), arg->output) != OutputNameMatch::None) {
            output = candidate.get();
            break;
          }
        }
      }
      if (output == nullptr) {
        return reject(error, arg->output.empty() ? "no focused output" : "unknown output: " + arg->output);
      }
      output->setRuntimeSdrWhite(static_cast<float>(arg->nits));
      return true;
    }

    bool actionKeyboardLayoutNext(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      return server.cycleKeyboardLayout();
    }

    bool actionShortcutsInhibitToggle(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (server.toggleKeyboardShortcutsInhibit()) {
        return true;
      }
      return reject(error, "keyboard shortcuts inhibition requires an unlocked focused surface");
    }

    bool actionSubmap(Server& server, const Keybind& bind, std::string* /*error*/) {
      const auto* arg = payloadIf<SubmapArg>(bind);
      if (arg == nullptr) {
        return false;
      }
      if (isSubmapReset(*arg)) {
        if (!server.inSubmap()) {
          return false;
        }
        server.popSubmap();
      } else {
        server.pushSubmap(arg->name);
      }
      return true;
    }

    bool actionScreenCastClear(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.clearScreenCastTarget();
      return true;
    }

    bool actionScreenCastSetWindow(Server& server, const Keybind& bind, std::string* error) {
      const auto target = clientTarget(server, windowIdArg(bind));
      if (!target.has_value()) {
        return reject(error, target.error());
      }
      View* view = *target;
      if (view == nullptr) {
        return reject(error, "no focused window");
      }
      if (view->extForeignIdentifier() == nullptr) {
        return reject(error, "window has no capture identifier");
      }
      return server.setScreenCastWindow(*view, error);
    }

    bool actionScreenCastSetOutput(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<OutputArg>(bind);
      Output* output = arg != nullptr && !arg->output.empty() ? server.outputFromName(arg->output)
                                                              : server.outputFromWlr(server.preferredOutput());
      if (output == nullptr) {
        return reject(
            error, arg != nullptr && !arg->output.empty() ? "unknown output: " + arg->output : "no focused output"
        );
      }
      return server.setScreenCastOutput(*output, error);
    }

    bool actionScreenCastFollowWindow(Server& server, const Keybind& /*bind*/, std::string* error) {
      return server.followScreenCastWindow(error);
    }

    bool actionScreenCastFollowOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
      return server.followScreenCastOutput(error);
    }

    bool actionScreenCastFollowStop(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.stopFollowingScreenCast();
      return true;
    }

    bool warpCursorToWindow(Server& server, View& view) { return server.cursor()->warpToView(view); }

    bool maybeWarpCursorToWindow(Server& server, View* view) {
      Overview* overview = server.overview();
      if (config().input.cursor.followsFocus && view != nullptr && (overview == nullptr || !overview->active())) {
        return warpCursorToWindow(server, *view);
      }
      return false;
    }

    void finishWorkspaceTransfer(Server& server, View& focused) {
      if (maybeWarpCursorToWindow(server, &focused)) {
        return;
      }
      Output* destination = focused.currentOutput();
      if (destination != nullptr && destination != server.outputFromWlr(server.preferredOutput())) {
        warpToOutputCenter(server, *destination);
      }
    }

    void focusWindowFromNavigation(Server& server, View* view) {
      if (view == nullptr) {
        return;
      }
      server.focusView(view, FocusReason::Directional);
      maybeWarpCursorToWindow(server, view);
    }

    bool actionWindowClose(Server& server, const Keybind& bind, std::string* error) {
      const auto target = clientTarget(server, windowIdArg(bind));
      if (!target.has_value()) {
        if (error != nullptr) {
          *error = target.error();
        }
        return false;
      }
      if (View* view = *target) {
        view->requestClose();
      }
      return true;
    }

    bool actionWindowKill(Server& server, const Keybind& bind, std::string* error) {
      const auto target = clientTarget(server, windowIdArg(bind));
      if (!target.has_value()) {
        return reject(error, target.error());
      }
      View* view = *target;
      if (view == nullptr) {
        return true;
      }
      const pid_t pid = view->pid();
      // Nonpositive PIDs signal process groups, and X11 clients can supply their own PID.
      if (pid <= 1 || pid == getpid()) {
        return reject(error, "window has no killable client process");
      }
      if (kill(pid, SIGKILL) != 0) {
        return reject(error, std::string("failed to kill window process: ") + std::strerror(errno));
      }
      return true;
    }

    bool actionWindowMinimize(Server& server, const Keybind& bind, std::string* error) {
      const auto target = placementTarget(server, windowIdArg(bind));
      if (!target.has_value()) {
        return reject(error, target.error());
      }
      View* view = *target;
      if (view == nullptr || view->minimized()) {
        return true;
      }
      if (!view->minimizable()) {
        return reject(error, "window-minimize needs a floating, unpinned window on a workspace");
      }
      if (!view->requestMinimize()) {
        return reject(error, "window-minimize cannot hide a fullscreen window or one being moved or resized");
      }
      return true;
    }

    bool actionWindowRestoreMinimized(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      View* latest = nullptr;
      for (View* view : workspace->allViews()) {
        if (view->mapped()
            && view->minimized()
            && (latest == nullptr || view->minimizeSerial() > latest->minimizeSerial())) {
          latest = view;
        }
      }
      if (latest != nullptr) {
        server.focusView(latest, FocusReason::ForeignActivation);
        maybeWarpCursorToWindow(server, latest);
      }
      return true;
    }

    bool actionWorkspaceToggleShowDesktop(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleShowDesktop();
      }
      return true;
    }

    bool tiledDragActive(Server& server) {
      const Cursor* cursor = server.cursor();
      return cursor != nullptr && cursor->isDraggingIntoLayout();
    }

    void invalidateHoverFocusAfterSceneChange(Server& server, bool changed) {
      if (changed && config().input.focus.followsMouse) {
        server.cursor()->invalidateHoverFocus();
      }
    }

    template <int Sign> void scrollActiveLayout(Server& server, int multiplier = 1) {
      Workspace* workspace = activeWorkspace(server);
      ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
      if (scrolling == nullptr || workspace->group()->output() == nullptr) {
        return;
      }
      const auto step = static_cast<double>(config().input.mouse.scrollWheelStep * multiplier);
      const int viewportPrimary = workspace->scrollViewportExtent();
      const auto maxScroll = static_cast<double>(scrolling->maxScroll(viewportPrimary));
      const double oldScroll = scrolling->scroll();
      const double newScroll = std::clamp(oldScroll + Sign * step, 0.0, maxScroll);
      if (newScroll == oldScroll) {
        return;
      }
      scrolling->setScroll(newScroll);
      workspace->markArrange();
      if (config().input.focus.followsMouse) {
        workspace->deferScrollHoverFocus();
      }
    }

    bool actionLayoutScrollDrag(Server& /*server*/, const Keybind& bind, std::string* error) {
      if (bind.mouseButton == 0) {
        if (error != nullptr) {
          *error = "layout-scroll-drag requires a mouse-button binding";
        }
        return false;
      }
      // Cursor owns the motion and release portion of this press-triggered action.
      return true;
    }

    template <int Direction> bool actionFocusAdjacent(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (bind.wheel != WheelDirection::None && tiledDragActive(server)) {
        scrollActiveLayout<Direction>(server, 2);
        return true;
      }
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusAdjacent(Direction)) {
          focusWindowFromNavigation(server, target);
        }
      }
      return true;
    }

    template <int Direction, wlr_direction WlrDir>
    bool actionFocusHorizontalOrOutput(Server& server, const Keybind& bind, std::string* error) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusAdjacent(Direction)) {
          focusWindowFromNavigation(server, target);
          return true;
        }
      }
      return actionOutputFocus<WlrDir>(server, bind, error);
    }

    template <int Direction, wlr_direction WlrDir>
    bool actionFocusVerticalOrOutput(Server& server, const Keybind& bind, std::string* error) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusVertical(Direction)) {
          focusWindowFromNavigation(server, target);
          return true;
        }
      }
      return actionOutputFocus<WlrDir>(server, bind, error);
    }

    // Tab or untab the focused column; nullopt toggles. A layout whose columns cannot be tabbed refuses.
    bool setFocusedColumnTabbed(Server& server, std::optional<bool> tabbed, std::string* error) {
      Workspace* workspace = windowActionWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      if (workspace->layout().tabbedContainers() == nullptr) {
        return reject(error, "tabs need a layout with columns: scrolling or master");
      }
      workspace->setFocusedColumnTabbed(tabbed);
      return true;
    }

    bool actionColumnToggleTabbed(Server& server, const Keybind& /*bind*/, std::string* error) {
      return setFocusedColumnTabbed(server, std::nullopt, error);
    }

    bool actionColumnSetDisplay(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<ColumnDisplayArg>(bind);
      if (arg == nullptr) {
        return true;
      }
      return setFocusedColumnTabbed(server, arg->display == ColumnDisplay::Tabbed, error);
    }

    template <int Direction>
    bool actionColumnFocusTabStep(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        focusWindowFromNavigation(server, workspace->tabs().stepTarget(Direction));
      }
      return true;
    }

    bool actionColumnFocusTab(Server& server, const Keybind& bind, std::string* /*error*/) {
      const auto* arg = payloadIf<TabIndexArg>(bind);
      Workspace* workspace = windowActionWorkspace(server);
      if (arg != nullptr && workspace != nullptr) {
        focusWindowFromNavigation(server, workspace->tabs().indexTarget(arg->index));
      }
      return true;
    }

    template <int Direction> bool actionColumnMoveTab(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->moveFocusedTab(Direction);
      }
      return true;
    }

    // Draw (1) or hide (0) the focused tab group's bar, or toggle it (-1).
    template <int Shown> bool actionColumnTabBar(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->setFocusedTabBar(Shown < 0 ? std::nullopt : std::optional<bool>(Shown > 0));
      }
      return true;
    }

    template <int Direction> bool actionFocusVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusVertical(Direction)) {
          focusWindowFromNavigation(server, target);
        }
      }
      return true;
    }

    template <int Direction>
    bool actionFocusVerticalOrWorkspace(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusVertical(Direction)) {
          focusWindowFromNavigation(server, target);
          return true;
        }
      }
      // No layout window in this direction. A detached scratchpad has no
      // layout neighbor, so retain the composite action's workspace fallback.
      Workspace* workspace = activeWorkspace(server);
      WorkspaceGroup* group = workspace != nullptr ? workspace->group() : nullptr;
      if (group == nullptr) {
        return true;
      }
      const size_t index = workspace->index();
      if (Direction < 0 && index == 0) {
        return true;
      }
      Workspace* targetWorkspace = group->workspaceAt(index + static_cast<size_t>(Direction));
      if (targetWorkspace != nullptr && targetWorkspace != group->active()) {
        group->select(targetWorkspace);
        Workspace* selected = group->active();
        maybeWarpCursorToWindow(server, selected != nullptr ? selected->focusedView() : nullptr);
      }
      return true;
    }

    template <int Direction> bool actionMoveColumn(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->moveFocusedColumn(Direction));
      }
      return true;
    }

    template <int Direction>
    bool actionMoveHorizontal(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->moveFocusedHorizontal(Direction));
      }
      return true;
    }

    bool actionFocusFirstColumn(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusFirstColumn()) {
          focusWindowFromNavigation(server, target);
        }
      }
      return true;
    }

    bool actionFocusLastColumn(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->focusLastColumn()) {
          focusWindowFromNavigation(server, target);
        }
      }
      return true;
    }

    bool actionMoveColumnFirst(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->moveFocusedColumnFirst();
      }
      return true;
    }

    bool actionMoveColumnLast(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->moveFocusedColumnLast();
      }
      return true;
    }

    template <int Direction, wlr_direction WlrDir>
    bool actionMoveHorizontalOrOutput(Server& server, const Keybind& bind, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = windowActionWorkspace(server)) {
        const View* focused = workspace->focusedView();
        const bool moved = focused != nullptr && focused->floating() ? workspace->moveFocusedHorizontal(Direction)
                                                                     : workspace->moveFocusedColumn(Direction);
        if (moved) {
          maybeWarpCursorToWindow(server, workspace->focusedView());
          return true;
        }
      }
      return actionColumnMoveToOutput<WlrDir>(server, bind, error);
    }

    template <int Direction, wlr_direction WlrDir>
    bool actionMoveVerticalOrOutput(Server& server, const Keybind& bind, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (workspace->moveFocusedVertical(Direction)) {
          maybeWarpCursorToWindow(server, workspace->focusedView());
          return true;
        }
      }
      return actionColumnMoveToOutput<WlrDir>(server, bind, error);
    }

    template <int Direction> bool actionMoveVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->moveFocusedVertical(Direction);
      }
      return true;
    }

    // The workspace one step from `index`, wrapping around the ends of the
    // inventory when the output enables cyclic workspaces. Null means the step
    // leaves the inventory, or that the output owns no workspace at all.
    Workspace* stepWorkspace(WorkspaceGroup& group, size_t index, int step) {
      const size_t count = group.workspaceCount();
      if (count == 0) {
        return nullptr;
      }
      const long long target = static_cast<long long>(index) + step;
      if (target >= 0 && target < static_cast<long long>(count)) {
        return group.workspaceAt(static_cast<size_t>(target));
      }
      const Output* output = group.output();
      if (output == nullptr || !output->configuredCyclicWorkspaces()) {
        return nullptr;
      }
      return step < 0 ? group.workspaceAt(count - 1) : group.workspaceAt(0);
    }

    template <int Direction>
    bool actionMoveVerticalOrWorkspace(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (!workspace->moveFocusedVertical(Direction)) {
          Workspace* source = workspace;
          if (source->group() == nullptr) {
            return true;
          }
          Workspace* target = stepWorkspace(*source->group(), source->index(), Direction);
          if (target == nullptr || target == source) {
            return true;
          }
          if (View* view = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr)) {
            moveViewToWorkspace(server, *view, *target);
          }
        }
      }
      return true;
    }

    template <int Direction> bool actionConsume(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->consumeFocused(Direction));
      }
      return true;
    }

    template <int Direction>
    bool actionConsumeOrExpel(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        const bool changed = workspace->expelFocused(Direction) || workspace->consumeFocused(Direction);
        invalidateHoverFocusAfterSceneChange(server, changed);
      }
      return true;
    }

    // The mirror of actionConsume: pulls the neighboring column's window in, leaving focus where it was.
    template <int Direction> bool actionConsumeFrom(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->consumeFromFocused(Direction));
      }
      return true;
    }

    template <int Direction> bool actionCycleWidth(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        cycleScratchpadSize<Direction>(*view, true);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->cycleFocusedWidth(Direction));
      }
      return true;
    }

    template <int Direction> bool actionCycleHeight(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        cycleScratchpadSize<Direction>(*view, false);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->cycleFocusedHeight(Direction));
      }
      return true;
    }

    bool actionSetWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          view->resizeFloatingFractions(std::clamp(arg->fraction, 0.1, 1.0), std::nullopt);
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          invalidateHoverFocusAfterSceneChange(server, workspace->setFocusedWidth(arg->fraction));
        }
      }
      return true;
    }

    bool actionModifyWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        if (const auto* arg = payloadIf<FractionArg>(bind); arg != nullptr) {
          if (const auto current = view->floatingFraction(true)) {
            view->resizeFloatingFractions(std::clamp(*current + arg->fraction, 0.1, 1.0), std::nullopt);
          }
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          invalidateHoverFocusAfterSceneChange(server, workspace->modifyFocusedWidth(arg->fraction));
        }
      }
      return true;
    }

    bool actionSetHeight(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          view->resizeFloatingFractions(std::nullopt, std::clamp(arg->fraction, 0.1, 1.0));
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          invalidateHoverFocusAfterSceneChange(server, workspace->setFocusedHeight(arg->fraction));
        }
      }
      return true;
    }

    bool actionModifyHeight(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        if (const auto* arg = payloadIf<FractionArg>(bind); arg != nullptr) {
          if (const auto current = view->floatingFraction(false)) {
            view->resizeFloatingFractions(std::nullopt, std::clamp(*current + arg->fraction, 0.1, 1.0));
          }
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<FractionArg>(bind)) {
          invalidateHoverFocusAfterSceneChange(server, workspace->modifyFocusedHeight(arg->fraction));
        }
      }
      return true;
    }

    // Edge-anchored resize: `Edges` names the edge that moves and the opposite
    // one stays put, so a positive argument grows the window there until the size
    // saturates.
    template <uint32_t Edges> bool actionResizeEdge(Server& server, const Keybind& bind, std::string* /*error*/) {
      const auto* arg = payloadIf<FractionArg>(bind);
      if (arg == nullptr) {
        return true;
      }
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        view->resizeFloatingEdge(Edges, arg->fraction);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->resizeFocusedEdge(Edges, arg->fraction));
      }
      return true;
    }

    bool actionToggleMaximize(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        if (view->maximizedToEdges()) {
          view->setMaximizedToEdges(false);
        }
        if (!view->scheduledFullscreen()) {
          view->toggleMaximized();
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFullWidth();
      }
      return true;
    }

    bool actionToggleMaximizeToEdges(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        view->toggleMaximizedToEdges();
        server.scratchpadManager()->restorePresentation(view);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedMaximizedToEdges();
      }
      return true;
    }

    bool actionToggleFullscreen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        view->toggleFullscreen();
        server.scratchpadManager()->restorePresentation(view);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        invalidateHoverFocusAfterSceneChange(server, workspace->toggleFocusedFullscreen());
      }
      return true;
    }

    bool actionToggleFloating(Server& server, const Keybind& bind, std::string* error) {
      if (const std::string_view id = windowIdArg(bind); !id.empty()) {
        const auto target = placementTarget(server, id);
        if (!target.has_value()) {
          if (error != nullptr) {
            *error = target.error();
          }
          return false;
        }
        View* view = *target;
        if (view->workspace() != nullptr && view->workspace()->layoutMode() == LayoutMode::Floating) {
          return reject(error, "window-toggle-floating needs a tiling layout");
        }
        view->toggleFloating(false);
        return true;
      } else {

        if (scratchpadHoldsFocus(server)) {
          return true;
        }
        if (Workspace* workspace = activeWorkspace(server)) {
          if (workspace->layoutMode() == LayoutMode::Floating) {
            return reject(error, "window-toggle-floating needs a tiling layout");
          }
          workspace->toggleFocusedFloating();
        }
      }
      return true;
    }

    bool actionTogglePinned(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (View* view = placementTarget(server, {}, FocusSource::Seat).value_or(nullptr)) {
        view->togglePinned();
      }
      return true;
    }

    bool actionWindowSnap(Server& server, const Keybind& bind, std::string* error) {
      View* view = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr);
      if (view == nullptr || view->tiled()) {
        return reject(error, "window-snap needs a floating window");
      }
      const auto* arg = payloadIf<SnapRegionArg>(bind);
      if (arg == nullptr || !view->snapTo(arg->region)) {
        return reject(error, "window-snap: the window cannot fit that region or its current state prevents snapping");
      }
      return true;
    }

    bool actionWindowUnsnap(Server& server, const Keybind&, std::string*) {
      if (View* view = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr)) {
        view->unsnap();
      }
      return true;
    }

    bool actionWindowCenter(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr)) {
        view->centerFloating();
        return true;
      }
      if (View* view = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr)) {
        view->centerFloating();
      }
      return true;
    }

    bool actionWindowMoveTo(Server& server, const Keybind& bind, std::string* error) {
      if (const auto* arg = payloadIf<WindowMoveToArg>(bind); arg != nullptr) {
        const auto target = placementTarget(server, arg->id);
        if (!target.has_value()) {
          return reject(error, target.error());
        }
        if (*target == nullptr) {
          return reject(error, "no focused window");
        }
        (*target)->moveFloatingTo(arg->position);
      }
      return true;
    }

    bool actionWindowMoveBy(Server& server, const Keybind& bind, std::string* error) {
      if (const auto* arg = payloadIf<WindowMoveByArg>(bind); arg != nullptr) {
        const auto target = placementTarget(server, arg->id);
        if (!target.has_value()) {
          return reject(error, target.error());
        }
        if (*target == nullptr) {
          return reject(error, "no focused window");
        }
        (*target)->moveFloatingBy(arg->deltaX, arg->deltaY);
      }
      return true;
    }

    bool actionColumnCenter(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (workspace->layoutMode() != LayoutMode::Scrolling) {
          return reject(error, "column-center requires the scrolling layout");
        }
        workspace->centerFocusedColumn();
      }
      return true;
    }

    template <int Direction> bool actionFocusCycle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (View* target = workspace->cycleFocusTarget(Direction)) {
          focusWindowFromNavigation(server, target);
        }
      }
      return true;
    }

    template <int Direction> bool actionSwapCycle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->swapFocusedInCycle(Direction);
      }
      return true;
    }

    template <bool Horizontal, int Direction>
    bool actionSwapDirectional(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->swapFocusedInDirection(Horizontal, Direction);
      }
      return true;
    }

    bool actionLayoutMasterCountIncrease(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->increaseMasterCount();
      }
      return true;
    }

    bool actionLayoutMasterCountDecrease(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->decreaseMasterCount();
      }
      return true;
    }

    bool actionLayoutToggleSplit(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (workspace->layoutMode() != LayoutMode::Dwindle) {
          return reject(error, "layout-toggle-split requires the dwindle layout");
        }
        workspace->toggleDwindleSplit();
      }
      return true;
    }

    template <bool Warp> bool actionWindowFocusId(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<WindowIdArg>(bind);
      if (arg == nullptr || arg->id.empty()) {
        if (error != nullptr) {
          *error = Warp ? "window-focus-warp requires a window id" : "window-focus requires a window id";
        }
        return false;
      }
      const auto target = placementTarget(server, arg->id);
      if (!target.has_value()) {
        if (error != nullptr) {
          *error = target.error();
        }
        return false;
      }
      View* view = *target;
      if (ScratchpadManager* scratchpad = server.scratchpadManager();
          scratchpad != nullptr && scratchpad->contains(view) && !view->onActiveWorkspace()) {
        // Hidden scratchpad entries fail FocusManager's visibility gate. Summon
        // their pad using the normal pointer-output policy, then select the
        // exact requested entry below without briefly focusing its remembered
        // window first.
        if (Output* output = server.outputFromWlr(server.preferredOutput()); output != nullptr) {
          scratchpad->summon(scratchpad->nameFor(view), output, view);
        }
      }
      server.focusView(view, FocusReason::ForeignActivation);
      if constexpr (Warp) {
        warpCursorToWindow(server, *view);
      } else {
        maybeWarpCursorToWindow(server, view);
      }
      return true;
    }

    bool actionWindowFocusLast(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      View* current = placementTarget(server, {}, FocusSource::Seat).value_or(nullptr);
      for (const auto& entry : server.registry().placementRoots()) {
        View* target = entry.get();
        if (target == current || !target->mapped() || target->minimized() || target->workspace() == nullptr) {
          continue;
        }
        server.focusView(target, FocusReason::ForeignActivation);
        maybeWarpCursorToWindow(server, target);
        break;
      }
      return true;
    }

    bool actionWorkspaceFocusLast(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      WorkspaceGroup* group = workspace->group();
      if (group == nullptr) {
        return true;
      }
      Workspace* target = group->previous();
      if (target == nullptr || target == group->active()) {
        return true;
      }
      group->select(target);
      Workspace* selected = group->active();
      maybeWarpCursorToWindow(server, selected != nullptr ? selected->focusedView() : nullptr);
      return true;
    }

    bool actionFocusSwitchFloating(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = windowActionWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      View* focused = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr);
      const bool seekFloating = focused == nullptr || !focused->floating();
      View* target = nullptr;
      for (const auto& entry : server.registry().placementRoots()) {
        View* view = entry.get();
        if (view == focused || !view->mapped() || view->minimized() || view->workspace() != workspace) {
          continue;
        }
        if (view->floating() != seekFloating) {
          continue;
        }
        target = view;
        break;
      }
      if (target != nullptr && target != focused) {
        focusWindowFromNavigation(server, target);
      }
      return true;
    }

    // Workspaces
    bool actionWorkspace(Server& server, const Keybind& bind, std::string* error) {
      const std::expected<Workspace*, std::string> target = resolveWorkspaceSelector(server, bind);
      if (!target.has_value()) {
        return reject(error, target.error());
      }

      WorkspaceGroup* group = (*target)->group();
      const auto warpToTargetOutput = [&] {
        Output* destination = group->output();
        if (destination != nullptr && destination != server.outputFromWlr(server.preferredOutput())) {
          warpToOutputCenter(server, *destination);
        }
      };
      if (bind.action == KeybindAction::WindowMoveToWorkspace
          || bind.action == KeybindAction::WindowMoveToWorkspaceSilent) {
        View* view = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr);
        if (view == nullptr && scratchpadHoldsFocus(server)) {
          return true;
        }
        const bool follow = bind.action == KeybindAction::WindowMoveToWorkspace;
        if (view == nullptr) {
          view = placementTarget(server, {}, FocusSource::Recent).value_or(nullptr);
        }
        if (view != nullptr) {
          moveViewToWorkspace(server, *view, **target, follow);
          return true;
        }
        if (!follow) {
          // Nothing to move: no-op, rather than falling through to the workspace switch below.
          return true;
        }
      }
      if (bind.action == KeybindAction::ColumnMoveToWorkspace) {
        moveFocusedColumnToWorkspace(server, **target);
        return true;
      }
      group->select(*target);
      warpToTargetOutput();
      return true;
    }

    template <int Direction>
    bool actionWorkspaceAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      WorkspaceGroup* group = workspace->group();
      if (group == nullptr) {
        return true;
      }
      Workspace* target = stepWorkspace(*group, group->active()->index(), Direction);
      if (target == nullptr || target == group->active()) {
        return true;
      }
      group->select(target);
      return true;
    }

    template <int Direction, bool Follow>
    bool actionWindowMoveToWorkspaceAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      View* scratchpadView = placementTarget(server, {}, FocusSource::Scratchpad).value_or(nullptr);
      Output* scratchpadOutput =
          scratchpadView != nullptr ? server.scratchpadManager()->outputFor(scratchpadView) : nullptr;
      Workspace* workspace = scratchpadOutput != nullptr && scratchpadOutput->workspaceGroup() != nullptr
          ? scratchpadOutput->workspaceGroup()->active()
          : windowActionWorkspace(server);
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      Workspace* target = stepWorkspace(*workspace->group(), workspace->index(), Direction);
      if (target == nullptr || target == workspace) {
        return true;
      }
      View* view = scratchpadView != nullptr ? scratchpadView
                                             : placementTarget(server, {}, FocusSource::Layout).value_or(nullptr);
      if (view != nullptr) {
        moveViewToWorkspace(server, *view, *target, Follow);
      }
      return true;
    }

    template <int Direction>
    bool actionColumnMoveToWorkspaceAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* source = windowActionWorkspace(server);
      if (source == nullptr || source->group() == nullptr) {
        return true;
      }
      Workspace* target = stepWorkspace(*source->group(), source->index(), Direction);
      if (target == nullptr || target == source) {
        return true;
      }
      moveFocusedColumnToWorkspace(server, *target);
      return true;
    }

    bool actionWorkspaceSetLayout(Server& server, const Keybind& bind, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      const auto* arg = payloadIf<LayoutModeArg>(bind);
      if (arg == nullptr) {
        return true;
      }
      static constexpr std::array kFullCycle{
          LayoutMode::Scrolling, LayoutMode::Dwindle, LayoutMode::Master, LayoutMode::Floating
      };
      const std::span<const LayoutMode> cycle = arg->toggleSequence.empty()
          ? std::span<const LayoutMode>(kFullCycle)
          : std::span<const LayoutMode>(arg->toggleSequence);
      // A mode outside the cycle, or its last mode, toggles to the first.
      const auto current = std::ranges::find(cycle, workspace->layoutMode());
      const bool wrap = current == cycle.end() || std::next(current) == cycle.end();
      const LayoutMode desired = arg->mode.value_or(wrap ? cycle.front() : *std::next(current));
      if (desired == workspace->layoutMode()) {
        return true;
      }
      workspace->overrideLayoutMode(desired);
      // The layout behind an interactive tiled resize is being replaced; drop
      // the stale session, same as the config-reload layout swap.
      server.cursor()->cancelStaleTiledResize();
      return true;
    }

    template <int Direction> bool actionWorkspaceMove(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      workspace->group()->moveActiveWorkspace(Direction);
      return true;
    }

    template <int Sign> bool actionLayoutScroll(Server& server, const Keybind& bind, std::string* /*error*/) {
      const int multiplier = bind.wheel != WheelDirection::None && tiledDragActive(server) ? 2 : 1;
      scrollActiveLayout<Sign>(server, multiplier);
      return true;
    }

    // Outputs
    bool focusOutput(Server& server, Output& target) {
      warpToOutputCenter(server, target);
      server.refocusExplicit(&target);
      WorkspaceGroup* group = target.workspaceGroup();
      Workspace* workspace = group != nullptr ? group->active() : nullptr;
      maybeWarpCursorToWindow(server, workspace != nullptr ? workspace->focusedView() : nullptr);
      return true;
    }

    template <wlr_direction D> bool actionOutputFocus(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return focusOutput(server, *target);
    }

    template <int Step> bool actionOutputFocusCycle(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = cycledOutput(server, Step, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return focusOutput(server, *target);
    }

    bool moveFocusedWindowToOutput(Server& server, Output& target, std::string* error) {
      WorkspaceGroup* targetGroup = target.workspaceGroup();
      Workspace* destination = targetGroup != nullptr ? targetGroup->active() : nullptr;
      if (destination == nullptr) {
        return reject(error, "output has no workspace");
      }
      View* view = placementTarget(server, {}, FocusSource::Layout).value_or(nullptr);
      if (view == nullptr) {
        return true; // nothing focused: silent no-op
      }
      moveViewToWorkspace(server, *view, *destination);
      return true;
    }

    template <wlr_direction D>
    bool actionWindowMoveToOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return moveFocusedWindowToOutput(server, *target, error);
    }

    template <int Step>
    bool actionWindowMoveToOutputCycle(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      std::string message;
      Output* target = cycledOutput(server, Step, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return moveFocusedWindowToOutput(server, *target, error);
    }

    template <wlr_direction D>
    bool actionColumnMoveToOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      WorkspaceGroup* targetGroup = target->workspaceGroup();
      Workspace* destination = targetGroup != nullptr ? targetGroup->active() : nullptr;
      if (destination == nullptr) {
        return reject(error, "output has no workspace");
      }
      moveFocusedColumnToWorkspace(server, *destination); // nothing focused: silent no-op
      return true;
    }

    template <wlr_direction D>
    bool actionWorkspaceMoveToOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      Workspace* source = activeWorkspace(server);
      if (source == nullptr || !source->hasViews()) {
        return reject(error, "workspace is empty");
      }
      WorkspaceGroup* targetGroup = target->workspaceGroup();
      if (targetGroup == nullptr) {
        return reject(error, "output has no workspace");
      }
      Workspace* destination = targetGroup->transferDestination();
      if (destination == nullptr) {
        return reject(
            error, targetGroup->dynamic() ? "output workspace limit reached" : "output has no empty fixed workspace"
        );
      }
      View* focused = source->focusedView();

      // Snapshot the source contents first: every setWorkspace below triggers reconcileDynamic on both groups, and
      // iterating the live layout while it rebuilds would be use-after-free.
      const bool sameLayout = source->layout().mode() == destination->layout().mode();
      const LayoutCapture tiles = sameLayout ? source->layout().captureState() : LayoutCapture{};
      const std::vector<Column> columns = sameLayout ? std::vector<Column>{} : source->layout().columns();
      std::vector<View*> floats;
      for (View* view : source->allViews()) {
        if (view->floating() && !view->pinned()) {
          floats.push_back(view);
        }
      }

      if (sameLayout) {
        // Restore into the empty destination before moving any members, so a
        // rejected snapshot leaves the source intact.
        if (tiles.snapshot == nullptr || !destination->layout().restoreState(*tiles.snapshot, tiles.members)) {
          return reject(error, "failed to restore workspace layout");
        }
        for (const LayoutMember& member : tiles.members) {
          if (member.view != nullptr) {
            member.view->moveToWorkspace(destination, /*attachToLayout=*/false);
          }
        }
      } else {
        for (const Column& column : columns) {
          rebuildTransferredColumn(*destination, column, static_cast<int>(destination->layout().columns().size()));
        }
      }
      for (View* view : floats) {
        view->rememberFloatingPosition();
        view->moveToWorkspace(destination);
        view->restoreFloatingPosition();
      }

      destination->markArrange();
      targetGroup->activate(destination);
      if (focused != nullptr && focused->workspace() == destination) {
        server.focusView(focused, FocusReason::Directional);
      } else {
        server.refocusExplicit(target);
      }
      warpToOutputCenter(server, *target);
      return true;
    }

    bool swapActiveWorkspaceWindows(Server& server, Output* sourceOutput, Output* targetOutput, std::string* error) {
      if (sourceOutput == nullptr || targetOutput == nullptr || sourceOutput == targetOutput) {
        return reject(error, "invalid outputs for swap");
      }
      WorkspaceGroup* sourceGroup = sourceOutput->workspaceGroup();
      WorkspaceGroup* targetGroup = targetOutput->workspaceGroup();
      if (sourceGroup == nullptr || targetGroup == nullptr) {
        return reject(error, "output has no workspace group");
      }
      Workspace* sourceWs = sourceGroup->active();
      Workspace* targetWs = targetGroup->active();
      if (sourceWs == nullptr || targetWs == nullptr) {
        return reject(error, "output has no active workspace");
      }
      if (!sourceWs->hasViews() && !targetWs->hasViews()) {
        return true;
      }

      View* sourceFocused = sourceWs->focusedView();
      View* targetFocused = targetWs->focusedView();
      View* seatFocus = seatFocusedWindow(server);

      double sourceScroll = 0.0;
      bool sourceCenteredRest = false;
      if (const ScrollingLayout* sc = sourceWs->scrollingLayout()) {
        sourceScroll = sc->scroll();
        sourceCenteredRest = sc->centeredRest();
      }

      double targetScroll = 0.0;
      bool targetCenteredRest = false;
      if (const ScrollingLayout* sc = targetWs->scrollingLayout()) {
        targetScroll = sc->scroll();
        targetCenteredRest = sc->centeredRest();
      }

      // The layout snapshots itself, so a dwindle split tree and a master area ratio survive the transfer the same
      // way scrolling's column widths do. restoreState refuses a snapshot from another mode, which is exactly the
      // case where no structure can be replayed: those windows keep their order and the destination layout shapes
      // them.
      const LayoutCapture sourceTiles = sourceWs->layout().captureState();
      const LayoutCapture targetTiles = targetWs->layout().captureState();

      const auto snapshotFloats = [](Workspace* ws) {
        std::vector<View*> floats;
        for (View* view : ws->allViews()) {
          if (view->floating() && !view->pinned()) {
            floats.push_back(view);
          }
        }
        return floats;
      };
      const std::vector<View*> sourceFloats = snapshotFloats(sourceWs);
      const std::vector<View*> targetFloats = snapshotFloats(targetWs);

      const auto transfer = [](const LayoutCapture& tiles, const std::vector<View*>& floats, Workspace* dest) {
        for (const LayoutMember& member : tiles.members) {
          if (member.view != nullptr) {
            member.view->moveToWorkspace(dest, /*attachToLayout=*/false);
          }
        }
        for (View* view : floats) {
          view->rememberFloatingPosition();
          view->moveToWorkspace(dest, /*attachToLayout=*/false);
        }
      };
      transfer(sourceTiles, sourceFloats, targetWs);
      transfer(targetTiles, targetFloats, sourceWs);

      const auto rebuild = [](Workspace* dest, const LayoutCapture& tiles, const std::vector<View*>& floats) {
        if (dest->layoutMode() == LayoutMode::Floating) {
          for (const LayoutMember& member : tiles.members) {
            if (member.view != nullptr) {
              dest->layoutAttach(member.view);
            }
          }
        } else if (tiles.snapshot == nullptr || !dest->layout().restoreState(*tiles.snapshot, tiles.members)) {
          for (const LayoutMember& member : tiles.members) {
            if (member.view != nullptr) {
              dest->layout().insertView(member.view, static_cast<int>(dest->layout().columns().size()));
            }
          }
        }
        for (View* view : floats) {
          if (dest->layoutMode() != LayoutMode::Floating && view->layoutFloated()) {
            view->setFloating(false, false);
          } else {
            view->restoreFloatingPosition();
          }
        }
      };
      rebuild(targetWs, sourceTiles, sourceFloats);
      rebuild(sourceWs, targetTiles, targetFloats);

      if (ScrollingLayout* sc = targetWs->scrollingLayout()) {
        sc->setScroll(sourceScroll, sourceCenteredRest);
        targetWs->clampScrollToRange();
      }

      if (ScrollingLayout* sc = sourceWs->scrollingLayout()) {
        sc->setScroll(targetScroll, targetCenteredRest);
        sourceWs->clampScrollToRange();
      }

      // Every transfer already handed each workspace a layout-aware replacement focus, so the remembered view is only
      // reinstated where it actually landed, and the fallback covers a workspace left with no focus at all.
      if (sourceFocused != nullptr && sourceFocused->workspace() == targetWs) {
        targetWs->setFocusedView(sourceFocused);
      } else if (targetWs->focusedView() == nullptr && targetWs->hasViews()) {
        targetWs->setFocusedView(targetWs->allViews().front());
      }

      if (targetFocused != nullptr && targetFocused->workspace() == sourceWs) {
        sourceWs->setFocusedView(targetFocused);
      } else if (sourceWs->focusedView() == nullptr && sourceWs->hasViews()) {
        sourceWs->setFocusedView(sourceWs->allViews().front());
      }

      // Without follow-warp, keep the seat on the pointer-selected source output and use the focus that arrived there.
      // With it enabled, retain the previously focused view so the seat and pointer travel together. Gesture avoids
      // revealing either target's column: the restored scroll offset above is what both strips must settle on.
      const bool followFocus = config().input.cursor.followsFocus;
      View* seatTarget =
          followFocus && seatFocus != nullptr && seatFocus->mapped() ? seatFocus : sourceWs->focusedView();
      if (seatTarget == nullptr && seatFocus != nullptr && seatFocus->mapped()) {
        seatTarget = seatFocus;
      }
      if (seatTarget != nullptr) {
        server.focusView(seatTarget, FocusReason::Gesture);
        if (followFocus) {
          finishWorkspaceTransfer(server, *seatTarget);
        }
      }

      sourceWs->markArrange(true);
      targetWs->markArrange(true);

      return true;
    }

    template <wlr_direction D>
    bool actionWorkspaceSwapActiveOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return swapActiveWorkspaceWindows(server, server.outputFromWlr(server.preferredOutput()), target, error);
    }

    template <int Step>
    bool actionWorkspaceSwapActiveOutputCycle(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = cycledOutput(server, Step, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      return swapActiveWorkspaceWindows(server, server.outputFromWlr(server.preferredOutput()), target, error);
    }

    // Overlays
    bool actionOverviewToggle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->toggle();
      return true;
    }

    bool actionOverviewOpen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->open();
      return true;
    }

    bool actionOverviewClose(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->close();
      return true;
    }

    bool actionCheatsheetToggle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->toggle();
      }
      return true;
    }

    bool actionCheatsheetOpen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->show();
      }
      return true;
    }

    bool actionCheatsheetClose(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->hide();
      }
      return true;
    }

    // Scratchpad
    bool actionMoveToScratchpad(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (!name || output == nullptr) {
        return false;
      }
      View* view = placementTarget(server, {}, FocusSource::Workspace).value_or(nullptr);
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && (view == nullptr || scratchpad->moveToScratchpad(view, *name, output));
    }

    bool actionScratchpadToggle(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (!name || output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->toggle(*name, output);
    }

    template <int Direction>
    bool actionScratchpadWindowShowStep(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (!name || output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->showStep(*name, Direction, output);
    }

    bool actionRestoreFromScratchpad(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      if (!name) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->restoreFocused(*name);
    }

    // Restores the focused window if it is the scratchpad's focused entry, otherwise moves it into the scratchpad.
    bool actionToggleScratchpad(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (!name || output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      if (scratchpad == nullptr) {
        return false;
      }
      if (focusedScratchpadWindow(server) != nullptr && scratchpad->hasFocus(*name)) {
        return scratchpad->restoreFocused(*name);
      }
      View* view = placementTarget(server, {}, FocusSource::Workspace).value_or(nullptr);
      return view == nullptr || scratchpad->moveToScratchpad(view, *name, output);
    }

    bool actionScratchpadFocusNext(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      if (!name) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->focusNext(*name);
    }

    template <EffectKind Kind, EffectSlotAction Action>
    bool actionEffect(Server& server, const Keybind& bind, std::string* error) {
      std::string_view name;
      std::string_view target;
      if constexpr (Action == EffectSlotAction::Set || Action == EffectSlotAction::Cycle) {
        if constexpr (Kind == EffectKind::Window || Kind == EffectKind::Border) {
          const auto* arg = payloadIf<EffectWindowArg>(bind);
          if (arg == nullptr) {
            return reject(error, "action carries no window effect selector");
          }
          name = arg->name;
          target = arg->id;
        } else if constexpr (Kind == EffectKind::Screen) {
          const auto* arg = payloadIf<EffectScreenArg>(bind);
          if (arg == nullptr) {
            return reject(error, "action carries no screen effect selector");
          }
          name = arg->name;
          target = arg->output;
        } else {
          const auto* arg = payloadIf<EffectCursorArg>(bind);
          if (arg == nullptr) {
            return reject(error, "action carries no cursor effect selector");
          }
          name = arg->name;
        }
      } else if constexpr (Kind == EffectKind::Window || Kind == EffectKind::Border) {
        const auto* arg = payloadIf<WindowIdArg>(bind);
        if (arg == nullptr) {
          return reject(error, "action carries no window target");
        }
        target = arg->id;
      } else if constexpr (Kind == EffectKind::Screen) {
        const auto* arg = payloadIf<OutputArg>(bind);
        if (arg == nullptr) {
          return reject(error, "action carries no output target");
        }
        target = arg->output;
      } else if (!std::holds_alternative<std::monostate>(bind.payload)) {
        return reject(error, "cursor action does not accept a target");
      }
      EffectSlot* slot = nullptr;
      View* view = nullptr;
      Output* output = nullptr;
      if constexpr (Kind == EffectKind::Window || Kind == EffectKind::Border) {
        const auto resolved = clientTarget(server, target);
        if (!resolved.has_value()) {
          return reject(error, resolved.error());
        }
        view = *resolved;
        if (view == nullptr || !view->mapped()) {
          return reject(error, "no focused window");
        }
        slot = &view->effectSlot(Kind);
      } else if constexpr (Kind == EffectKind::Screen) {
        if (target.empty()) {
          output = server.outputFromWlr(server.preferredOutput());
        } else {
          // Explicit effect targets include disabled outputs, which retain their slots.
          for (const auto& candidate : server.outputs()) {
            if (outputNameMatch(candidate->identity(), target) != OutputNameMatch::None) {
              output = candidate.get();
              break;
            }
          }
        }
        if (output == nullptr) {
          return reject(error, target.empty() ? "no focused output" : "unknown output: " + std::string(target));
        }
        slot = &output->screenEffectSlot();
      } else {
        slot = &server.cursorEffectSlot();
      }
      const EffectSlotActionResult result = server.applyEffectAction(*slot, Kind, Action, name);
      if (result.error) {
        return reject(error, *result.error);
      }
      if (!result.changed) {
        return true;
      }
      // Preparation rebinds and schedules outputs, including static-effect removal.
      server.effects().prepare(server.renderer());
      if (view != nullptr) {
        view->refreshEffectSelection();
      }
      return true;
    }

    constexpr std::array<ActionHandlerFn, static_cast<size_t>(KeybindAction::Count)> kActionHandlers = {
        nullptr,
        &actionSpawn,
        &actionWindowClose,
        &actionWindowKill,
        &actionWindowMinimize,
        &actionWindowRestoreMinimized,
        &actionWorkspaceToggleShowDesktop,
        &actionSessionQuit,
        &actionFocusAdjacent<-1>,
        &actionFocusAdjacent<1>,
        &actionFocusHorizontalOrOutput<-1, WLR_DIRECTION_LEFT>,
        &actionFocusHorizontalOrOutput<1, WLR_DIRECTION_RIGHT>,
        &actionFocusVertical<-1>,
        &actionFocusVertical<1>,
        &actionFocusVerticalOrWorkspace<-1>,
        &actionFocusVerticalOrWorkspace<1>,
        &actionFocusSwitchFloating,
        &actionFocusVerticalOrOutput<-1, WLR_DIRECTION_UP>,
        &actionFocusVerticalOrOutput<1, WLR_DIRECTION_DOWN>,
        &actionMoveColumn<-1>,
        &actionMoveColumn<1>,
        &actionWorkspace,
        &actionColumnMoveToWorkspaceAdjacent<1>,
        &actionColumnMoveToWorkspaceAdjacent<-1>,
        &actionMoveHorizontalOrOutput<-1, WLR_DIRECTION_LEFT>,
        &actionMoveHorizontalOrOutput<1, WLR_DIRECTION_RIGHT>,
        &actionMoveVertical<-1>,
        &actionMoveVertical<1>,
        &actionMoveHorizontal<-1>,
        &actionMoveHorizontal<1>,
        &actionMoveVerticalOrWorkspace<-1>,
        &actionMoveVerticalOrWorkspace<1>,
        &actionMoveVerticalOrOutput<-1, WLR_DIRECTION_UP>,
        &actionMoveVerticalOrOutput<1, WLR_DIRECTION_DOWN>,
        &actionConsume<-1>,
        &actionConsumeOrExpel<-1>,
        &actionConsume<1>,
        &actionConsumeOrExpel<1>,
        &actionConsumeFrom<-1>,
        &actionConsumeFrom<1>,
        &actionCycleWidth<1>,
        &actionCycleWidth<-1>,
        &actionSetWidth,
        &actionToggleMaximize,
        &actionToggleMaximizeToEdges,
        &actionToggleFullscreen,
        &actionToggleFloating,
        &actionTogglePinned,
        &actionFocusCycle<1>,
        &actionWorkspace,
        &actionWorkspace,
        &actionWorkspace,
        &actionWindowMoveToWorkspaceAdjacent<1, false>,
        &actionWindowMoveToWorkspaceAdjacent<-1, false>,
        &actionWindowMoveToWorkspaceAdjacent<1, true>,
        &actionWindowMoveToWorkspaceAdjacent<-1, true>,
        &actionConfigReload,
        &actionCursorHide,
        &actionCursorShow,
        &actionScreenCastClear,
        &actionScreenCastSetOutput,
        &actionScreenCastSetWindow,
        &actionScreenCastFollowWindow,
        &actionScreenCastFollowOutput,
        &actionScreenCastFollowStop,
        &actionKeyboardLayoutNext,
        &actionShortcutsInhibitToggle,
        &actionLayoutScrollDrag,
        &actionLayoutScroll<-1>,
        &actionLayoutScroll<1>,
        &actionLayoutScroll<-1>,
        &actionLayoutScroll<1>,
        &actionOverviewToggle,
        &actionOverviewOpen,
        &actionOverviewClose,
        &actionCheatsheetToggle,
        &actionCheatsheetOpen,
        &actionCheatsheetClose,
        &actionMoveToScratchpad,
        &actionScratchpadToggle,
        &actionRestoreFromScratchpad,
        &actionToggleScratchpad,
        &actionScratchpadFocusNext,
        &actionSubmap,
        &actionWindowFocusId<false>,
        &actionWindowFocusId<true>,
        &actionWorkspaceAdjacent<1>,
        &actionWorkspaceAdjacent<-1>,
        &actionOutputFocus<WLR_DIRECTION_LEFT>,
        &actionOutputFocus<WLR_DIRECTION_RIGHT>,
        &actionOutputFocus<WLR_DIRECTION_UP>,
        &actionOutputFocus<WLR_DIRECTION_DOWN>,
        &actionOutputFocusCycle<1>,
        &actionOutputFocusCycle<-1>,
        &actionWindowMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionWindowMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionWindowMoveToOutput<WLR_DIRECTION_UP>,
        &actionWindowMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionWindowMoveToOutputCycle<1>,
        &actionWindowMoveToOutputCycle<-1>,
        &actionColumnMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionColumnMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionColumnMoveToOutput<WLR_DIRECTION_UP>,
        &actionColumnMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_UP>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionWorkspaceSwapActiveOutput<WLR_DIRECTION_LEFT>,
        &actionWorkspaceSwapActiveOutput<WLR_DIRECTION_RIGHT>,
        &actionWorkspaceSwapActiveOutput<WLR_DIRECTION_UP>,
        &actionWorkspaceSwapActiveOutput<WLR_DIRECTION_DOWN>,
        &actionWorkspaceSwapActiveOutputCycle<1>,
        &actionWorkspaceSwapActiveOutputCycle<-1>,
        &actionModifyWidth,
        &actionWindowCenter,
        &actionWindowMoveTo,
        &actionWindowMoveBy,
        &actionWorkspaceSetLayout,
        &actionDpms<false>,
        &actionDpms<true>,
        &actionOutputEnablement<OutputEnableAction::Disable>,
        &actionOutputEnablement<OutputEnableAction::Enable>,
        &actionOutputEnablement<OutputEnableAction::Toggle>,
        &actionWorkspaceMove<1>,
        &actionWorkspaceMove<-1>,
        &actionColumnCenter,
        &actionFocusFirstColumn,
        &actionFocusLastColumn,
        &actionMoveColumnFirst,
        &actionMoveColumnLast,
        &actionFocusCycle<-1>,
        &actionSwapCycle<1>,
        &actionSwapCycle<-1>,
        &actionSwapDirectional<true, -1>,
        &actionSwapDirectional<true, 1>,
        &actionSwapDirectional<false, -1>,
        &actionSwapDirectional<false, 1>,
        &actionLayoutMasterCountIncrease,
        &actionLayoutMasterCountDecrease,
        &actionSetHeight,
        &actionModifyHeight,
        &actionResizeEdge<WLR_EDGE_LEFT>,
        &actionResizeEdge<WLR_EDGE_RIGHT>,
        &actionResizeEdge<WLR_EDGE_TOP>,
        &actionResizeEdge<WLR_EDGE_BOTTOM>,
        &actionCycleHeight<1>,
        &actionCycleHeight<-1>,
        &actionWindowFocusLast,
        &actionWorkspaceFocusLast,
        &actionEffect<EffectKind::Window, EffectSlotAction::Set>,
        &actionEffect<EffectKind::Window, EffectSlotAction::Cycle>,
        &actionEffect<EffectKind::Window, EffectSlotAction::Toggle>,
        &actionEffect<EffectKind::Window, EffectSlotAction::Reset>,
        &actionEffect<EffectKind::Border, EffectSlotAction::Set>,
        &actionEffect<EffectKind::Border, EffectSlotAction::Cycle>,
        &actionEffect<EffectKind::Border, EffectSlotAction::Toggle>,
        &actionEffect<EffectKind::Border, EffectSlotAction::Reset>,
        &actionEffect<EffectKind::Screen, EffectSlotAction::Set>,
        &actionEffect<EffectKind::Screen, EffectSlotAction::Cycle>,
        &actionEffect<EffectKind::Screen, EffectSlotAction::Toggle>,
        &actionEffect<EffectKind::Screen, EffectSlotAction::Reset>,
        &actionEffect<EffectKind::Cursor, EffectSlotAction::Set>,
        &actionEffect<EffectKind::Cursor, EffectSlotAction::Cycle>,
        &actionEffect<EffectKind::Cursor, EffectSlotAction::Toggle>,
        &actionEffect<EffectKind::Cursor, EffectSlotAction::Reset>,
        &actionColumnToggleTabbed,
        &actionColumnSetDisplay,
        &actionColumnFocusTabStep<1>,
        &actionColumnFocusTabStep<-1>,
        &actionColumnFocusTab,
        &actionColumnMoveTab<1>,
        &actionColumnMoveTab<-1>,
        &actionColumnTabBar<-1>,
        &actionColumnTabBar<1>,
        &actionColumnTabBar<0>,
        &actionScratchpadWindowShowStep<1>,
        &actionScratchpadWindowShowStep<-1>,
        &actionLayoutToggleSplit,
        &actionOutputSdrWhite,
        &actionWindowSnap,
        &actionWindowUnsnap,
    };

    consteval bool everyActionHasHandler() {
      if (kActionHandlers.front() != nullptr) {
        return false;
      }
      return std::ranges::all_of(kActionHandlers.begin() + 1, kActionHandlers.end(), [](ActionHandlerFn handler) {
        return handler != nullptr;
      });
    }

    static_assert(everyActionHasHandler());

  } // namespace

  ActionHandlerFn actionHandlerFor(KeybindAction action) {
    const auto index = static_cast<size_t>(action);
    return index < kActionHandlers.size() ? kActionHandlers[index] : nullptr;
  }

  bool actionRegistryComplete() {
    std::array<bool, kActionHandlers.size()> advertised{};
    for (const ActionSpec& spec : actionSpecs()) {
      const auto index = static_cast<size_t>(spec.action);
      if (index == 0 || index >= advertised.size() || advertised[index] || kActionHandlers[index] == nullptr) {
        return false;
      }
      advertised[index] = true;
    }
    return std::ranges::all_of(advertised.begin() + 1, advertised.end(), [](bool present) { return present; });
  }

} // namespace umbriel
