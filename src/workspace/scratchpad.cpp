#include "workspace/scratchpad.h"

#include "config/config.h"
#include "input/cursor.h"
#include "output/output.h"
#include "scene/animation_shader.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <set>
#include <utility>

namespace umbriel {

  namespace {
    constexpr std::string_view kImplicitScratchpad = "default";

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
  } // namespace

  ScratchpadManager::ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot)
      : m_server(&server), m_root(root), m_shadowRoot(shadowRoot) {
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

  bool ScratchpadManager::tickAnimations(uint64_t nowMsec) {
    bool movedBackdrop = false;
    for (auto& [output, fade] : m_backdropFades) {
      if (fade.tick(nowMsec)) {
        updateDimAndBlur(output);
        if (const auto rect = m_dimRects.find(output); rect != m_dimRects.end()) {
          updateAnimationShader(&rect->second->node, m_server->renderer(), AnimationEvent::Scratchpad, fade);
        }
        if (const auto blur = m_blurNodes.find(output); blur != m_blurNodes.end()) {
          updateAnimationShader(&blur->second->node, m_server->renderer(), AnimationEvent::Scratchpad, fade);
        }
        movedBackdrop = true;
      }
    }

    if (m_hidingViews.empty()) {
      return movedBackdrop;
    }
    std::erase_if(m_hidingViews, [](View* view) {
      if (view->presentedOpacity() > 0.002F) {
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

  bool ScratchpadManager::visibleOn(Output* output) const {
    return std::ranges::any_of(m_scratchpads, [output](const auto& entry) {
      return entry.second.visible && entry.second.output == output;
    });
  }

  bool ScratchpadManager::contains(const View* view) const { return findEntry(view) != nullptr; }

  Output* ScratchpadManager::outputFor(const View* view) const {
    const Entry* entry = findEntry(view);
    const Scratchpad* scratchpad = entry != nullptr ? findScratchpad(entry->scratchpad) : nullptr;
    return scratchpad != nullptr ? scratchpad->output : nullptr;
  }

  std::string_view ScratchpadManager::nameFor(const View* view) const {
    const Entry* entry = findEntry(view);
    return entry != nullptr ? std::string_view(entry->scratchpad) : std::string_view{};
  }

  bool ScratchpadManager::hasScratchpad(std::string_view name) const { return findScratchpad(name) != nullptr; }

  bool ScratchpadManager::moveToScratchpad(View* view, std::string_view name, Output* invokingOutput) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr
        || invokingOutput == nullptr
        || m_server == nullptr
        || m_root == nullptr
        || m_shadowRoot == nullptr) {
      return false;
    }
    if (view == nullptr || !view->mapped() || contains(view)) {
      return false;
    }

    if (!scratchpad->visible || scratchpad->output == nullptr) {
      moveScratchpad(name, invokingOutput);
      scratchpad = findScratchpad(name);
    }
    Output* output = scratchpad != nullptr ? scratchpad->output : nullptr;
    if (output == nullptr) {
      return false;
    }

    Entry entry{
        .view = view,
        .scratchpad = std::string(name),
        .returnOutput = {},
        .displacedPosition = std::nullopt,
        .returnWorkspace = {},
        .returnTiled = view->tiled(),
    };
    Output* sourceOutput = nullptr;
    if (Workspace* previous = view->workspace()) {
      entry.returnWorkspace = previous->name();
      if (previous->group() != nullptr && previous->group()->output() != nullptr) {
        sourceOutput = previous->group()->output();
        entry.returnOutput = sourceOutput->wlr()->name;
      }
    }
    if (view->toplevel()->scheduled.fullscreen || view->toplevel()->current.fullscreen) {
      view->toggleFullscreen();
    }
    if (view->pinned()) {
      view->togglePinned();
    }
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false, false);
    }
    view->setFloating(true);
    view->cancelPositionAnimation();

    const wlr_box targetArea = usableArea(*m_server, output);
    const auto& scratchpadConfig = config().animation.scratchpad;
    if (scratchpadConfig.fullscreen) {
      if (!view->toplevel()->scheduled.fullscreen && !view->toplevel()->current.fullscreen) {
        view->toggleFullscreen();
      }
    } else if (scratchpadConfig.maximize) {
      view->toggleMaximizedToEdges();
    } else if (
        scratchpadConfig.scale > 0.0 && scratchpadConfig.scale <= 1.0 && targetArea.width > 0 && targetArea.height > 0
    ) {
      const int targetWidth = std::max(100, static_cast<int>(std::lround(targetArea.width * scratchpadConfig.scale)));
      const int targetHeight = std::max(100, static_cast<int>(std::lround(targetArea.height * scratchpadConfig.scale)));
      wlr_xdg_toplevel_set_size(view->toplevel(), targetWidth, targetHeight);
      view->setPosition(
          targetArea.x + std::max(0, (targetArea.width - targetWidth) / 2),
          targetArea.y + std::max(0, (targetArea.height - targetHeight) / 2)
      );
    } else {
      view->setPosition(view->sceneTree()->node.x, view->sceneTree()->node.y);
    }

    view->moveToWorkspace(nullptr);
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setInScratchpad(true);
    const bool visible = scratchpad->visible;
    m_entries.push_back(std::move(entry));
    setVisible(name, visible);
    view->notifyOutputScale();
    output->updateVrr();
    output->updateHdr();
    m_server->scheduleIpcWindowsEvent();
    m_server->refocus(sourceOutput);
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
    Output* output = state->output;
    const bool presented = visible && output != nullptr;
    const wlr_box targetArea = usableArea(*m_server, output);
    const auto& animation = config().animation;
    const auto& scratchpad = animation.scratchpad;
    const bool animate = animateTransition && output != nullptr && animation.enabled && scratchpad.enabled;
    retargetBackdrop(output, visibleOn(output), animateTransition);

    for (const Entry& entry : m_entries) {
      if (entry.scratchpad != name || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      if (presented) {
        view->setOnActiveWorkspace(true);
        view->enterForeignOutput(output);
        std::erase(m_hidingViews, view);
        view->cancelPositionAnimation();
        view->setNodeEnabled(true);
        const int width = view->presentation().width();
        const int height = view->presentation().height();
        if (width > 0 && height > 0 && targetArea.width > 0 && targetArea.height > 0) {
          const int centerX = view->sceneTree()->node.x + width / 2;
          const int centerY = view->sceneTree()->node.y + height / 2;
          const bool centerOnTarget = centerX >= targetArea.x
              && centerX < targetArea.x + targetArea.width
              && centerY >= targetArea.y
              && centerY < targetArea.y + targetArea.height;
          if (!centerOnTarget) {
            view->snapPosition(
                targetArea.x + std::max(0, (targetArea.width - width) / 2),
                targetArea.y + std::max(0, (targetArea.height - height) / 2)
            );
          }
        }
        if (animate) {
          view->animateFadeTo(1.0F, scratchpad.durationMs, scratchpad.curve);
        } else {
          view->cancelFadeAnimation();
          view->setFadeAlpha(1.0F);
        }
        syncViewPresentation(view, true);
      } else {
        view->setOnActiveWorkspace(false);
        // Hidden scratchpad windows remain assigned to their pad's output for
        // foreign-toplevel consumers, just like windows on inactive
        // workspaces. Moving the pad to no output still clears that
        // membership through moveScratchpad().
        view->enterForeignOutput(output);
        if (animate) {
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
    updateDimAndBlur(output);
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
    state->output = output;
    state->usableArea = output != nullptr && targetArea.width > 0 && targetArea.height > 0
        ? std::optional<wlr_box>{targetArea}
        : std::nullopt;
    if (clearDisplacement) {
      state->displacedOutput.clear();
    }

    for (Entry& entry : m_entries) {
      if (entry.scratchpad != name || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      if (clearDisplacement) {
        entry.displacedPosition.reset();
      }
      if (previous != nullptr
          && output != nullptr
          && previousArea.width > 0
          && previousArea.height > 0
          && targetArea.width > 0
          && targetArea.height > 0) {
        remapViewRestoreGeometry(view, previousArea, targetArea);
        if (view != alreadyPositioned) {
          const double xFraction = static_cast<double>(view->sceneTree()->node.x - previousArea.x) / previousArea.width;
          const double yFraction =
              static_cast<double>(view->sceneTree()->node.y - previousArea.y) / previousArea.height;
          const int newX = targetArea.x + static_cast<int>(std::lround(xFraction * targetArea.width));
          const int newY = targetArea.y + static_cast<int>(std::lround(yFraction * targetArea.height));
          view->cancelPositionAnimation();
          view->setPosition(
              std::clamp(
                  newX, targetArea.x, targetArea.x + std::max(0, targetArea.width - view->toplevel()->current.width)
              ),
              std::clamp(
                  newY, targetArea.y, targetArea.y + std::max(0, targetArea.height - view->toplevel()->current.height)
              )
          );
        }
      }

      if (state->visible && output != nullptr) {
        wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
        view->reparentShadow(m_shadowRoot);
        view->setOnActiveWorkspace(true);
        std::erase(m_hidingViews, view);
        view->setNodeEnabled(true);
        if (view != alreadyPositioned) {
          syncViewPresentation(view, true);
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

    if (previous != nullptr) {
      retargetBackdrop(previous, visibleOn(previous));
      updateDimAndBlur(previous);
    }
    if (output != nullptr) {
      retargetBackdrop(output, visibleOn(output));
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

  void ScratchpadManager::remapViewRestoreGeometry(View* view, const wlr_box& previousArea, const wlr_box& targetArea) {
    if (view == nullptr
        || previousArea.width <= 0
        || previousArea.height <= 0
        || targetArea.width <= 0
        || targetArea.height <= 0) {
      return;
    }
    const auto remap = [&](wlr_box& box) {
      const double xFraction = static_cast<double>(box.x - previousArea.x) / previousArea.width;
      const double yFraction = static_cast<double>(box.y - previousArea.y) / previousArea.height;
      const int newX = targetArea.x + static_cast<int>(std::lround(xFraction * targetArea.width));
      const int newY = targetArea.y + static_cast<int>(std::lround(yFraction * targetArea.height));
      box.x = std::clamp(newX, targetArea.x, targetArea.x + std::max(0, targetArea.width - box.width));
      box.y = std::clamp(newY, targetArea.y, targetArea.y + std::max(0, targetArea.height - box.height));
    };
    if (view->m_hasMaximizeRestoreBox) {
      remap(view->m_maximizeRestoreBox);
    }
    if (view->m_hasFullscreenRestoreBox) {
      remap(view->m_fullscreenRestoreBox);
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
        remapViewRestoreGeometry(view, previousArea, targetArea);
        const double xFraction = static_cast<double>(view->sceneTree()->node.x - previousArea.x) / previousArea.width;
        const double yFraction = static_cast<double>(view->sceneTree()->node.y - previousArea.y) / previousArea.height;
        const int newX = targetArea.x + static_cast<int>(std::lround(xFraction * targetArea.width));
        const int newY = targetArea.y + static_cast<int>(std::lround(yFraction * targetArea.height));
        const int width = view->toplevel()->current.width;
        const int height = view->toplevel()->current.height;
        view->cancelPositionAnimation();
        view->setPosition(
            std::clamp(newX, targetArea.x, targetArea.x + std::max(0, targetArea.width - width)),
            std::clamp(newY, targetArea.y, targetArea.y + std::max(0, targetArea.height - height))
        );
        syncViewPresentation(view, true);
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
    const bool visible = visibleOn(output);
    const double dim = config().animation.scratchpad.dim;
    const bool blurEnabled = config().animation.scratchpad.blur && config().appearance.blur.enabled;
    const auto fade = m_backdropFades.find(output);
    const float currentAlpha = fade != m_backdropFades.end() ? static_cast<float>(fade->second.current()) : 0.0F;

    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &box);

    if (wlr_scene_rect* rect = dimRectFor(output)) {
      if ((!visible && currentAlpha <= 0.001F) || dim <= 0.0 || currentAlpha <= 0.001F) {
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
      if ((!visible && currentAlpha <= 0.001F) || !blurEnabled || currentAlpha <= 0.001F) {
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

  void ScratchpadManager::releaseOutput(Output* output) {
    std::vector<std::string> stranded;
    for (const auto& [name, scratchpad] : m_scratchpads) {
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
        fade.snap(visibleOn(output) ? 1.0 : 0.0);
        updateDimAndBlur(output);
      }
      for (const Entry& entry : m_entries) {
        if (entry.view == nullptr) {
          continue;
        }
        const Scratchpad* scratchpad = findScratchpad(entry.scratchpad);
        const bool visible = scratchpad != nullptr && scratchpad->visible && scratchpad->output != nullptr;
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

  bool ScratchpadManager::toggle(std::string_view name, Output* invokingOutput) {
    Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || invokingOutput == nullptr || !hasEntries(name)) {
      return false;
    }
    if (scratchpad->visible && scratchpad->output == invokingOutput) {
      setVisible(name, false);
      m_server->refocus(invokingOutput);
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
    if (View* view = focused(name)) {
      m_server->focusView(view);
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
      if (remembered != nullptr && remembered->scratchpad == name && scratchpad->lastFocused->mapped()) {
        return scratchpad->lastFocused;
      }
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

  void ScratchpadManager::noteFocus(View* view) {
    m_focusedView = nullptr;
    Entry* entry = findEntry(view);
    if (entry == nullptr) {
      return;
    }
    Scratchpad* scratchpad = findScratchpad(entry->scratchpad);
    if (scratchpad == nullptr) {
      return;
    }
    m_focusedView = view;
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
    restorePresentation(view);
  }

  void ScratchpadManager::restorePresentation(View* view) {
    const Entry* entry = findEntry(view);
    const Scratchpad* scratchpad = entry != nullptr ? findScratchpad(entry->scratchpad) : nullptr;
    if (view == nullptr || entry == nullptr || scratchpad == nullptr || scratchpad->output == nullptr) {
      return;
    }
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setOnActiveWorkspace(scratchpad->visible);
    view->enterForeignOutput(scratchpad->output);
    view->setNodeEnabled(scratchpad->visible);
    syncViewPresentation(view);
  }

  void ScratchpadManager::syncViewPresentation(View* view, bool refreshMaximized) {
    const Entry* entry = findEntry(view);
    if (view == nullptr || entry == nullptr || !view->mapped()) {
      return;
    }
    if (Cursor* cursor = m_server->cursor(); cursor != nullptr && cursor->isDraggingView(view)) {
      return;
    }
    if (view->toplevel()->scheduled.fullscreen) {
      view->applyFullscreenLayout();
      return;
    }
    const Scratchpad* scratchpad = findScratchpad(entry->scratchpad);
    if (refreshMaximized
        && scratchpad != nullptr
        && scratchpad->output != nullptr
        && (view->maximizedToEdges() || view->m_floatingMaximized)) {
      const wlr_box area = usableArea(*m_server, scratchpad->output);
      if (area.width > 0 && area.height > 0) {
        view->cancelPositionAnimation();
        view->setPosition(area.x, area.y);
        wlr_xdg_toplevel_set_size(view->toplevel(), area.width, area.height);
        view->applyPresentation(area);
      }
      return;
    }
    const wlr_box& geometry = view->toplevel()->base->geometry;
    if (geometry.width <= 0 || geometry.height <= 0) {
      return;
    }
    view->applyPresentation({
        .x = view->sceneTree()->node.x,
        .y = view->sceneTree()->node.y,
        .width = geometry.width,
        .height = geometry.height,
    });
  }

  bool ScratchpadManager::focusNext(std::string_view name) {
    const Scratchpad* scratchpad = findScratchpad(name);
    if (scratchpad == nullptr || !scratchpad->visible || scratchpad->output == nullptr) {
      return false;
    }
    std::vector<View*> views;
    for (const Entry& entry : m_entries) {
      if (entry.scratchpad == name && entry.view != nullptr && entry.view->mapped()) {
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

  bool ScratchpadManager::restoreView(View* view, Output* fallback, bool focus) {
    const auto iterator =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (iterator == m_entries.end()) {
      return false;
    }
    Entry entry = std::move(*iterator);
    Scratchpad* scratchpad = findScratchpad(entry.scratchpad);
    Output* scratchpadOutput = scratchpad != nullptr ? scratchpad->output : fallback;
    if (scratchpad != nullptr && scratchpad->lastFocused == view) {
      scratchpad->lastFocused = nullptr;
    }
    m_entries.erase(iterator);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);

    view->reparentShadow(nullptr);
    view->setInScratchpad(false);
    Output* restoreOutput = m_server->outputFromName(entry.returnOutput);
    if (restoreOutput == nullptr) {
      restoreOutput = scratchpadOutput;
    }
    Workspace* workspace = restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr
        ? restoreOutput->workspaceGroup()->workspaceNamed(entry.returnWorkspace)
        : nullptr;
    if (workspace == nullptr && restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr) {
      workspace = restoreOutput->workspaceGroup()->active();
    }
    view->moveToWorkspace(workspace, false);
    if (entry.returnTiled) {
      view->setFloating(false);
    } else {
      view->setFloating(true);
      if (restoreOutput != nullptr && restoreOutput != scratchpadOutput) {
        const wlr_box area = restoreOutput->usableArea();
        if (area.width > 0 && area.height > 0) {
          const int width = view->toplevel()->current.width;
          const int height = view->toplevel()->current.height;
          view->setPosition(
              std::clamp(view->sceneTree()->node.x, area.x, area.x + std::max(0, area.width - width)),
              std::clamp(view->sceneTree()->node.y, area.y, area.y + std::max(0, area.height - height))
          );
        }
      }
    }
    if (workspace != nullptr) {
      workspace->syncViewPresentation(view);
    }

    if (scratchpad != nullptr && !hasEntries(entry.scratchpad) && scratchpad->visible) {
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

  void ScratchpadManager::remove(View* view) {
    const auto iterator =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (iterator == m_entries.end()) {
      return;
    }
    const std::string name = iterator->scratchpad;
    Scratchpad* scratchpad = findScratchpad(name);
    view->reparentShadow(nullptr);
    view->setInScratchpad(false);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    if (scratchpad != nullptr && scratchpad->lastFocused == view) {
      scratchpad->lastFocused = nullptr;
    }
    std::erase(m_hidingViews, view);
    m_entries.erase(iterator);
    if (scratchpad != nullptr && !hasEntries(name) && scratchpad->visible) {
      setVisible(name, false);
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
              static_cast<double>(entry.view->sceneTree()->node.x - homeArea.x) / homeArea.width,
              static_cast<double>(entry.view->sceneTree()->node.y - homeArea.y) / homeArea.height,
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
    bool restoredAny = false;
    for (const std::string& name : removed) {
      Scratchpad* scratchpad = findScratchpad(name);
      Output* fallback = scratchpad != nullptr ? scratchpad->output : nullptr;
      if (scratchpad != nullptr && scratchpad->visible) {
        setVisible(name, false, false);
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
        restoredAny = restoreView(view, fallback, false) || restoredAny;
      }
      m_scratchpads.erase(name);
    }
    for (const std::string& name : desired) {
      m_scratchpads.try_emplace(name);
    }
    if (restoredAny) {
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
          const bool managerOwnedGeometry = entry.view->toplevel()->scheduled.fullscreen
              || entry.view->toplevel()->current.fullscreen
              || entry.view->maximizedToEdges()
              || entry.view->m_floatingMaximized;
          if (entry.displacedPosition && !managerOwnedGeometry && homeArea.width > 0 && homeArea.height > 0) {
            entry.view->cancelPositionAnimation();
            entry.view->setPosition(
                homeArea.x + static_cast<int>(std::lround((*entry.displacedPosition)[0] * homeArea.width)),
                homeArea.y + static_cast<int>(std::lround((*entry.displacedPosition)[1] * homeArea.height))
            );
            if (scratchpad->visible) {
              restorePresentation(entry.view);
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
