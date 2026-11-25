#include "workspace/scratchpad.h"

#include "config/config.h"
#include "config/resolve.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <utility>

namespace umbriel {

  namespace {
    constexpr std::string_view kImplicitScratchpad = "default";
    constexpr std::chrono::seconds kSpawnTimeout{10};
    // How far a dialog's parent chain is followed. The bound keeps a parent cycle from looping.
    constexpr int kMaxParentDepth = 8;
    // Presented opacity at or below which a fading window counts as hidden.
    constexpr float kFadedOutOpacity = 0.002F;

    wlr_box usableArea(Server& server, Output* output) {
      if (output == nullptr) {
        return {};
      }
      wlr_box area = output->usableArea();
      if (area.width <= 0 || area.height <= 0) {
        wlr_output_layout_get_box(server.outputLayout(), output->wlr(), &area);
      }
      return area;
    }

    bool sameBox(const wlr_box& first, const wlr_box& second) {
      return first.x == second.x && first.y == second.y && first.width == second.width && first.height == second.height;
    }

    // Opening the overview hides every scratchpad, and nothing shows one again until it closes.
    bool overviewOpen(Server& server) {
      const Overview* overview = server.overview();
      return overview != nullptr && overview->active();
    }

    // A pad's `[[scratchpad]]` definition, or null when it has none, as the implicit pad does.
    const ScratchpadConfig* definitionFor(std::string_view name) {
      const auto definition = std::ranges::find(config().scratchpads, name, &ScratchpadConfig::name);
      return definition != config().scratchpads.end() ? &*definition : nullptr;
    }

    // The step `offset` places from `view`, wrapping at the ends; counted from the first step when `view` is not one.
    View* stepFrom(const std::vector<View*>& steps, const View* view, int offset) {
      const auto count = static_cast<std::ptrdiff_t>(steps.size());
      const auto current = std::ranges::find(steps, view);
      const std::ptrdiff_t index = current == steps.end() ? 0 : current - steps.begin();
      return steps[static_cast<size_t>((index + offset + count) % count)];
    }
  } // namespace

  ScratchpadManager::ScratchpadManager(Server& server, wlr_scene_tree* root) : m_server(&server), m_root(root) {
    m_server->registerAnimatable(this);
    if (config().scratchpads.empty()) {
      m_scratchpads.try_emplace(std::string(kImplicitScratchpad));
    } else {
      for (const ScratchpadConfig& scratchpad : config().scratchpads) {
        m_scratchpads.try_emplace(scratchpad.name);
      }
    }
  }

  ScratchpadManager::~ScratchpadManager() {
    m_server->unregisterAnimatable(this);
    for (auto& [output, rect] : m_dimRects) {
      wlr_scene_node_destroy(&rect->node);
    }
    for (auto& [output, blur] : m_blurNodes) {
      wlr_scene_node_destroy(&blur->node);
    }
  }

  bool ScratchpadManager::tickAnimations(double nowMsec) {
    bool movedBackdrop = false;
    for (auto& [output, fade] : m_backdropFades) {
      if (fade.tick(nowMsec)) {
        updateDimAndBlur(output);
        if (const auto rect = m_dimRects.find(output); rect != m_dimRects.end()) {
          bindAnimationEffect(&rect->second->node, AnimationEvent::Scratchpad, fade);
        }
        if (const auto blur = m_blurNodes.find(output); blur != m_blurNodes.end()) {
          bindAnimationEffect(&blur->second->node, AnimationEvent::Scratchpad, fade);
        }
        movedBackdrop = true;
      }
    }

    if (m_hidingViews.empty()) {
      return movedBackdrop;
    }
    std::erase_if(m_hidingViews, [](View* view) {
      if (view->presentedOpacity() > kFadedOutOpacity) {
        return false;
      }
      view->setNodeEnabled(false);
      return true;
    });
    return movedBackdrop || !m_hidingViews.empty();
  }

  bool ScratchpadManager::hasActiveAnimations() const {
    return !m_hidingViews.empty()
        || std::ranges::any_of(m_backdropFades, [](const auto& entry) { return entry.second.animating(); });
  }

  bool ScratchpadManager::animatesOn(const Output* output) const {
    const auto fade =
        std::ranges::find_if(m_backdropFades, [output](const auto& entry) { return entry.first == output; });
    if (fade != m_backdropFades.end() && fade->second.animating()) {
      return true;
    }
    return std::ranges::any_of(m_hidingViews, [this, output](const View* view) { return outputFor(view) == output; });
  }

  ScratchpadManager::Scratchpad* ScratchpadManager::findScratchpad(std::string_view name) {
    const auto scratchpad = m_scratchpads.find(name);
    return scratchpad != m_scratchpads.end() ? &scratchpad->second : nullptr;
  }

  const ScratchpadManager::Scratchpad* ScratchpadManager::findScratchpad(std::string_view name) const {
    const auto scratchpad = m_scratchpads.find(name);
    return scratchpad != m_scratchpads.end() ? &scratchpad->second : nullptr;
  }

  ScratchpadManager::Entry* ScratchpadManager::findEntry(const View* view) {
    const auto entry =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    return entry != m_entries.end() ? &*entry : nullptr;
  }

  const ScratchpadManager::Entry* ScratchpadManager::findEntry(const View* view) const {
    const auto entry =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    return entry != m_entries.end() ? &*entry : nullptr;
  }

  bool ScratchpadManager::hasEntries(std::string_view name) const {
    return std::ranges::any_of(m_entries, [name](const Entry& entry) { return entry.scratchpad == name; });
  }

  bool ScratchpadManager::modalOn(const Output* output) const {
    return std::ranges::any_of(m_scratchpads, [output](const auto& entry) {
      const ScratchpadConfig* definition = definitionFor(entry.first);
      return entry.second.visible && entry.second.output == output && (definition == nullptr || definition->modal);
    });
  }

  bool ScratchpadManager::stickyOn(const Output* output) const {
    return std::ranges::any_of(m_scratchpads, [output](const auto& entry) {
      const ScratchpadConfig* definition = definitionFor(entry.first);
      return entry.second.visible && entry.second.output == output && definition != nullptr && definition->sticky;
    });
  }

  bool ScratchpadManager::presents(const Scratchpad& scratchpad, const View* view) {
    if (!scratchpad.visible || scratchpad.output == nullptr) {
      return false;
    }
    if (scratchpad.solo == nullptr) {
      return true;
    }
    // A dialog shows with the window it belongs to.
    const View* candidate = view;
    for (int depth = 0; depth < kMaxParentDepth && candidate != nullptr && candidate != scratchpad.solo; ++depth) {
      candidate = candidate->shellParent();
    }
    return candidate == scratchpad.solo;
  }

  bool ScratchpadManager::ridesWithParent(const View* view, std::string_view name) const {
    // An attached dialog belongs to no scratchpad of its own: it shows inside its parent.
    const View* parent = view != nullptr && !view->attached() ? view->shellParent() : nullptr;
    const Entry* entry = parent != nullptr ? findEntry(parent) : nullptr;
    return entry != nullptr && entry->scratchpad == name;
  }

  View* ScratchpadManager::memberFor(View* view, std::string_view name) const {
    for (int depth = 0; depth < kMaxParentDepth && ridesWithParent(view, name); ++depth) {
      view = view->shellParent();
    }
    return view;
  }

  bool ScratchpadManager::isMember(const Entry& entry, std::string_view name) const {
    return entry.scratchpad == name
        && entry.view != nullptr
        && entry.view->mapped()
        && !ridesWithParent(entry.view, name);
  }

  std::vector<View*> ScratchpadManager::members(std::string_view name) const {
    std::vector<View*> views;
    for (const Entry& entry : m_entries) {
      if (isMember(entry, name)) {
        views.push_back(entry.view);
      }
    }
    return views;
  }

  View* ScratchpadManager::memberBeside(std::string_view name, const View* view, int direction) const {
    // Search from the entry itself, so a window that is closing still has its place.
    const auto position = std::ranges::find(m_entries, view, &Entry::view);
    if (position == m_entries.end()) {
      return nullptr;
    }
    const auto count = static_cast<std::ptrdiff_t>(m_entries.size());
    const std::ptrdiff_t start = position - m_entries.begin();
    for (std::ptrdiff_t offset = 1; offset < count; ++offset) {
      const Entry& entry = m_entries[static_cast<size_t>((((start + direction * offset) % count) + count) % count)];
      if (isMember(entry, name)) {
        return entry.view;
      }
    }
    return nullptr;
  }

  void ScratchpadManager::forgetLastFocused(Scratchpad& scratchpad, std::string_view name, const View* view) const {
    if (view == nullptr) {
      return;
    }
    if (scratchpad.resumeAt == view) {
      scratchpad.resumeAt = memberBeside(name, view, 1);
    }
    if (scratchpad.lastFocused != view) {
      return;
    }
    scratchpad.lastFocused = nullptr;
    View* owner = view->shellParent();
    for (int depth = 0; owner != nullptr && owner != view && depth < kMaxParentDepth; ++depth) {
      if (const Entry* entry = findEntry(owner); entry != nullptr && entry->scratchpad == name) {
        scratchpad.lastFocused = owner;
        return;
      }
      owner = owner->shellParent();
    }
    scratchpad.resumeAt = memberBeside(name, view, 1);
  }

  // An attached dialog has no entry of its own; the queries answer for the window it lives in.
  bool ScratchpadManager::contains(const View* view) const {
    return view != nullptr && findEntry(view->placementRoot()) != nullptr;
  }

  Output* ScratchpadManager::outputFor(const View* view) const {
    const Entry* entry = view != nullptr ? findEntry(view->placementRoot()) : nullptr;
    const Scratchpad* scratchpad = entry != nullptr ? findScratchpad(entry->scratchpad) : nullptr;
    return scratchpad != nullptr ? scratchpad->output : nullptr;
  }

  std::string_view ScratchpadManager::nameFor(const View* view) const {
    const Entry* entry = view != nullptr ? findEntry(view->placementRoot()) : nullptr;
    return entry != nullptr ? std::string_view(entry->scratchpad) : std::string_view{};
  }

  bool ScratchpadManager::hasScratchpad(std::string_view name) const { return findScratchpad(name) != nullptr; }

  Workspace* ScratchpadManager::workspaceOf(std::string_view name) const {
    const Scratchpad* scratchpad = findScratchpad(name);
    return scratchpad != nullptr ? scratchpad->workspace.get() : nullptr;
  }

  std::vector<Workspace*> ScratchpadManager::workspaces() const {
    std::vector<Workspace*> workspaces;
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.workspace != nullptr) {
        workspaces.push_back(scratchpad.workspace.get());
      }
    }
    return workspaces;
  }

  void ScratchpadManager::refreshLayouts() {
    for (auto& [name, scratchpad] : m_scratchpads) {
      Workspace* workspace = scratchpad.workspace.get();
      if (workspace == nullptr) {
        continue;
      }
      // A config reload reasserts the configured mode, dropping any runtime workspace-set-layout override.
      workspace->clearLayoutModeOverride();
      ResolvedLayoutConfig layout = resolveScratchpadLayout(config(), name);
      if (workspace->layoutConfig() != layout) {
        workspace->applyLayoutConfig(std::move(layout));
      } else {
        workspace->tabs().sync();
      }
    }
  }

  void ScratchpadManager::syncWorkspace(std::string_view name, Scratchpad& scratchpad) {
    WorkspaceGroup* group = scratchpad.output != nullptr ? scratchpad.output->workspaceGroup() : nullptr;
    if (group != nullptr && scratchpad.workspace == nullptr) {
      scratchpad.workspace = std::make_unique<Workspace>(*group, name, resolveScratchpadLayout(config(), name));
      for (const Entry& entry : m_entries) {
        if (entry.scratchpad == name && entry.view != nullptr && entry.view->mapped()) {
          entry.view->moveToWorkspace(scratchpad.workspace.get());
        }
      }
    } else if (group != nullptr && scratchpad.workspace->group() != group) {
      scratchpad.workspace->transferTo(*group);
    } else if (group == nullptr && scratchpad.workspace != nullptr) {
      // shortcut: parking drops a tiled pad's arrangement; members rejoin in entry order. Upgrade with
      // Layout::captureState if users lose layouts on monitor loss.
      for (const Entry& entry : m_entries) {
        if (entry.scratchpad != name || entry.view == nullptr) {
          continue;
        }
        entry.view->setWorkspace(nullptr, false);
        entry.view->setSceneParent(m_root);
        entry.view->setOnActiveWorkspace(false);
        entry.view->setNodeEnabled(false);
      }
      scratchpad.workspace.reset();
    }
  }

  Output* ScratchpadManager::presentationOutput(std::string_view name, Output* fallback) const {
    const Scratchpad* scratchpad = findScratchpad(name);
    return scratchpad != nullptr && scratchpad->visible && scratchpad->output != nullptr ? scratchpad->output
                                                                                         : fallback;
  }

  Output* ScratchpadManager::restoreOutputFor(const View* view) const {
    const Entry* entry = view != nullptr ? findEntry(view->placementRoot()) : nullptr;
    return entry != nullptr ? m_server->outputFromName(entry->returnOutput) : nullptr;
  }

  bool ScratchpadManager::moveToScratchpad(View* view, std::string_view name, Output* invokingOutput) {
    return admit(view, name, invokingOutput, Admission::Interactive, {});
  }

  bool ScratchpadManager::assignAutomatic(
      View* view, std::string_view name, Output* placementOutput, const AutomaticAdmission& options
  ) {
    return admit(view, name, placementOutput, Admission::Automatic, options);
  }

  bool ScratchpadManager::assignFromParent(View* view, const View* parent, const AutomaticAdmission& options) {
    const Entry* parentEntry = findEntry(parent);
    const Scratchpad* scratchpad = parentEntry != nullptr ? findScratchpad(parentEntry->scratchpad) : nullptr;
    if (view == nullptr
        || parent == nullptr
        || view == parent
        || view->attached()
        || !parent->mapped()
        || !parent->onActiveWorkspace()
        || scratchpad == nullptr
        || !scratchpad->visible
        || scratchpad->output == nullptr) {
      return false;
    }
    const std::string name = parentEntry->scratchpad;
    return admit(view, name, scratchpad->output, Admission::Automatic, options);
  }

  bool ScratchpadManager::admit(
      View* view, std::string_view name, Output* invokingOutput, Admission admission, const AutomaticAdmission& options
  ) {
    assert(!view->attached());
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || invokingOutput == nullptr || m_server == nullptr || m_root == nullptr) {
      return false;
    }
    if (view == nullptr || !view->mapped()) {
      return false;
    }

    Entry* existing = findEntry(view);
    if (existing != nullptr && admission == Admission::Interactive) {
      return false;
    }

    const bool transferring = existing != nullptr;
    const bool sameScratchpad = existing != nullptr && existing->scratchpad == name;
    const bool wasActivated = view->activated();
    Output* sourceOutput = options.focusOrigin != nullptr ? options.focusOrigin : view->currentOutput();
    // Read before the member joins the pad, whose output then resolves the window's own.
    Output* arrivalOutput = view->currentOutput();
    std::string previousScratchpad;
    // Set when the window leaving another scratchpad was the one that scratchpad showed on its own.
    bool leftPreviousSolo = false;

    const auto setReturnLocation = [](Entry& entry, Output* output, Workspace* workspace) {
      entry.returnOutput.clear();
      entry.returnWorkspace.clear();
      entry.returnWorkspaceIndex = 0;
      entry.returnWorkspaceNamed = false;
      entry.returnWorkspaceAnchor.reset();
      entry.returnDisplacedWorkspace.reset();
      if (workspace != nullptr) {
        entry.returnWorkspace = workspace->name();
        entry.returnWorkspaceIndex = workspace->index();
        entry.returnWorkspaceNamed = workspace->named();
        if (workspace->displacedWorkspace()) {
          entry.returnWorkspaceAnchor = workspace->reserveForLaunch();
          entry.returnDisplacedWorkspace = workspace->displacedWorkspace();
        }
        if (workspace->group() != nullptr && workspace->group()->output() != nullptr) {
          output = workspace->group()->output();
        }
      }
      if (output != nullptr && output->wlr()->name != nullptr) {
        entry.returnOutput = output->wlr()->name;
      }
    };

    if (transferring) {
      if (options.updateRestoreLocation) {
        setReturnLocation(*existing, options.restoreOutput, options.restoreWorkspace);
      }
      if (options.restoreTiled) {
        existing->returnTiled = *options.restoreTiled;
      }
      previousScratchpad = existing->scratchpad;
      if (!sameScratchpad) {
        if (Scratchpad* previous = findScratchpad(previousScratchpad); previous != nullptr) {
          forgetLastFocused(*previous, previousScratchpad, view);
          if (previous->solo == view) {
            previous->solo = nullptr;
            leftPreviousSolo = true;
          }
        }
        existing->scratchpad = std::string(name);
      }
    }

    if (sameScratchpad) {
      if (!scratchpad->visible && scratchpad->output != invokingOutput) {
        moveScratchpad(name, invokingOutput);
      }
      return true;
    }

    if (!scratchpad->visible || scratchpad->output == nullptr) {
      moveScratchpad(name, invokingOutput);
      scratchpad = findScratchpad(name);
    }
    Output* output = scratchpad != nullptr ? scratchpad->output : nullptr;
    Workspace* padWorkspace = scratchpad != nullptr ? scratchpad->workspace.get() : nullptr;
    if (output == nullptr || padWorkspace == nullptr) {
      return false;
    }

    std::optional<Entry> newEntry;
    if (!transferring) {
      newEntry = Entry{
          .view = view,
          .scratchpad = std::string(name),
          .returnOutput = {},
          .displacedPosition = std::nullopt,
          .returnWorkspace = {},
          .returnWorkspaceIndex = 0,
          .returnWorkspaceNamed = false,
          .returnWorkspaceAnchor = {},
          .returnDisplacedWorkspace = nullptr,
          .returnTiled = options.restoreTiled.value_or(view->tiled()),
      };
      Workspace* previous = options.restoreWorkspace != nullptr ? options.restoreWorkspace : view->workspace();
      setReturnLocation(*newEntry, options.restoreOutput, previous);
    }
    if (view->scheduledFullscreen() || view->currentFullscreen()) {
      view->toggleFullscreen();
    }
    if (view->pinned()) {
      view->togglePinned();
    }
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false, false);
    }
    // A tile floats while its layout still holds it, so the column's width and maximize state survive for its return.
    if (view->tiled() && padWorkspace->layoutMode() == LayoutMode::Stacking) {
      view->floatForStackingLayout();
    }
    if (newEntry) {
      m_entries.push_back(std::move(*newEntry));
    }
    view->moveToWorkspace(padWorkspace);
    if (!view->tiled()) {
      // Settle at the float target, not wherever the float animation started.
      const int settledX = view->layoutTargetX();
      const int settledY = view->layoutTargetY();
      view->cancelPositionAnimation();
      view->setPosition(settledX, settledY);
    }
    // An interactive admission is the only shape that needs this: a window still opening is placed afterwards by
    // View::handleMap against the pad's output, and a rule admission is followed by its own summon back to the rule's
    // output, so remapping either would scale or clamp the window twice.
    if (admission == Admission::Interactive && arrivalOutput != nullptr && arrivalOutput != output) {
      remapViewGeometry(
          view, usableArea(*m_server, arrivalOutput), usableArea(*m_server, output), usesProportionalSize(name), true
      );
    }
    view->setInScratchpad(true);
    scratchpad->pendingSpawn.reset();
    const bool visible = scratchpad->visible;
    // A pad showing one window shows a window the user moves in, in place of the others; a dialog brings the member it
    // belongs to. A dialog that joins with its parent automatically shows alongside it instead.
    if (scratchpad->solo != nullptr && admission == Admission::Interactive) {
      scratchpad->solo = memberFor(view, name);
    }
    setVisible(name, visible, admission == Admission::Interactive);
    // Without its shown window, a scratchpad showing one window hides rather than revealing all the others.
    if (transferring && (leftPreviousSolo || !hasEntries(previousScratchpad))) {
      setVisible(previousScratchpad, false, admission == Admission::Interactive);
    }
    view->notifyOutputScale();
    output->updateVrr();
    output->updateHdr();
    m_server->scheduleIpcWindowsEvent();
    // A focused window that ends up off screen gives up focus: its scratchpad may be hidden, or may show another
    // window on its own.
    if (admission == Admission::Interactive || (wasActivated && !presents(*scratchpad, view))) {
      m_server->refocus(sourceOutput);
    }
    return true;
  }

  void ScratchpadManager::retargetBackdrop(Output* output, bool visible, bool animateTransition) {
    if (output == nullptr) {
      return;
    }
    const auto& animation = config().animation;
    const auto& scratchpad = animation.scratchpad;
    auto fadeIt = m_backdropFades.try_emplace(output, 0.0).first;
    AnimatedValue& backdropFade = fadeIt->second;
    const double fadeTarget = visible ? 1.0 : 0.0;
    if (animateTransition
        && animation.enabled
        && scratchpad.enabled
        && (backdropFade.animating() || backdropFade.current() != fadeTarget)) {
      backdropFade.retarget(fadeTarget, scratchpad.durationMs, scratchpad.curve);
    } else {
      backdropFade.snap(fadeTarget);
    }
    updateDimAndBlur(output);
  }

  void ScratchpadManager::setVisible(std::string_view name, bool visible, bool animateTransition) {
    Scratchpad* state = findScratchpad(name);
    if (state == nullptr) {
      return;
    }
    state->visible = visible;
    if (visible) {
      state->hiddenByCycle = false;
    } else {
      state->solo = nullptr;
      state->active = false;
    }
    Output* output = state->output;
    const auto& animation = config().animation;
    const auto& scratchpad = animation.scratchpad;
    const bool animate = animateTransition && output != nullptr && animation.enabled && scratchpad.enabled;
    retargetBackdrop(output, modalOn(output), animateTransition);

    for (const Entry& entry : m_entries) {
      if (entry.scratchpad != name || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      if (presents(*state, view)) {
        view->setOnActiveWorkspace(true);
        view->enterForeignOutput(output);
        std::erase(m_hidingViews, view);
        view->cancelPositionAnimation();
        view->setNodeEnabled(true);
        // A show never reframes a member: the floating margin keeps a window grabbable wherever it was left, and a pad
        // that changes output remaps its members in moveScratchpad().
        if (animate) {
          view->animateFadeTo(1.0F, scratchpad.durationMs, scratchpad.curve);
        } else {
          view->cancelFadeAnimation();
          view->setFadeAlpha(1.0F);
        }
        if (state->workspace != nullptr) {
          state->workspace->syncViewPresentation(view);
        }
      } else {
        view->setOnActiveWorkspace(false);
        // Hidden scratchpad windows remain assigned to their pad's output for
        // foreign-toplevel consumers, just like windows on inactive
        // workspaces. Moving the pad to no output still clears that
        // membership through moveScratchpad().
        view->enterForeignOutput(output);
        // A member already faded out, such as one a single-window pad was not showing, has nothing left to fade.
        const bool alreadyHidden = view->presentedOpacity() <= kFadedOutOpacity
            && std::ranges::find(m_hidingViews, view) == m_hidingViews.end();
        if (animate && !alreadyHidden) {
          view->setNodeEnabled(true);
          view->animateFadeTo(0.0F, scratchpad.durationMs, scratchpad.curve);
          if (std::ranges::find(m_hidingViews, view) == m_hidingViews.end()) {
            m_hidingViews.push_back(view);
          }
        } else {
          std::erase(m_hidingViews, view);
          view->cancelFadeAnimation();
          view->setFadeAlpha(0.0F);
          view->setNodeEnabled(false);
        }
      }
    }
    if (visible && state->workspace != nullptr) {
      state->workspace->markArrange(false);
    }
    updateDimAndBlur(output);
    updateStacking();
  }

  void ScratchpadManager::moveScratchpad(
      std::string_view name, Output* output, View* alreadyPositioned, bool clearDisplacement
  ) {
    Scratchpad* state = findScratchpad(name);
    if (state == nullptr) {
      return;
    }
    Output* previous = state->output;
    if (previous == output) {
      if (output != nullptr) {
        refreshOutputGeometry(output);
      }
      if (clearDisplacement) {
        state->displacedOutput.clear();
        for (Entry& entry : m_entries) {
          if (entry.scratchpad == name) {
            entry.displacedPosition.reset();
          }
        }
      }
      return;
    }

    if (state->visible && output != nullptr) {
      std::string conflictingName;
      for (const auto& [candidateName, candidate] : m_scratchpads) {
        if (&candidate != state && candidate.visible && candidate.output == output) {
          conflictingName = candidateName;
          break;
        }
      }
      if (!conflictingName.empty()) {
        setVisible(conflictingName, false);
      }
    }

    const wlr_box previousArea = state->usableArea.value_or(usableArea(*m_server, previous));
    const wlr_box targetArea = usableArea(*m_server, output);
    const bool proportionalSize = usesProportionalSize(name);
    state->output = output;
    // Retain the size basis while every output is absent.
    if (output != nullptr && targetArea.width > 0 && targetArea.height > 0) {
      state->usableArea = targetArea;
    }
    if (clearDisplacement) {
      state->displacedOutput.clear();
    }
    syncWorkspace(name, *state);

    for (Entry& entry : m_entries) {
      if (entry.scratchpad != name || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      if (clearDisplacement) {
        entry.displacedPosition.reset();
      }
      // A pad with no output keeps the last area it was sized against, so members land on the output that replaces it
      // instead of keeping coordinates that belong to the dead one.
      if (output != nullptr
          && previousArea.width > 0
          && previousArea.height > 0
          && targetArea.width > 0
          && targetArea.height > 0) {
        remapViewGeometry(view, previousArea, targetArea, proportionalSize, view != alreadyPositioned);
      }

      if (presents(*state, view)) {
        view->setOnActiveWorkspace(true);
        std::erase(m_hidingViews, view);
        // A member a single-window scratchpad was not showing is still faded out; showing it here fades it back in.
        view->cancelFadeAnimation();
        view->setNodeEnabled(true);
        if (view != alreadyPositioned && state->workspace != nullptr) {
          state->workspace->syncViewPresentation(view);
        }
      } else if (state->visible) {
        std::erase(m_hidingViews, view);
        view->cancelFadeAnimation();
        view->setFadeAlpha(0.0F);
        view->setOnActiveWorkspace(false);
        view->setNodeEnabled(false);
      }
      // setOnActiveWorkspace() resolves workspace-less views through the
      // preferred output, which need not be this pad's destination during
      // hotplug restoration. Apply the manager-owned assignment last.
      view->enterForeignOutput(output);
      if (view->mapped()) {
        view->notifyOutputScale();
      }
    }
    if (state->workspace != nullptr) {
      state->workspace->markArrange(false);
    }

    if (previous != nullptr) {
      retargetBackdrop(previous, modalOn(previous));
      updateDimAndBlur(previous);
    }
    if (output != nullptr) {
      retargetBackdrop(output, modalOn(output));
      updateDimAndBlur(output);
    }
    if (previous != nullptr) {
      previous->updateVrr();
      previous->updateHdr();
    }
    if (output != nullptr) {
      output->updateVrr();
      output->updateHdr();
    }
    m_server->scheduleIpcWindowsEvent();
  }

  bool ScratchpadManager::usesProportionalSize(std::string_view name) const {
    const ScratchpadConfig* definition = definitionFor(name);
    return definition != nullptr && definition->proportionalSize;
  }

  void ScratchpadManager::remapViewGeometry(
      View* view, const wlr_box& previousArea, const wlr_box& targetArea, bool proportionalSize, bool reposition
  ) {
    if (view == nullptr
        || view->tiled()
        || previousArea.width <= 0
        || previousArea.height <= 0
        || targetArea.width <= 0
        || targetArea.height <= 0) {
      return;
    }
    const SizeHints hints = view->sizeHints();
    const auto proportionalExtent = [](int size, int previous, int target) {
      const long scaled = std::lround(static_cast<double>(size) * target / previous);
      return static_cast<int>(std::clamp(scaled, 1L, static_cast<long>(std::numeric_limits<int>::max())));
    };
    const auto resize = [&](wlr_box& box) {
      if (proportionalSize) {
        box.width = clampWidth(proportionalExtent(box.width, previousArea.width, targetArea.width), hints);
        box.height = clampHeight(proportionalExtent(box.height, previousArea.height, targetArea.height), hints);
      }
    };
    const auto remap = [&](wlr_box& box) {
      const double xFraction = static_cast<double>(box.x - previousArea.x) / previousArea.width;
      const double yFraction = static_cast<double>(box.y - previousArea.y) / previousArea.height;
      const int newX = targetArea.x + static_cast<int>(std::lround(xFraction * targetArea.width));
      const int newY = targetArea.y + static_cast<int>(std::lround(yFraction * targetArea.height));
      resize(box);
      box.x = std::clamp(newX, targetArea.x, targetArea.x + std::max(0, targetArea.width - box.width));
      box.y = std::clamp(newY, targetArea.y, targetArea.y + std::max(0, targetArea.height - box.height));
    };
    if (view->m_hasMaximizeRestoreBox) {
      remap(view->m_maximizeRestoreBox);
    }
    if (view->m_hasFullscreenRestoreBox) {
      remap(view->m_fullscreenRestoreBox);
    }

    const bool managedSize = view->scheduledFullscreen()
        || view->currentFullscreen()
        || view->maximizedToEdges()
        || view->m_floatingMaximized;
    const auto [width, height] = proportionalSize && !managedSize
        ? view->floatingSize()
        : std::array{view->currentSize().width, view->currentSize().height};
    wlr_box box{view->layoutX(), view->layoutY(), width, height};
    if (reposition) {
      remap(box);
    } else {
      resize(box);
    }
    if (reposition) {
      view->cancelPositionAnimation();
      view->setPosition(box.x, box.y);
    }
    if (proportionalSize && !managedSize && (box.width != width || box.height != height)) {
      view->requestFloatingSize(box.width, box.height);
      view->beginResizeAnimation(box.width, box.height);
    }
  }

  void ScratchpadManager::refreshOutputGeometry(Output* output) {
    if (output == nullptr || m_server == nullptr) {
      return;
    }
    const wlr_box targetArea = usableArea(*m_server, output);
    if (targetArea.width <= 0 || targetArea.height <= 0) {
      return;
    }

    bool geometryChanged = false;
    bool movedEntry = false;
    Cursor* cursor = m_server->cursor();
    for (auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.output != output) {
        continue;
      }
      if (scratchpad.workspace != nullptr) {
        scratchpad.workspace->markArrange(false);
      }
      if (!scratchpad.usableArea) {
        scratchpad.usableArea = targetArea;
        continue;
      }
      const wlr_box previousArea = *scratchpad.usableArea;
      scratchpad.usableArea = targetArea;
      if (sameBox(previousArea, targetArea) || previousArea.width <= 0 || previousArea.height <= 0) {
        continue;
      }
      geometryChanged = true;

      for (Entry& entry : m_entries) {
        if (entry.scratchpad != name
            || entry.view == nullptr
            || (cursor != nullptr && cursor->isDraggingView(entry.view))) {
          continue;
        }
        View* view = entry.view;
        remapViewGeometry(view, previousArea, targetArea, false);
        if (scratchpad.workspace != nullptr) {
          scratchpad.workspace->syncViewPresentation(view);
        }
        movedEntry = true;
      }
    }

    if (geometryChanged) {
      updateDimAndBlur(output);
    }
    if (movedEntry) {
      m_server->scheduleIpcWindowsEvent();
    }
  }

  wlr_scene_rect* ScratchpadManager::dimRectFor(Output* output) {
    if (const auto it = m_dimRects.find(output); it != m_dimRects.end()) {
      return it->second;
    }
    static constexpr float kBlack[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    wlr_scene_rect* rect = wlr_scene_rect_create(m_root, 1, 1, kBlack);
    wlr_scene_node_lower_to_bottom(&rect->node);
    wlr_scene_node_set_enabled(&rect->node, false);
    m_dimRects.emplace(output, rect);
    return rect;
  }

  wlr_scene_blur* ScratchpadManager::blurNodeFor(Output* output) {
    if (const auto it = m_blurNodes.find(output); it != m_blurNodes.end()) {
      return it->second;
    }
    wlr_scene_blur* blur = wlr_scene_blur_create(m_root, 1, 1);
    if (blur != nullptr) {
      wlr_scene_node_lower_to_bottom(&blur->node);
      wlr_scene_node_set_enabled(&blur->node, false);
      m_blurNodes.emplace(output, blur);
    }
    return blur;
  }

  void ScratchpadManager::updateDimAndBlur(Output* output) {
    if (output == nullptr) {
      return;
    }
    const double dim = config().animation.scratchpad.dim;
    const bool blurEnabled = config().animation.scratchpad.blur && config().appearance.blur.enabled;
    const auto fade = m_backdropFades.find(output);
    const float currentAlpha = fade != m_backdropFades.end() ? static_cast<float>(fade->second.current()) : 0.0F;

    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &box);

    if (wlr_scene_rect* rect = dimRectFor(output)) {
      if (dim <= 0.0 || currentAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&rect->node, false);
      } else {
        wlr_scene_node_set_position(&rect->node, box.x, box.y);
        wlr_scene_rect_set_size(rect, box.width, box.height);
        const float color[4] = {
            0.0F,
            0.0F,
            0.0F,
            std::clamp(static_cast<float>(dim * currentAlpha), 0.0F, 1.0F),
        };
        wlr_scene_rect_set_color(rect, color);
        wlr_scene_node_set_enabled(&rect->node, true);
      }
    }

    if (wlr_scene_blur* blur = blurNodeFor(output)) {
      if (!blurEnabled || currentAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&blur->node, false);
      } else {
        wlr_scene_node_set_position(&blur->node, box.x, box.y);
        wlr_scene_blur_set_size(blur, box.width, box.height);
        wlr_scene_blur_set_corner_radius(blur, 0);
        wlr_scene_blur_set_alpha(blur, std::clamp(currentAlpha, 0.0F, 1.0F));
        wlr_scene_node_set_enabled(&blur->node, true);
      }
    }
  }

  void ScratchpadManager::updateStacking() {
    const auto overFullscreen = [this](const auto& item) {
      const auto& [name, scratchpad] = item;
      if (scratchpad.output == nullptr) {
        return false;
      }
      // A pad fading out stays lifted until it is gone, so it does not drop under the fullscreen window.
      return (scratchpad.visible || animatesOn(scratchpad.output))
          && (scratchpad.output->fullscreenAboveTopLayer() || std::ranges::any_of(m_entries, [&](const Entry& entry) {
                return entry.scratchpad == name
                    && entry.view != nullptr
                    && (entry.view->scheduledFullscreen() || entry.view->currentFullscreen());
              }));
    };
    // Anchor to the tree below, so a drag tree that View::enterDragPresentation stacks above the pad stays there.
    wlr_scene_tree* below =
        std::ranges::any_of(m_scratchpads, overFullscreen) ? m_server->fullscreenTree() : m_server->xdgTree();
    wlr_scene_node_place_above(&m_root->node, &below->node);
  }

  void ScratchpadManager::releaseOutput(Output* output) {
    std::vector<std::string> stranded;
    for (auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.output == output) {
        stranded.push_back(name);
      }
    }
    for (const std::string& name : stranded) {
      moveScratchpad(name, nullptr, nullptr, false);
    }

    m_backdropFades.erase(output);
    if (const auto it = m_dimRects.find(output); it != m_dimRects.end()) {
      wlr_scene_node_destroy(&it->second->node);
      m_dimRects.erase(it);
    }
    if (const auto it = m_blurNodes.find(output); it != m_blurNodes.end()) {
      wlr_scene_node_destroy(&it->second->node);
      m_blurNodes.erase(it);
    }
  }

  void ScratchpadManager::applyConfig() {
    const auto& animation = config().animation;
    const bool animate = animation.enabled && animation.scratchpad.enabled;
    if (!animate) {
      for (auto& [output, fade] : m_backdropFades) {
        fade.snap(modalOn(output) ? 1.0 : 0.0);
        updateDimAndBlur(output);
      }
      for (const Entry& entry : m_entries) {
        if (entry.view == nullptr) {
          continue;
        }
        const Scratchpad* scratchpad = findScratchpad(entry.scratchpad);
        const bool visible = scratchpad != nullptr && presents(*scratchpad, entry.view);
        entry.view->cancelFadeAnimation();
        entry.view->setFadeAlpha(visible ? 1.0F : 0.0F);
        entry.view->setNodeEnabled(visible);
      }
      m_hidingViews.clear();
      return;
    }
    for (const auto& entry : m_backdropFades) {
      updateDimAndBlur(entry.first);
    }
  }

  bool ScratchpadManager::summon(std::string_view name, Output* invokingOutput, View* member) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || invokingOutput == nullptr || overviewOpen(*m_server)) {
      return false;
    }
    // A pad showing one window shows the member asked for instead; a dialog shows with the member it belongs to.
    member = memberFor(member, name);
    if (member != nullptr && scratchpad->solo != nullptr && scratchpad->solo != member) {
      scratchpad->solo = member;
      if (scratchpad->output == invokingOutput) {
        setVisible(name, true);
        return true;
      }
    }
    if (scratchpad->visible && scratchpad->output == invokingOutput) {
      return true;
    }

    std::string conflictingName;
    for (const auto& [candidateName, candidate] : m_scratchpads) {
      if (&candidate != scratchpad && candidate.visible && candidate.output == invokingOutput) {
        conflictingName = candidateName;
        break;
      }
    }
    if (!conflictingName.empty()) {
      setVisible(conflictingName, false);
    }

    if (scratchpad->output != invokingOutput) {
      moveScratchpad(name, invokingOutput);
      scratchpad = findScratchpad(name);
    }
    if (scratchpad == nullptr) {
      return false;
    }
    if (!scratchpad->visible) {
      setVisible(name, true);
    }
    return true;
  }

  bool ScratchpadManager::toggle(std::string_view name, Output* invokingOutput) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || invokingOutput == nullptr || overviewOpen(*m_server)) {
      return false;
    }
    if (scratchpad->visible && scratchpad->output == invokingOutput) {
      setVisible(name, false);
      m_server->refocus(invokingOutput);
      return true;
    }

    if (!summon(name, invokingOutput)) {
      return false;
    }
    scratchpad->active = true;
    refocusActive(invokingOutput);
    if (!hasEntries(name) && (!scratchpad->pendingSpawn || !scratchpad->pendingSpawn->live())) {
      const ScratchpadConfig* definition = definitionFor(name);
      if (definition != nullptr && !definition->spawnWhenEmpty.empty()) {
        scratchpad->pendingSpawn = PendingSpawn{.expiresAt = std::chrono::steady_clock::now() + kSpawnTimeout};
        m_server->spawn(definition->spawnWhenEmpty.c_str(), "scratchpad.spawn_when_empty", true);
      }
    }
    return true;
  }

  std::string_view ScratchpadManager::activeOn(const Output* output) const {
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (output != nullptr && scratchpad.output == output && scratchpad.visible && scratchpad.active) {
        return name;
      }
    }
    return {};
  }

  void ScratchpadManager::deactivateOn(const Output* output) {
    for (auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.output == output) {
        scratchpad.active = false;
      }
    }
  }

  bool ScratchpadManager::dismissOn(Output* output) {
    const auto shown = std::ranges::find_if(m_scratchpads, [output](const auto& entry) {
      return output != nullptr && entry.second.visible && entry.second.output == output;
    });
    if (shown == m_scratchpads.end()) {
      return false;
    }
    setVisible(shown->first, false);
    return true;
  }

  bool ScratchpadManager::focusOn(Output* output) {
    for (auto& [name, scratchpad] : m_scratchpads) {
      if (output != nullptr && scratchpad.visible && scratchpad.output == output) {
        scratchpad.active = true;
        return refocusActive(output);
      }
    }
    return false;
  }

  bool ScratchpadManager::refocusActive(Output* output) {
    const std::string_view name = activeOn(output);
    if (name.empty()) {
      return false;
    }
    if (m_server->sessionLocked()) {
      return true;
    }
    if (View* view = focused(name)) {
      if (!view->inputDelegate()->acceptsKeyboardFocus()) {
        return false;
      }
      m_server->focusView(view);
    } else if (m_server->exclusiveKeyboardLayer() != nullptr) {
      m_server->refocus();
    } else {
      m_server->clearKeyboardFocus();
    }
    return true;
  }

  void ScratchpadManager::hideAll() {
    std::vector<std::string> visible;
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.visible) {
        visible.push_back(name);
      }
    }
    for (const std::string& name : visible) {
      setVisible(name, false, false);
    }
  }

  View* ScratchpadManager::focused(std::string_view name) const {
    const Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || !scratchpad->visible || scratchpad->output == nullptr) {
      return nullptr;
    }
    if (scratchpad->lastFocused != nullptr) {
      const Entry* remembered = findEntry(scratchpad->lastFocused);
      if (remembered != nullptr
          && remembered->scratchpad == name
          && scratchpad->lastFocused->mapped()
          && presents(*scratchpad, scratchpad->lastFocused)) {
        return scratchpad->lastFocused;
      }
    }
    if (scratchpad->solo != nullptr) {
      return scratchpad->solo;
    }
    for (const Entry& entry : m_entries) {
      if (entry.scratchpad == name && entry.view != nullptr && entry.view->mapped()) {
        return entry.view;
      }
    }
    return nullptr;
  }

  bool ScratchpadManager::hasFocus(std::string_view name) const {
    if (m_focusedView == nullptr || focused(name) == nullptr) {
      return false;
    }
    const Entry* entry = findEntry(m_focusedView);
    return entry != nullptr && entry->scratchpad == name;
  }

  View* ScratchpadManager::focusedOn(const Output* output) const {
    if (output == nullptr
        || m_focusedView == nullptr
        || !m_focusedView->mapped()
        || !m_focusedView->inputDelegate()->acceptsKeyboardFocus()
        || !m_focusedView->onActiveWorkspace()) {
      return nullptr;
    }
    return outputFor(m_focusedView) == output ? m_focusedView : nullptr;
  }

  void ScratchpadManager::noteFocus(View* view) {
    m_focusedView = nullptr;
    Entry* entry = findEntry(view);
    if (entry == nullptr) {
      if (view != nullptr) {
        // Focus that leaves for a window behind a modal scratchpad takes the scratchpad away with it.
        if (!view->pinned() && modalOn(view->currentOutput())) {
          dismissOn(view->currentOutput());
        }
        deactivateOn(view->currentOutput());
      }
      return;
    }
    Scratchpad* scratchpad = findScratchpad(entry->scratchpad);
    if (scratchpad == nullptr) {
      return;
    }
    m_focusedView = view;
    scratchpad->active = true;
    scratchpad->lastFocused = view;
  }

  void ScratchpadManager::finishMove(View* view, Output* output) {
    Entry* entry = findEntry(view);
    if (entry == nullptr || view == nullptr) {
      return;
    }
    Scratchpad* scratchpad = findScratchpad(entry->scratchpad);
    if (scratchpad == nullptr) {
      return;
    }
    const std::string name = entry->scratchpad;
    if (output != nullptr && scratchpad->output != output) {
      moveScratchpad(name, output, view);
    }
    view->restoreHomePresentation();
  }

  bool ScratchpadManager::focusNext(std::string_view name) {
    const Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || !scratchpad->visible || scratchpad->output == nullptr) {
      return false;
    }
    std::vector<View*> views;
    for (const Entry& entry : m_entries) {
      // Only windows on screen: a pad showing one window keeps focus on it.
      if (entry.scratchpad == name
          && entry.view != nullptr
          && entry.view->mapped()
          && entry.view->inputDelegate()->acceptsKeyboardFocus()
          && presents(*scratchpad, entry.view)) {
        views.push_back(entry.view);
      }
    }
    if (views.empty()) {
      return false;
    }
    View* current = focused(name);
    const auto it = std::ranges::find(views, current);
    View* target = it == views.end() || std::next(it) == views.end() ? views.front() : *std::next(it);
    m_server->focusView(target, FocusReason::Directional);
    return true;
  }

  bool ScratchpadManager::showStep(std::string_view name, int direction, Output* invokingOutput) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr
        || invokingOutput == nullptr
        || (direction != -1 && direction != 1)
        || overviewOpen(*m_server)) {
      return false;
    }
    if (!hasEntries(name)) {
      // An empty pad behaves as toggle does: it may launch its spawn_when_empty command.
      return toggle(name, invokingOutput);
    }
    const std::vector<View*> steps = members(name);
    if (steps.empty()) {
      return false;
    }
    // A focused or remembered dialog stands for the member it belongs to.
    const auto findMember = [&](View* view) { return std::ranges::find(steps, memberFor(view, name)); };
    View* target = nullptr;
    if (scratchpad->visible && scratchpad->output == invokingOutput) {
      // Step from the window on show, or from the focused one when every window shows.
      target = stepFrom(steps, memberFor(focused(name), name), direction);
    } else {
      const auto remembered = findMember(scratchpad->lastFocused);
      target = remembered != steps.end() ? *remembered : steps.front();
    }
    return showOnly(name, *scratchpad, target, invokingOutput);
  }

  bool ScratchpadManager::cycle(std::string_view name, int direction, Output* invokingOutput, const View* focus) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || invokingOutput == nullptr || overviewOpen(*m_server)) {
      return false;
    }
    if (!hasEntries(name)) {
      return toggle(name, invokingOutput);
    }
    if (scratchpad->visible) {
      // With no window holding the keyboard, as under a layer surface or the lock, there is nothing to take focus back
      // from, so a pad shown here hides as toggle does.
      if (focus != nullptr ? nameFor(focus) == name : scratchpad->output == invokingOutput) {
        setVisible(name, false);
        scratchpad->hiddenByCycle = true;
        m_server->refocus(scratchpad->output);
        return true;
      }
      // Shown without focus, here or on another output: bring it here as it is and focus its window.
      if (!summon(name, invokingOutput)) {
        return false;
      }
      scratchpad->active = true;
      refocusActive(invokingOutput);
      return true;
    }
    const std::vector<View*> steps = members(name);
    if (steps.empty()) {
      return false;
    }
    const auto isStep = [&](const View* view) { return std::ranges::find(steps, view) != steps.end(); };
    View* target = nullptr;
    if (View* last = memberFor(scratchpad->lastFocused, name); isStep(last)) {
      // A pad dismissed some other way, such as by focusing a window behind it, comes back as it was.
      target = scratchpad->hiddenByCycle ? stepFrom(steps, last, direction) : last;
    } else if (isStep(scratchpad->resumeAt)) {
      // resumeAt is the window after the one that left, so one step back from it is the window before that one.
      target = direction > 0 ? scratchpad->resumeAt : stepFrom(steps, scratchpad->resumeAt, -1);
    } else {
      target = direction > 0 ? steps.front() : steps.back();
    }
    return showOnly(name, *scratchpad, target, invokingOutput);
  }

  bool
  ScratchpadManager::showOnly(std::string_view name, Scratchpad& scratchpad, View* member, Output* invokingOutput) {
    scratchpad.solo = member;
    if (scratchpad.visible && scratchpad.output == invokingOutput) {
      setVisible(name, true);
    } else if (!summon(name, invokingOutput)) {
      scratchpad.solo = nullptr;
      return false;
    }
    m_server->focusView(member);
    return true;
  }

  bool ScratchpadManager::restoreView(View* view, Output* fallback, bool focus, Workspace* target) {
    const auto iterator =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (iterator == m_entries.end()) {
      return false;
    }
    Entry entry = std::move(*iterator);
    Scratchpad* scratchpad = findScratchpad(entry.scratchpad);
    Output* scratchpadOutput = scratchpad != nullptr ? scratchpad->output : fallback;
    if (scratchpad != nullptr) {
      forgetLastFocused(*scratchpad, entry.scratchpad, view);
    }
    const bool wasSolo = scratchpad != nullptr && scratchpad->solo == view;
    m_entries.erase(iterator);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);

    view->setInScratchpad(false);
    Output* restoreOutput =
        target != nullptr ? target->group()->output() : m_server->outputFromName(entry.returnOutput);
    if (restoreOutput == nullptr) {
      restoreOutput = scratchpadOutput;
    }
    Workspace* workspace = target;
    const auto origin = target == nullptr ? entry.returnDisplacedWorkspace : nullptr;
    bool homeAvailable = false;
    if (origin) {
      Output* homeOutput = m_server->outputFromName(origin->outputName);
      WorkspaceGroup* homeGroup = homeOutput != nullptr ? homeOutput->workspaceGroup() : nullptr;
      homeAvailable = homeGroup != nullptr;
      if (homeAvailable) {
        restoreOutput = homeOutput;
        if (origin->workspaceNamed) {
          workspace = homeGroup->workspaceNamed(origin->workspaceName);
        } else {
          if (homeGroup->dynamic()) {
            while (homeGroup->workspaceCount() <= origin->workspaceIndex) {
              if (homeGroup->insertDynamicWorkspace(homeGroup->workspaceCount()) == nullptr) {
                break;
              }
            }
          }
          workspace = homeGroup->workspaceAt(origin->workspaceIndex);
          if (homeGroup->dynamic() && workspace != nullptr && (workspace->named() || workspace->displacedWorkspace())) {
            workspace = homeGroup->insertDynamicWorkspace(origin->workspaceIndex);
          }
        }
        if (workspace != nullptr && workspace->displacedWorkspace()) {
          workspace = nullptr;
        }
        if (workspace == nullptr && homeGroup->active() != nullptr && !homeGroup->active()->displacedWorkspace()) {
          workspace = homeGroup->active();
        }
        for (size_t index = 0; workspace == nullptr && index < homeGroup->workspaceCount(); ++index) {
          Workspace* candidate = homeGroup->workspaceAt(index);
          if (!candidate->displacedWorkspace()) {
            workspace = candidate;
          }
        }
      } else {
        if (const auto anchor = entry.returnWorkspaceAnchor.lock(); anchor != nullptr && anchor->workspace != nullptr) {
          Workspace* saved = anchor->workspace;
          Output* output = saved->group()->output();
          if (saved->displacedWorkspace() && output->desktopEnabled() && output->wlr()->enabled) {
            workspace = saved;
          }
        }
        if (workspace == nullptr) {
          workspace = m_server->displacedWorkspaceDestination(origin, restoreOutput);
        }
      }
      if (workspace != nullptr) {
        restoreOutput = workspace->group()->output();
      }
    }
    if (target == nullptr && restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr) {
      WorkspaceGroup* group = restoreOutput->workspaceGroup();
      if (!origin && !entry.returnWorkspace.empty()) {
        workspace = entry.returnWorkspaceNamed ? group->workspaceNamed(entry.returnWorkspace)
                                               : group->workspaceAtClamped(entry.returnWorkspaceIndex);
        if (!entry.returnWorkspaceNamed && group->dynamic() && workspace != nullptr && workspace->named()) {
          workspace = group->insertDynamicWorkspace(entry.returnWorkspaceIndex);
        }
      }
      if (workspace == nullptr) {
        workspace = group->active();
      }
    }
    view->moveToWorkspace(workspace, false);
    if (origin && !homeAvailable && !view->displacedHome()) {
      View::DisplacedHome home;
      home.outputName = origin->outputName;
      home.workspaceName = origin->workspaceName;
      home.workspaceIndex = origin->workspaceIndex;
      home.workspaceNamed = origin->workspaceNamed;
      home.layoutModeOverride = origin->layoutModeOverride;
      home.configGeneration = origin->configGeneration;
      home.workspaceOrigin = origin;
      view->markDisplaced(std::move(home));
      m_server->scheduleDisplacedViewRestore();
    }
    if (entry.returnTiled) {
      view->setFloating(false, false);
    } else {
      view->setFloating(true, false);
      if (restoreOutput != nullptr && restoreOutput != scratchpadOutput) {
        const wlr_box area = restoreOutput->usableArea();
        if (area.width > 0 && area.height > 0) {
          const int width = view->currentSize().width;
          const int height = view->currentSize().height;
          view->setPosition(
              std::clamp(view->layoutX(), area.x, area.x + std::max(0, area.width - width)),
              std::clamp(view->layoutY(), area.y, area.y + std::max(0, area.height - height))
          );
        }
      }
    }
    if (workspace != nullptr) {
      workspace->exitFullscreenForIncomingView(view);
      workspace->syncViewPresentation(view);
    }

    // Without its shown window, a pad showing one window hides rather than revealing all the others.
    if (scratchpad != nullptr && (wasSolo || !hasEntries(entry.scratchpad)) && scratchpad->visible) {
      setVisible(entry.scratchpad, false);
    }
    if (scratchpadOutput != nullptr) {
      scratchpadOutput->updateVrr();
      scratchpadOutput->updateHdr();
    }
    if (focus) {
      m_server->focusView(view);
    }
    return true;
  }

  bool ScratchpadManager::restoreFocused(std::string_view name) {
    View* view = focused(name);
    if (view == nullptr) {
      return false;
    }
    const Scratchpad* scratchpad = findScratchpad(name);
    return restoreView(view, scratchpad != nullptr ? scratchpad->output : nullptr, true);
  }

  bool ScratchpadManager::restoreToWorkspace(View* view, Workspace* workspace) {
    return workspace != nullptr && restoreView(view, outputFor(view), false, workspace);
  }

  void ScratchpadManager::remove(View* view) {
    const auto iterator =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (iterator == m_entries.end()) {
      return;
    }
    const std::string name = iterator->scratchpad;
    Scratchpad* scratchpad = findScratchpad(name);
    view->setInScratchpad(false);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    if (scratchpad != nullptr) {
      forgetLastFocused(*scratchpad, name, view);
    }
    const bool wasSolo = scratchpad != nullptr && scratchpad->solo == view;
    std::erase(m_hidingViews, view);
    m_entries.erase(iterator);
    // Without its shown window, a pad showing one window hides rather than revealing all the others.
    if (scratchpad != nullptr && wasSolo && scratchpad->visible) {
      setVisible(name, false);
    }
    // A hide_when_empty pad hides once its last member leaves, handing that member's keyboard focus back.
    const ScratchpadConfig* definition = definitionFor(name);
    if (scratchpad != nullptr
        && scratchpad->visible
        && definition != nullptr
        && definition->hideWhenEmpty
        && !hasEntries(name)) {
      const bool heldFocus = View::fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface) == view;
      setVisible(name, false);
      if (heldFocus) {
        m_server->refocus(scratchpad->output);
      }
    }
  }

  void ScratchpadManager::moveOutput(Output* from, Output* to) {
    if (from == to) {
      return;
    }
    std::vector<std::string> moving;
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (scratchpad.output == from) {
        moving.push_back(name);
      }
    }
    for (const std::string& name : moving) {
      Scratchpad* scratchpad = findScratchpad(name);
      if (scratchpad == nullptr) {
        continue;
      }
      if (scratchpad->displacedOutput.empty() && from != nullptr && from->wlr()->name != nullptr) {
        scratchpad->displacedOutput = from->wlr()->name;
        const wlr_box homeArea = from->layoutBox();
        for (Entry& entry : m_entries) {
          if (entry.scratchpad != name || entry.view == nullptr || homeArea.width <= 0 || homeArea.height <= 0) {
            continue;
          }
          entry.displacedPosition = {{
              static_cast<double>(entry.view->layoutX() - homeArea.x) / homeArea.width,
              static_cast<double>(entry.view->layoutY() - homeArea.y) / homeArea.height,
          }};
        }
      }
      moveScratchpad(name, to, nullptr, false);
    }
  }

  void ScratchpadManager::reconcileConfig() {
    std::set<std::string, std::less<>> desired;
    if (config().scratchpads.empty()) {
      desired.emplace(kImplicitScratchpad);
    } else {
      for (const ScratchpadConfig& scratchpad : config().scratchpads) {
        desired.emplace(scratchpad.name);
      }
    }

    std::vector<std::string> removed;
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (!desired.contains(name)) {
        removed.push_back(name);
      }
    }
    bool refocusNeeded = false;
    for (const std::string& name : removed) {
      Scratchpad* scratchpad = findScratchpad(name);
      Output* fallback = scratchpad != nullptr ? scratchpad->output : nullptr;
      if (scratchpad != nullptr && scratchpad->visible) {
        setVisible(name, false, false);
        refocusNeeded = true;
      }
      std::vector<View*> views;
      for (const Entry& entry : m_entries) {
        if (entry.scratchpad == name && entry.view != nullptr) {
          views.push_back(entry.view);
        }
      }
      if (Cursor* cursor = m_server->cursor(); cursor != nullptr) {
        const bool removesGrabbedView =
            std::ranges::any_of(views, [&](const View* view) { return cursor->isDraggingView(view); });
        if (removesGrabbedView) {
          cursor->resetMode();
        }
      }
      for (View* view : views) {
        refocusNeeded = restoreView(view, fallback, false) || refocusNeeded;
      }
      m_scratchpads.erase(name);
    }
    for (const std::string& name : desired) {
      m_scratchpads.try_emplace(name);
    }
    refreshLayouts();
    // A shown pad keeps its backdrop only while it is still modal.
    for (const auto& [output, fade] : m_backdropFades) {
      retargetBackdrop(output, modalOn(output));
    }
    // A pad that turned modal does not stay up over the window behind it that has focus.
    if (View* focused = View::fromSurface(m_server->seat()->wlr()->keyboard_state.focused_surface);
        focused != nullptr && !contains(focused)) {
      noteFocus(focused);
    }
    if (refocusNeeded) {
      m_server->refocus();
    }
  }

  size_t ScratchpadManager::restoreDisplaced(Output* fallback) {
    if (fallback == nullptr || m_server == nullptr) {
      return 0;
    }
    size_t restored = 0;
    std::vector<std::string> names;
    for (const auto& [name, scratchpad] : m_scratchpads) {
      if (!scratchpad.displacedOutput.empty() || scratchpad.output == nullptr) {
        names.push_back(name);
      }
    }

    for (const std::string& name : names) {
      Scratchpad* scratchpad = findScratchpad(name);
      if (scratchpad == nullptr) {
        continue;
      }
      Output* home =
          scratchpad->displacedOutput.empty() ? nullptr : m_server->outputFromName(scratchpad->displacedOutput);
      Output* target = home != nullptr ? home : (scratchpad->output == nullptr ? fallback : nullptr);
      if (target == nullptr) {
        continue;
      }
      const bool moved = target != scratchpad->output;
      if (moved) {
        moveScratchpad(name, target, nullptr, false);
      }
      scratchpad = findScratchpad(name);
      if (home != nullptr && scratchpad != nullptr) {
        const wlr_box homeArea = home->layoutBox();
        for (Entry& entry : m_entries) {
          if (entry.scratchpad != name || entry.view == nullptr) {
            continue;
          }
          const bool managerOwnedGeometry = entry.view->scheduledFullscreen()
              || entry.view->currentFullscreen()
              || entry.view->maximizedToEdges()
              || entry.view->m_floatingMaximized
              || entry.view->tiled();
          if (entry.displacedPosition && !managerOwnedGeometry && homeArea.width > 0 && homeArea.height > 0) {
            entry.view->cancelPositionAnimation();
            const FloatingPoint restored{
                .x = homeArea.x + static_cast<int>(std::lround((*entry.displacedPosition)[0] * homeArea.width)),
                .y = homeArea.y + static_cast<int>(std::lround((*entry.displacedPosition)[1] * homeArea.height)),
            };
            // A wider output scales the fraction past the grabbable margin, so the placement is clamped like any other
            // restore.
            const FloatingPoint origin = entry.view->grabbableOrigin(restored, home->usableArea());
            entry.view->setPosition(origin.x, origin.y);
            if (scratchpad->visible && scratchpad->workspace != nullptr) {
              scratchpad->workspace->syncViewPresentation(entry.view);
            }
          }
          entry.displacedPosition.reset();
        }
        scratchpad->displacedOutput.clear();
      }
      if (moved) {
        restored += static_cast<size_t>(std::ranges::count_if(m_entries, [&](const Entry& entry) {
          return entry.scratchpad == name;
        }));
      }
    }
    return restored;
  }

} // namespace umbriel
