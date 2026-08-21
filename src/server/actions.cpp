#include "server/actions.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/scrolling.h"
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
#include <utility>

namespace umbriel {

  namespace {

    Workspace* activeWorkspace(Server& server) {
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->active();
    }

    Output* scratchpadOutput(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<OutputArg>(bind);
      if (arg == nullptr || arg->output.empty()) {
        return server.outputFromWlr(server.preferredOutput());
      }
      Output* output = server.outputFromName(arg->output);
      if (output == nullptr && error != nullptr) {
        *error = "unknown output: " + arg->output;
      }
      return output;
    }

    // The action was consumed, but with a message for the caller.
    bool reject(std::string* error, std::string message) {
      if (error != nullptr) {
        *error = std::move(message);
      }
      return true;
    }

    // Resolve `<workspace>` or `<workspace>/<output>` against the current output layout. Qualified selectors address
    // exactly one group; unqualified ones prefer the focused output, then a unique match anywhere.
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
        Workspace* target = group->workspaceForSelector(selector->name);
        if (target == nullptr) {
          return std::unexpected("unknown workspace on output " + selector->output + ": " + selector->name);
        }
        return target;
      }

      Output* preferred = server.outputFromWlr(server.preferredOutput());
      WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;

      // Dynamic numbered workspaces belong to their output. Resolve this first
      // so an existing number elsewhere cannot steal a local request.
      const bool numericSelector = !selector->name.empty()
          && std::ranges::all_of(selector->name, [](char value) { return value >= '0' && value <= '9'; });
      if (preferredGroup != nullptr && preferredGroup->dynamic() && numericSelector) {
        if (Workspace* target = preferredGroup->workspaceForSelector(selector->name)) {
          return target;
        }
      }

      Workspace* target = nullptr;
      bool ambiguous = false;
      for (const auto& output : server.outputs()) {
        WorkspaceGroup* group = output->workspaceGroup();
        Workspace* match = group != nullptr ? group->workspaceNamed(selector->name) : nullptr;
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
        target = preferredGroup != nullptr ? preferredGroup->workspaceForSelector(selector->name) : nullptr;
        if (target == nullptr) {
          return std::unexpected("unknown workspace: " + selector->name);
        }
        return target;
      }

      if (ambiguous) {
        Workspace* preferredMatch =
            preferredGroup != nullptr ? preferredGroup->workspaceNamed(selector->name) : nullptr;
        if (preferredMatch == nullptr) {
          return std::unexpected(
              "ambiguous workspace: " + selector->name + " (qualify it as " + selector->name + "/<output>)"
          );
        }
        return preferredMatch;
      }
      return target;
    }

    // A scratchpad window owns the focus, so window-level toggles are inert.
    bool scratchpadHoldsFocus(Server& server) {
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->hasFocus(server.outputFromWlr(server.preferredOutput()));
    }

    View* focusedWindow(Server& server) {
      if (View* view = View::fromSurface(server.seat()->wlr()->keyboard_state.focused_surface)) {
        return view;
      }
      Workspace* workspace = activeWorkspace(server);
      return workspace != nullptr ? workspace->focusedView() : nullptr;
    }

    // Move `view` to `target` (possibly on another output), activate the target workspace, and focus the view. Floats
    // land proportionally via their remembered usable-area fraction.
    void moveViewToWorkspace(Server& server, View& view, Workspace& target) {
      const bool floating = view.floating();
      if (floating) {
        view.rememberFloatingPosition();
      }
      view.setWorkspace(&target); // layoutAttach self-guards on tiled()
      target.group()->activate(&target);
      if (floating) {
        view.restoreFloatingPosition();
      }
      server.focusView(&view, FocusReason::Directional);
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
      wlr_output* adjacent = wlr_output_layout_adjacent_output(
          server.outputLayout(), direction, reference->wlr(), server.cursor()->wlr()->x, server.cursor()->wlr()->y
      );
      if (adjacent == nullptr || adjacent == reference->wlr()) {
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
      return server.outputFromWlr(adjacent);
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
      server.spawn(arg != nullptr ? arg->command.c_str() : "");
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
      if (ScratchpadManager* scratchpad = server.scratchpadManager()) {
        if (Output* output = server.outputFromWlr(server.preferredOutput())) {
          if (View* view = scratchpad->focused(output)) {
            wlr_xdg_toplevel_send_close(view->toplevel());
            return true;
          }
        }
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* view = workspace->focusedView()) {
          wlr_xdg_toplevel_send_close(view->toplevel());
        }
      }
      return true;
    }

    bool tiledDragActive(Server& server) {
      Cursor* cursor = server.cursor();
      View* moving = cursor != nullptr ? cursor->grabbedView() : nullptr;
      return moving != nullptr && moving->tiled() && cursor->isDraggingView(moving);
    }

    template <int Sign> void scrollActiveLayout(Server& server, int multiplier = 1) {
      Workspace* workspace = activeWorkspace(server);
      ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
      if (scrolling == nullptr || workspace->group()->output() == nullptr) {
        return;
      }
      const auto step = static_cast<double>(config().input.mouse.scrollWheelStep * multiplier);
      const int viewportWidth =
          std::max(1, workspace->group()->output()->usableArea().width - 2 * workspace->layoutConfig().edgePad);
      const auto maxScroll = static_cast<double>(scrolling->maxScroll(viewportWidth));
      scrolling->setScroll(std::clamp(scrolling->scroll() + Sign * step, 0.0, maxScroll));
      workspace->markArrange();
    }

    template <int Direction> bool actionFocusAdjacent(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (Overview* overview = server.overview(); overview != nullptr && overview->interactive()) {
        overview->focusAdjacent(Direction);
        return true;
      }
      if (bind.wheel != WheelDirection::None && tiledDragActive(server)) {
        scrollActiveLayout<Direction>(server, 2);
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* target = workspace->focusAdjacent(Direction)) {
          server.focusView(target, FocusReason::Directional);
        }
      }
      return true;
    }

    template <int Direction> bool actionFocusVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* target = workspace->focusVertical(Direction)) {
          server.focusView(target, FocusReason::Directional);
        }
      }
      return true;
    }

    template <int Direction> bool actionMoveColumn(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->moveFocusedColumn(Direction);
      }
      return true;
    }

    template <int Direction> bool actionMoveVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->moveFocusedVertical(Direction);
      }
      return true;
    }

    bool actionConsumeLeft(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->consumeFocusedLeft();
      }
      return true;
    }

    bool actionExpelRight(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->expelFocusedRight();
      }
      return true;
    }

    bool actionCycleWidth(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->cycleFocusedWidth();
      }
      return true;
    }

    bool actionSetWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->setFocusedWidth(arg->fraction);
        }
      }
      return true;
    }

    bool actionModifyWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (const auto* arg = payloadIf<WidthArg>(bind)) {
          workspace->modifyFocusedWidth(arg->fraction);
        }
      }
      return true;
    }

    bool actionToggleMaximize(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFullWidth();
      }
      return true;
    }

    bool actionToggleMaximizeToEdges(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedMaximizedToEdges();
      }
      return true;
    }

    bool actionToggleFullscreen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
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

    bool actionFocusNext(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.focusNextWindow();
      return true;
    }

    bool actionWindowFocusId(Server& server, const Keybind& bind, std::string* error) {
      const auto* arg = payloadIf<WindowIdArg>(bind);
      if (arg == nullptr || arg->id.empty()) {
        if (error != nullptr) {
          *error = "window-focus requires a window id";
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
      server.focusView(view, FocusReason::ForeignActivation);
      return true;
    }

    // Workspaces
    bool actionWorkspace(Server& server, const Keybind& bind, std::string* error) {
      const std::expected<Workspace*, std::string> target = resolveWorkspaceSelector(server, bind);
      if (!target.has_value()) {
        return reject(error, target.error());
      }

      WorkspaceGroup* group = (*target)->group();
      if (bind.action == KeybindAction::WindowMoveToWorkspace) {
        for (const auto& entry : server.views()) {
          if (entry->mapped() && entry->onActiveWorkspace()) {
            moveViewToWorkspace(server, *entry, **target);
            return true;
          }
        }
      }
      group->select(*target);
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

    bool actionWorkspaceSetLayout(Server& server, const Keybind& bind, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr) {
        return true;
      }
      const auto* arg = payloadIf<LayoutModeArg>(bind);
      if (arg == nullptr) {
        return true;
      }
      const LayoutMode desired = arg->mode.value_or(
          workspace->layoutMode() == LayoutMode::Scrolling ? LayoutMode::Dwindle : LayoutMode::Scrolling
      );
      if (desired == workspace->layoutMode()) {
        return true;
      }
      workspace->overrideLayoutMode(desired);
      // The layout behind an interactive tiled resize is being replaced; drop
      // the stale session, same as the config-reload layout swap.
      server.cursor()->cancelStaleTiledResize();
      return true;
    }

    template <int Sign> bool actionLayoutScroll(Server& server, const Keybind& bind, std::string* /*error*/) {
      const int multiplier = bind.wheel != WheelDirection::None && tiledDragActive(server) ? 2 : 1;
      scrollActiveLayout<Sign>(server, multiplier);
      return true;
    }

    // Outputs
    template <wlr_direction D> bool actionOutputFocus(Server& server, const Keybind& /*bind*/, std::string* error) {
      std::string message;
      Output* target = adjacentOutput(server, D, &message);
      if (target == nullptr) {
        return reject(error, std::move(message));
      }
      warpToOutputCenter(server, *target);
      server.refocus(target);
      return true;
    }

    template <wlr_direction D>
    bool actionWindowMoveToOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
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
      Workspace* source = activeWorkspace(server);
      View* view = source != nullptr ? source->focusedView() : nullptr;
      if (view == nullptr) {
        return true; // nothing focused: silent no-op
      }
      moveViewToWorkspace(server, *view, *destination);
      warpToOutputCenter(server, *target);
      return true;
    }

    template <wlr_direction D>
    bool actionColumnMoveToOutput(Server& server, const Keybind& /*bind*/, std::string* error) {
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
      Workspace* source = activeWorkspace(server);
      View* focused = source != nullptr ? source->focusedView() : nullptr;
      if (focused == nullptr) {
        return true; // nothing focused: silent no-op
      }
      const int column = source->layout().columnOf(focused);
      if (column < 0) {
        // Floating focus: behave exactly like the window move.
        moveViewToWorkspace(server, *focused, *destination);
        warpToOutputCenter(server, *target);
        return true;
      }

      // Snapshot the column before mutating: setWorkspace rebuilds the source
      // layout on every view that leaves.
      const std::vector<View*> columnViews = source->layout().columns()[static_cast<size_t>(column)].views;
      const double width = source->layout().columns()[static_cast<size_t>(column)].widthFrac;

      View* first = columnViews.front();
      first->setWorkspace(destination, /*attachToLayout=*/false);
      destination->layout().insertView(first, static_cast<int>(destination->layout().columns().size()));
      if (destination->scrollingLayout() != nullptr) {
        destination->layout().setWidthFraction(destination->layout().columnOf(first), width);
      }
      for (size_t i = 1; i < columnViews.size(); ++i) {
        View* view = columnViews[i];
        view->setWorkspace(destination, /*attachToLayout=*/false);
        destination->layout().insertViewIntoColumn(view, destination->layout().columnOf(first), static_cast<int>(i));
      }
      destination->markArrange();
      destination->group()->activate(destination);
      server.focusView(focused, FocusReason::Directional);
      warpToOutputCenter(server, *target);
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
      Workspace* destination = targetGroup->createWorkspace(source->name().c_str());
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
        first->setWorkspace(destination, /*attachToLayout=*/false);
        destination->layout().insertView(first, static_cast<int>(destination->layout().columns().size()));
        if (destination->scrollingLayout() != nullptr) {
          destination->layout().setWidthFraction(destination->layout().columnOf(first), column.widthFrac);
        }
        for (size_t i = 1; i < column.views.size(); ++i) {
          View* view = column.views[i];
          view->setWorkspace(destination, /*attachToLayout=*/false);
          destination->layout().insertViewIntoColumn(view, destination->layout().columnOf(first), static_cast<int>(i));
        }
      }
      for (View* view : floats) {
        view->rememberFloatingPosition();
        view->setWorkspace(destination);
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
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      Workspace* workspace = activeWorkspace(server);
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr
          && workspace != nullptr
          && scratchpad->moveToScratchpad(workspace->focusedView(), output);
    }

    bool actionScratchpadToggle(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->toggle(output);
    }

    bool actionRestoreFromScratchpad(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->restoreFocused(output);
    }

    bool actionScratchpadFocusNext(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->focusNext(output);
    }

    constexpr std::array<ActionHandlerFn, static_cast<size_t>(KeybindAction::Count)> kActionHandlers = {
        nullptr,
        &actionSpawn,
        &actionWindowClose,
        &actionSessionQuit,
        &actionFocusAdjacent<-1>,
        &actionFocusAdjacent<1>,
        &actionFocusVertical<-1>,
        &actionFocusVertical<1>,
        &actionMoveColumn<-1>,
        &actionMoveColumn<1>,
        &actionMoveVertical<-1>,
        &actionMoveVertical<1>,
        &actionConsumeLeft,
        &actionExpelRight,
        &actionCycleWidth,
        &actionSetWidth,
        &actionToggleMaximize,
        &actionToggleMaximizeToEdges,
        &actionToggleFullscreen,
        &actionToggleFloating,
        &actionTogglePinned,
        &actionFocusNext,
        &actionWorkspace,
        &actionWorkspace,
        &actionConfigReload,
        &actionKeyboardLayoutNext,
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
        &actionScratchpadFocusNext,
        &actionSubmap,
        &actionWindowFocusId,
        &actionWorkspaceAdjacent<1>,
        &actionWorkspaceAdjacent<-1>,
        &actionOutputFocus<WLR_DIRECTION_LEFT>,
        &actionOutputFocus<WLR_DIRECTION_RIGHT>,
        &actionOutputFocus<WLR_DIRECTION_UP>,
        &actionOutputFocus<WLR_DIRECTION_DOWN>,
        &actionWindowMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionWindowMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionWindowMoveToOutput<WLR_DIRECTION_UP>,
        &actionWindowMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionColumnMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionColumnMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionColumnMoveToOutput<WLR_DIRECTION_UP>,
        &actionColumnMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_LEFT>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_RIGHT>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_UP>,
        &actionWorkspaceMoveToOutput<WLR_DIRECTION_DOWN>,
        &actionModifyWidth,
        &actionWindowCenter,
        &actionWorkspaceSetLayout,
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
