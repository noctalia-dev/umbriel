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
#include <expected>
#include <optional>
#include <string_view>
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

    std::string workspaceNameToken(std::string_view name) {
      const bool needsQuotes =
          !name.empty() && std::ranges::all_of(name, [](char value) { return value >= '0' && value <= '9'; });
      return needsQuotes ? "\"" + std::string(name) + "\"" : std::string(name);
    }

    // Resolve a typed workspace reference against the current output layout. Indices stay on the preferred output;
    // names resolve globally, using that output only to disambiguate duplicates. An output qualifier confines either
    // kind to exactly one group.
    std::expected<Workspace*, std::string> resolveWorkspaceSelector(Server& server, const Keybind& bind) {
      const auto* selector = payloadIf<WorkspaceArg>(bind);
      if (selector == nullptr) {
        return std::unexpected(std::string("action carries no workspace selector"));
      }
      if (!selector->output.empty()) {
        Output* output = server.outputFromName(selector->output);
        if (output == nullptr) {
          return std::unexpected("unknown output: " + selector->output);
        }
        WorkspaceGroup* group = output->workspaceGroup();
        if (group == nullptr) {
          return std::unexpected("output has no workspace group: " + selector->output);
        }
        Workspace* target = nullptr;
        std::string reference;
        if (const auto* index = std::get_if<WorkspaceIndex>(&selector->reference)) {
          reference = std::to_string(index->value);
          if (index->value > 0) {
            target = group->workspaceAtClamped(index->value - 1);
          }
        } else if (const auto* name = std::get_if<WorkspaceName>(&selector->reference)) {
          reference = name->value;
          target = group->workspaceNamed(name->value);
        }
        if (target == nullptr) {
          return std::unexpected("unknown workspace on output " + selector->output + ": " + reference);
        }
        return target;
      }

      Output* preferred = server.outputFromWlr(server.preferredOutput());
      WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;

      if (const auto* index = std::get_if<WorkspaceIndex>(&selector->reference)) {
        Workspace* target = index->value > 0 && preferredGroup != nullptr
            ? preferredGroup->workspaceAtClamped(index->value - 1)
            : nullptr;
        if (target == nullptr) {
          return std::unexpected("unknown workspace position: " + std::to_string(index->value));
        }
        return target;
      }

      const auto* name = std::get_if<WorkspaceName>(&selector->reference);
      if (name == nullptr) {
        return std::unexpected(std::string("action carries an invalid workspace selector"));
      }

      Workspace* target = nullptr;
      bool ambiguous = false;
      for (const auto& output : server.outputs()) {
        WorkspaceGroup* group = output->workspaceGroup();
        Workspace* match = group != nullptr ? group->workspaceNamed(name->value) : nullptr;
        if (match == nullptr) {
          continue;
        }
        if (target != nullptr) {
          ambiguous = true;
        } else {
          target = match;
        }
      }

      if (target == nullptr) {
        return std::unexpected("unknown workspace: " + name->value);
      }

      if (ambiguous) {
        Workspace* preferredMatch = preferredGroup != nullptr ? preferredGroup->workspaceNamed(name->value) : nullptr;
        if (preferredMatch == nullptr) {
          const std::string token = workspaceNameToken(name->value);
          return std::unexpected("ambiguous workspace: " + name->value + " (qualify it as " + token + "/<output>)");
        }
        return preferredMatch;
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

    bool scratchpadHoldsFocus(Server& server) { return focusedScratchpadWindow(server) != nullptr; }

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
      Workspace* workspace = activeWorkspace(server);
      return workspace != nullptr ? workspace->focusedView() : nullptr;
    }

    const std::vector<double>& floatingPresetsFor(View& view) {
      if (Output* output = view.currentOutput(); output != nullptr && output->workspaceGroup() != nullptr) {
        if (Workspace* workspace = output->workspaceGroup()->active()) {
          return workspace->layoutConfig().widthPresets;
        }
      }
      return config().layout.widthPresets;
    }

    template <int Direction> void cycleScratchpadSize(View& view, bool width) {
      const auto axis = view.floatingAxisBasis(width);
      if (!axis) {
        return;
      }
      const auto& presets = floatingPresetsFor(view);
      const double current = presetSnappedFraction(presets, (*axis)[0], (*axis)[1]);
      const double next = nextFractionPreset(presets, current, Direction);
      view.resizeFloatingFractions(
          width ? std::optional(next) : std::nullopt, width ? std::nullopt : std::optional(next)
      );
    }

    // Move `view` to `target` (possibly on another output), activate the target workspace, and focus the view. Floats
    // land proportionally via their remembered usable-area fraction.
    void moveViewToWorkspace(Server& server, View& view, Workspace& target) {
      Workspace* source = view.workspace();
      const bool workspaceChanged = source != &target;
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
      view.moveToWorkspace(&target); // layoutAttach self-guards on tiled()
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
      target.group()->activate(&target);
      if (floating) {
        view.restoreFloatingPosition();
      }
      server.focusView(&view, FocusReason::Directional);
      if (workspaceChanged) {
        finishWorkspaceTransfer(server, view);
      }
    }

    // Move the focused tiled column as one structural unit. Rebuild it only
    // after snapshotting because every view transfer mutates the source layout.
    // A floating focus has no column, so it follows the single-window behavior
    // used by the directional output actions.
    bool moveFocusedColumnToWorkspace(Server& server, Workspace& source, Workspace& target) {
      if (&source == &target) {
        return false;
      }
      View* focused = source.focusedView();
      if (focused == nullptr) {
        return false;
      }
      const int columnIndex = source.layout().columnOf(focused);
      const auto& sourceColumns = source.layout().columns();
      if (columnIndex < 0 || columnIndex >= static_cast<int>(sourceColumns.size())) {
        moveViewToWorkspace(server, *focused, target);
        return true;
      }

      const Column column = sourceColumns[static_cast<size_t>(columnIndex)];
      if (column.views.empty()) {
        return false;
      }

      const int focusedTargetColumn = target.layout().columnOf(target.focusedView());
      const int targetIndex =
          focusedTargetColumn >= 0 ? focusedTargetColumn + 1 : static_cast<int>(target.layout().columns().size());
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
        for (size_t row = 0; row < column.heightWeights.size(); ++row) {
          scrolling->setHeightWeight(targetColumn, static_cast<int>(row), column.heightWeights[row]);
        }
        scrolling->setTopGapWeight(targetColumn, column.topGapWeight);
        scrolling->setBottomGapWeight(targetColumn, column.bottomGapWeight);
      }

      target.markArrange();
      target.group()->activate(&target);
      server.focusView(focused, FocusReason::Directional);
      finishWorkspaceTransfer(server, *focused);
      return true;
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
        confirm->show();
        return true;
      }
      server.stop();
      return true;
    }

    bool actionConfigReload(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.handleConfigReload();
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
        if (!output->configuredEnabled()) {
          if (!requested.empty()) {
            return reject(error, "output is disabled by config: " + requested);
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
        return reject(error, "no configured outputs");
      }
      return true;
    }

    bool actionKeyboardLayoutNext(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      return server.cycleKeyboardLayout();
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

    // Window IDs are ext-foreign-toplevel identifiers, the same strings
    // clients receive from the protocol and the IPC surface reuses.
    View* viewByForeignIdentifier(Server& server, std::string_view id) {
      for (const auto& view : server.views()) {
        if (!view->mapped()) {
          continue;
        }
        const char* identifier = view->extForeignIdentifier();
        if (identifier != nullptr && id == identifier) {
          return view.get();
        }
      }
      return nullptr;
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
      if (const auto* arg = payloadIf<WindowIdArg>(bind); arg != nullptr && !arg->id.empty()) {
        View* view = viewByForeignIdentifier(server, arg->id);
        if (view == nullptr) {
          if (error != nullptr) {
            *error = "unknown window: " + arg->id;
          }
          return false;
        }
        wlr_xdg_toplevel_send_close(view->toplevel());
        return true;
      }
      if (View* view = focusedWindow(server)) {
        wlr_xdg_toplevel_send_close(view->toplevel());
      }
      return true;
    }

    bool tiledDragActive(Server& server) {
      const Cursor* cursor = server.cursor();
      return cursor != nullptr && cursor->isDraggingIntoLayout();
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
      scrolling->setScroll(std::clamp(scrolling->scroll() + Sign * step, 0.0, maxScroll));
      workspace->markArrange();
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
        workspace->moveFocusedColumn(Direction);
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
        if (workspace->moveFocusedColumn(Direction)) {
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

    template <int Direction>
    bool actionMoveVerticalOrWorkspace(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (!workspace->moveFocusedVertical(Direction)) {
          Workspace* source = workspace;
          if (source->group() == nullptr) {
            return true;
          }
          WorkspaceGroup* group = source->group();
          const size_t index = source->index();
          if (Direction < 0 && index == 0) {
            return true;
          }
          Workspace* target = group->workspaceAt(index + static_cast<size_t>(Direction));
          if (target == nullptr || target == source) {
            return true;
          }
          if (View* view = source->focusedView()) {
            moveViewToWorkspace(server, *view, *target);
          }
        }
      }
      return true;
    }

    template <int Direction> bool actionConsume(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        workspace->consumeFocused(Direction);
      }
      return true;
    }

    template <int Direction>
    bool actionConsumeOrExpel(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = windowActionWorkspace(server)) {
        if (!workspace->expelFocused(Direction)) {
          workspace->consumeFocused(Direction);
        }
      }
      return true;
    }

    template <int Direction> bool actionCycleWidth(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        cycleScratchpadSize<Direction>(*view, true);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->cycleFocusedWidth(Direction);
      }
      return true;
    }

    template <int Direction> bool actionCycleHeight(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        cycleScratchpadSize<Direction>(*view, false);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->cycleFocusedHeight(Direction);
      }
      return true;
    }

    bool actionSetWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          view->resizeFloatingFractions(std::clamp(arg->fraction, 0.1, 1.0), std::nullopt);
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->setFocusedWidth(arg->fraction);
        }
      }
      return true;
    }

    bool actionModifyWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind); arg != nullptr) {
          if (const auto current = view->floatingFraction(true)) {
            view->resizeFloatingFractions(std::clamp(*current + arg->fraction, 0.1, 1.0), std::nullopt);
          }
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->modifyFocusedWidth(arg->fraction);
        }
      }
      return true;
    }

    bool actionSetHeight(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          view->resizeFloatingFractions(std::nullopt, std::clamp(arg->fraction, 0.1, 1.0));
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->setFocusedHeight(arg->fraction);
        }
      }
      return true;
    }

    bool actionModifyHeight(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind); arg != nullptr) {
          if (const auto current = view->floatingFraction(false)) {
            view->resizeFloatingFractions(std::nullopt, std::clamp(*current + arg->fraction, 0.1, 1.0));
          }
        }
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->modifyFocusedHeight(arg->fraction);
        }
      }
      return true;
    }

    bool actionToggleMaximize(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (View* view = focusedScratchpadWindow(server)) {
        if (view->maximizedToEdges()) {
          view->setMaximizedToEdges(false);
        }
        if (!view->toplevel()->scheduled.fullscreen) {
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
      if (View* view = focusedScratchpadWindow(server)) {
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
      if (View* view = focusedScratchpadWindow(server)) {
        view->toggleFullscreen();
        server.scratchpadManager()->restorePresentation(view);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFullscreen();
      }
      return true;
    }

    bool actionToggleFloating(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFloating();
      }
      return true;
    }

    bool actionTogglePinned(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (View* view = focusedWindow(server)) {
        view->togglePinned();
      }
      return true;
    }

    bool actionWindowCenter(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* view = workspace->focusedView()) {
          view->centerFloating();
        }
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

    template <bool Warp> bool actionWindowFocusId(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<WindowIdArg>(bind);
      if (arg == nullptr || arg->id.empty()) {
        if (error != nullptr) {
          *error = Warp ? "window-focus-warp requires a window id" : "window-focus requires a window id";
        }
        return false;
      }
      View* view = viewByForeignIdentifier(server, arg->id);
      if (view == nullptr) {
        if (error != nullptr) {
          *error = "unknown window: " + arg->id;
        }
        return false;
      }
      if (ScratchpadManager* scratchpad = server.scratchpadManager();
          scratchpad != nullptr && scratchpad->contains(view) && !view->onActiveWorkspace()) {
        // Hidden scratchpad entries fail FocusManager's visibility gate. Summon
        // their pad using the normal pointer-output policy, then select the
        // exact requested entry below without briefly focusing its remembered
        // window first.
        if (Output* output = server.outputFromWlr(server.preferredOutput()); output != nullptr) {
          scratchpad->summon(scratchpad->nameFor(view), output);
        }
      }
      server.focusView(view, FocusReason::ForeignActivation);
      if constexpr (Warp) {
        warpCursorToWindow(server, *view);
      }
      return true;
    }

    bool actionWindowFocusLast(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      View* current = focusedWindow(server);
      for (const auto& entry : server.registry().all()) {
        View* target = entry.get();
        if (target == current || !target->mapped() || target->workspace() == nullptr) {
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
      View* focused = workspace->focusedView();
      const bool seekFloating = focused == nullptr || !focused->floating();
      View* target = nullptr;
      for (const auto& entry : server.registry().all()) {
        View* view = entry.get();
        if (view == focused || !view->mapped() || view->workspace() != workspace) {
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
      if (bind.action == KeybindAction::WindowMoveToWorkspace) {
        if (scratchpadHoldsFocus(server)) {
          return true;
        }
        for (const auto& entry : server.views()) {
          if (entry->mapped() && entry->onActiveWorkspace()) {
            moveViewToWorkspace(server, *entry, **target);
            return true;
          }
        }
      }
      if (bind.action == KeybindAction::ColumnMoveToWorkspace) {
        if (Workspace* source = windowActionWorkspace(server)) {
          moveFocusedColumnToWorkspace(server, *source, **target);
        }
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
      const size_t index = group->active()->index();
      if (Direction < 0 && index == 0) {
        return true; // no wrap-around; silent no-op at the first workspace
      }
      Workspace* target = group->workspaceAt(index + static_cast<size_t>(Direction));
      if (target == nullptr || target == group->active()) {
        return true;
      }
      group->select(target);
      return true;
    }

    template <int Direction>
    bool actionWindowMoveToWorkspaceAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = windowActionWorkspace(server);
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      WorkspaceGroup* group = workspace->group();
      const size_t index = workspace->index();
      if (Direction < 0 && index == 0) {
        return true;
      }
      Workspace* target = group->workspaceAt(index + static_cast<size_t>(Direction));
      if (target == nullptr || target == workspace) {
        return true;
      }
      if (View* view = workspace->focusedView()) {
        moveViewToWorkspace(server, *view, *target);
      }
      return true;
    }

    template <int Direction>
    bool actionColumnMoveToWorkspaceAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* source = windowActionWorkspace(server);
      if (source == nullptr || source->group() == nullptr) {
        return true;
      }
      WorkspaceGroup* group = source->group();
      const size_t index = source->index();
      if (Direction < 0 && index == 0) {
        return true;
      }
      Workspace* target = group->workspaceAt(index + static_cast<size_t>(Direction));
      if (target == nullptr || target == source) {
        return true;
      }
      moveFocusedColumnToWorkspace(server, *source, *target);
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
      const auto nextMode = [](LayoutMode mode) {
        switch (mode) {
        case LayoutMode::Scrolling:
          return LayoutMode::Dwindle;
        case LayoutMode::Dwindle:
          return LayoutMode::Master;
        case LayoutMode::Master:
          return LayoutMode::Scrolling;
        }
        return LayoutMode::Scrolling;
      };
      const LayoutMode desired = arg->mode.value_or(nextMode(workspace->layoutMode()));
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
      server.refocus(&target);
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
      Workspace* source = windowActionWorkspace(server);
      View* view = source != nullptr ? source->focusedView() : nullptr;
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
      Workspace* source = windowActionWorkspace(server);
      if (source == nullptr || !moveFocusedColumnToWorkspace(server, *source, *destination)) {
        return true; // nothing focused: silent no-op
      }
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
      struct ColumnSnapshot {
        std::vector<View*> views;
        double widthFrac = 0.5;
      };
      std::vector<ColumnSnapshot> columns;
      for (const Column& column : source->layout().columns()) {
        columns.push_back({column.views, column.widthFrac});
      }
      std::vector<View*> floats;
      for (View* view : source->allViews()) {
        if (view->floating() && !view->pinned()) {
          floats.push_back(view);
        }
      }

      for (const ColumnSnapshot& column : columns) {
        if (column.views.empty()) {
          continue;
        }
        View* first = column.views.front();
        first->moveToWorkspace(destination, /*attachToLayout=*/false);
        destination->layout().insertView(first, static_cast<int>(destination->layout().columns().size()));
        if (destination->scrollingLayout() != nullptr) {
          destination->layout().setWidthFraction(destination->layout().columnOf(first), column.widthFrac);
        }
        for (size_t i = 1; i < column.views.size(); ++i) {
          View* view = column.views[i];
          view->moveToWorkspace(destination, /*attachToLayout=*/false);
          destination->layout().insertViewIntoColumn(view, destination->layout().columnOf(first), static_cast<int>(i));
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
        server.refocus(target);
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
        if (tiles.snapshot == nullptr || !dest->layout().restoreState(*tiles.snapshot, tiles.members)) {
          for (const LayoutMember& member : tiles.members) {
            if (member.view != nullptr) {
              dest->layout().insertView(member.view, static_cast<int>(dest->layout().columns().size()));
            }
          }
        }
        for (View* view : floats) {
          view->restoreFloatingPosition();
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

      // Gesture keeps the seat focus where it is without revealing its column: the restored scroll offset above is
      // what both strips must settle on.
      if (seatFocus != nullptr && seatFocus->mapped()) {
        server.focusView(seatFocus, FocusReason::Gesture);
        maybeWarpCursorToWindow(server, seatFocus);
      } else if (sourceWs->focusedView() != nullptr) {
        server.focusView(sourceWs->focusedView(), FocusReason::Gesture);
        maybeWarpCursorToWindow(server, sourceWs->focusedView());
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
      Workspace* workspace = activeWorkspace(server);
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr
          && workspace != nullptr
          && scratchpad->moveToScratchpad(workspace->focusedView(), *name, output);
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

    bool actionRestoreFromScratchpad(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      if (!name) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->restoreFocused(*name);
    }

    // Toggles the focused window's scratchpad membership: if the focused
    // window is currently the scratchpad's focused entry, restore it (same as
    // actionRestoreFromScratchpad); otherwise move it into the scratchpad
    // (same as actionMoveToScratchpad).
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
      Workspace* workspace = activeWorkspace(server);
      return workspace != nullptr && scratchpad->moveToScratchpad(workspace->focusedView(), *name, output);
    }

    bool actionScratchpadFocusNext(Server& server, const Keybind& bind, std::string* error) {
      const auto name = scratchpadName(server, bind, error);
      if (!name) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->focusNext(*name);
    }

    constexpr std::array<ActionHandlerFn, static_cast<size_t>(KeybindAction::Count)> kActionHandlers = {
        nullptr,
        &actionSpawn,
        &actionWindowClose,
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
        &actionMoveVerticalOrWorkspace<-1>,
        &actionMoveVerticalOrWorkspace<1>,
        &actionMoveVerticalOrOutput<-1, WLR_DIRECTION_UP>,
        &actionMoveVerticalOrOutput<1, WLR_DIRECTION_DOWN>,
        &actionConsume<-1>,
        &actionConsumeOrExpel<-1>,
        &actionConsume<1>,
        &actionConsumeOrExpel<1>,
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
        &actionWindowMoveToWorkspaceAdjacent<1>,
        &actionWindowMoveToWorkspaceAdjacent<-1>,
        &actionConfigReload,
        &actionKeyboardLayoutNext,
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
        &actionWorkspaceSetLayout,
        &actionDpms<false>,
        &actionDpms<true>,
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
        &actionLayoutMasterCountIncrease,
        &actionLayoutMasterCountDecrease,
        &actionSetHeight,
        &actionModifyHeight,
        &actionCycleHeight<1>,
        &actionCycleHeight<-1>,
        &actionWindowFocusLast,
        &actionWorkspaceFocusLast,
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
