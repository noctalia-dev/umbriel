#include "workspace/scratchpad.h"

#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace umbriel {

  ScratchpadManager::ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot)
      : m_server(&server), m_root(root), m_shadowRoot(shadowRoot) {
    m_server->registerAnimatable(this);
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
    return std::ranges::any_of(m_hidingViews, [this, output](const View* view) {
      return std::ranges::any_of(m_entries, [view, output](const Entry& entry) {
        return entry.view == view && entry.output == output;
      });
    });
  }

  bool ScratchpadManager::contains(const View* view) const {
    return std::ranges::any_of(m_entries, [view](const Entry& entry) { return entry.view == view; });
  }

  bool ScratchpadManager::moveToScratchpad(View* view, Output* output) {
    if (output == nullptr || m_server == nullptr || m_root == nullptr || m_shadowRoot == nullptr) {
      return false;
    }
    if (view == nullptr || !view->mapped() || contains(view)) {
      return false;
    }

    Entry entry{
        .view = view,
        .output = output,
        .returnOutput = {},
        .displacedOutput = {},
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
      view->togglePinned(); // unpins and returns to floating/tiled state
    }
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false);
    }
    view->setFloating(true);
    view->cancelPositionAnimation();

    wlr_box targetArea = output->usableArea();
    if (targetArea.width <= 0 || targetArea.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &targetArea);
    }
    const auto& spCfg = config().animation.scratchpad;
    if (spCfg.fullscreen) {
      if (!view->toplevel()->scheduled.fullscreen && !view->toplevel()->current.fullscreen) {
        view->toggleFullscreen();
      }
    } else if (spCfg.maximize) {
      view->toggleMaximizedToEdges();
    } else if (spCfg.scale > 0.0 && spCfg.scale <= 1.0 && targetArea.width > 0 && targetArea.height > 0) {
      const int targetW = std::max(100, static_cast<int>(std::lround(targetArea.width * spCfg.scale)));
      const int targetH = std::max(100, static_cast<int>(std::lround(targetArea.height * spCfg.scale)));
      wlr_xdg_toplevel_set_size(view->toplevel(), targetW, targetH);
      const int newX = targetArea.x + std::max(0, (targetArea.width - targetW) / 2);
      const int newY = targetArea.y + std::max(0, (targetArea.height - targetH) / 2);
      view->setPosition(newX, newY);
    } else {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      view->setPosition(x, y);
    }

    view->moveToWorkspace(nullptr);
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setScratchpadBorder(true);
    const bool wasVisible = std::ranges::find(m_visibleOutputs, output) != m_visibleOutputs.end();
    m_entries.push_back(std::move(entry));
    setVisible(output, wasVisible);
    m_server->refocus(sourceOutput);
    return true;
  }

  void ScratchpadManager::setVisible(Output* output, bool visible) {
    if (output == nullptr) {
      return;
    }
    if (visible) {
      if (std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
        m_visibleOutputs.push_back(output);
      }
    } else {
      std::erase(m_visibleOutputs, output);
    }
    wlr_box targetArea = output->usableArea();
    if (targetArea.width <= 0 || targetArea.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &targetArea);
    }
    const auto& animation = config().animation;
    const auto& scratchpad = animation.scratchpad;
    auto fadeIt = m_backdropFades.try_emplace(output, 0.0).first;
    AnimatedValue& backdropFade = fadeIt->second;
    const double fadeTarget = visible ? 1.0 : 0.0;
    if (animation.enabled && scratchpad.enabled && (backdropFade.animating() || backdropFade.current() != fadeTarget)) {
      backdropFade.retarget(fadeTarget, scratchpad.durationMs, scratchpad.curve);
    } else {
      backdropFade.snap(fadeTarget);
    }

    for (const Entry& entry : m_entries) {
      if (entry.output != output || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      if (visible) {
        view->setOnActiveWorkspace(true);
        std::erase(m_hidingViews, view);
        view->cancelPositionAnimation();
        view->setNodeEnabled(true);
        // Reposition only if the window's center would land off this output's usable area
        const int width = view->presentation().width();
        const int height = view->presentation().height();
        if (width > 0 && height > 0) {
          const int centerX = view->sceneTree()->node.x + width / 2;
          const int centerY = view->sceneTree()->node.y + height / 2;
          const bool centerOnTarget = centerX >= targetArea.x
              && centerX < targetArea.x + targetArea.width
              && centerY >= targetArea.y
              && centerY < targetArea.y + targetArea.height;
          if (!centerOnTarget) {
            const int newX = targetArea.x + std::max(0, (targetArea.width - width) / 2);
            const int newY = targetArea.y + std::max(0, (targetArea.height - height) / 2);
            view->snapPosition(newX, newY);
          }
        }
        if (animation.enabled && scratchpad.enabled) {
          view->animateFadeTo(1.0F, scratchpad.durationMs, scratchpad.curve);
        } else {
          view->setFadeAlpha(1.0F);
        }
      } else {
        view->setOnActiveWorkspace(false);
        if (animation.enabled && scratchpad.enabled) {
          // Keep the render tree alive until tickAnimations observes the completed fade. The inactive-workspace flag
          // already removes this view from focus and action selection while it is still visible.
          view->setNodeEnabled(true);
          view->animateFadeTo(0.0F, scratchpad.durationMs, scratchpad.curve);
          if (std::ranges::find(m_hidingViews, view) == m_hidingViews.end()) {
            m_hidingViews.push_back(view);
          }
        } else {
          view->setFadeAlpha(0.0F);
          view->setNodeEnabled(false);
        }
      }
    }
    updateDimAndBlur(output);
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
    const bool visible = std::ranges::find(m_visibleOutputs, output) != m_visibleOutputs.end();
    const double dim = config().animation.scratchpad.dim;
    const bool blurEnabled = config().animation.scratchpad.blur && config().appearance.blur.enabled;
    const auto fade = m_backdropFades.find(output);
    const float curAlpha = fade != m_backdropFades.end() ? static_cast<float>(fade->second.current()) : 0.0F;

    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &box);

    if (wlr_scene_rect* rect = dimRectFor(output)) {
      if ((!visible && curAlpha <= 0.001F) || dim <= 0.0 || curAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&rect->node, false);
      } else {
        wlr_scene_node_set_position(&rect->node, box.x, box.y);
        wlr_scene_rect_set_size(rect, box.width, box.height);
        const float color[4] = {0.0F, 0.0F, 0.0F, std::clamp(static_cast<float>(dim * curAlpha), 0.0F, 1.0F)};
        wlr_scene_rect_set_color(rect, color);
        wlr_scene_node_set_enabled(&rect->node, true);
      }
    }

    if (wlr_scene_blur* blur = blurNodeFor(output)) {
      if ((!visible && curAlpha <= 0.001F) || !blurEnabled || curAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&blur->node, false);
      } else {
        wlr_scene_node_set_position(&blur->node, box.x, box.y);
        wlr_scene_blur_set_size(blur, box.width, box.height);
        wlr_scene_blur_set_corner_radius(blur, 0);
        wlr_scene_blur_set_alpha(blur, std::clamp(curAlpha, 0.0F, 1.0F));
        wlr_scene_node_set_enabled(&blur->node, true);
      }
    }
  }

  void ScratchpadManager::releaseOutput(Output* output) {
    std::erase(m_visibleOutputs, output);
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
        const bool visible = std::ranges::find(m_visibleOutputs, output) != m_visibleOutputs.end();
        fade.snap(visible ? 1.0 : 0.0);
        updateDimAndBlur(output);
      }
      for (const Entry& entry : m_entries) {
        if (entry.view == nullptr) {
          continue;
        }
        const bool visible = std::ranges::find(m_visibleOutputs, entry.output) != m_visibleOutputs.end();
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

  bool ScratchpadManager::toggle(Output* output) {
    if (output == nullptr
        || std::ranges::none_of(m_entries, [output](const Entry& entry) { return entry.output == output; })) {
      return false;
    }
    const bool show = std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end();
    setVisible(output, show);
    if (show) {
      if (View* view = focused(output)) {
        m_server->focusView(view);
      }
    } else {
      m_server->refocus(output);
    }
    return true;
  }

  View* ScratchpadManager::focused(Output* output) const {
    if (std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
      return nullptr;
    }
    const auto remembered = std::ranges::find_if(m_entries, [output](const Entry& entry) {
      return entry.output == output && entry.lastFocused;
    });
    if (remembered != m_entries.end()) {
      return remembered->view;
    }
    for (const Entry& entry : m_entries) {
      if (entry.output == output) {
        return entry.view;
      }
    }
    return nullptr;
  }

  bool ScratchpadManager::hasFocus(Output* output) const {
    if (m_focusedView == nullptr || std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
      return false;
    }
    return std::ranges::any_of(m_entries, [this, output](const Entry& entry) {
      return entry.view == m_focusedView && entry.output == output;
    });
  }

  void ScratchpadManager::noteFocus(View* view) {
    m_focusedView = nullptr;
    const auto focused = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (focused == m_entries.end()) {
      return;
    }

    m_focusedView = view;
    for (Entry& entry : m_entries) {
      if (entry.output == focused->output) {
        entry.lastFocused = entry.view == view;
      }
    }
  }

  void ScratchpadManager::finishMove(View* view, Output* output) {
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it == m_entries.end() || view == nullptr) {
      return;
    }
    Output* previous = it->output;
    if (output != nullptr) {
      it->output = output;
      if (std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
        m_visibleOutputs.push_back(output);
      }
    }
    if (previous != it->output && it->lastFocused) {
      for (Entry& entry : m_entries) {
        if (&entry != &*it && entry.output == it->output) {
          entry.lastFocused = false;
        }
      }
    }
    if (previous != it->output
        && std::ranges::none_of(m_entries, [previous](const Entry& entry) { return entry.output == previous; })) {
      std::erase(m_visibleOutputs, previous);
    }
    restorePresentation(view);
  }

  void ScratchpadManager::restorePresentation(View* view) {
    if (view == nullptr || !contains(view)) {
      return;
    }
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setOnActiveWorkspace(true);
    view->setNodeEnabled(true);
  }

  bool ScratchpadManager::focusNext(Output* output) {
    if (output == nullptr || std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
      return false;
    }
    std::vector<View*> views;
    for (const Entry& entry : m_entries) {
      if (entry.output == output && entry.view != nullptr && entry.view->mapped()) {
        views.push_back(entry.view);
      }
    }
    if (views.empty()) {
      return false;
    }
    View* current = focused(output);
    const auto it = std::ranges::find(views, current);
    View* target = it == views.end() || std::next(it) == views.end() ? views.front() : *std::next(it);
    m_server->focusView(target, FocusReason::Directional);
    return true;
  }

  bool ScratchpadManager::restoreFocused(Output* output) {
    View* view = focused(output);
    if (view == nullptr) {
      return false;
    }
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it == m_entries.end()) {
      return false;
    }
    Entry entry = std::move(*it);
    m_entries.erase(it);
    if (std::ranges::none_of(m_entries, [output](const Entry& e) { return e.output == output; })) {
      std::erase(m_visibleOutputs, output);
    }
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);
    Output* restoreOutput = m_server->outputFromName(entry.returnOutput);
    if (restoreOutput == nullptr) {
      restoreOutput = output;
    }
    Workspace* workspace = restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr
        ? restoreOutput->workspaceGroup()->workspaceNamed(entry.returnWorkspace)
        : nullptr;
    if (workspace == nullptr && restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr) {
      workspace = restoreOutput->workspaceGroup()->active();
    }
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    view->moveToWorkspace(workspace, false);
    if (entry.returnTiled) {
      view->setFloating(false);
    } else {
      view->setFloating(true);
      if (restoreOutput != nullptr && restoreOutput != output) {
        const wlr_box usable = restoreOutput->usableArea();
        if (usable.width > 0 && usable.height > 0) {
          const int w = view->toplevel()->current.width;
          const int h = view->toplevel()->current.height;
          view->setPosition(
              std::clamp(view->sceneTree()->node.x, usable.x, usable.x + std::max(0, usable.width - w)),
              std::clamp(view->sceneTree()->node.y, usable.y, usable.y + std::max(0, usable.height - h))
          );
        }
      }
    }
    if (workspace != nullptr) {
      workspace->syncViewPresentation(view);
    }
    m_server->focusView(view);
    return true;
  }

  void ScratchpadManager::remove(View* view) {
    const auto entry =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (entry == m_entries.end()) {
      return;
    }
    Output* entryOutput = entry->output;
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);
    m_entries.erase(entry);
    if (entryOutput != nullptr
        && std::ranges::none_of(m_entries, [entryOutput](const Entry& e) { return e.output == entryOutput; })) {
      std::erase(m_visibleOutputs, entryOutput);
    }
  }

  void ScratchpadManager::moveOutput(Output* from, Output* to) {
    if (from == to) {
      return;
    }
    const bool movedRemembered = std::ranges::any_of(m_entries, [from](const Entry& entry) {
      return entry.output == from && entry.lastFocused;
    });
    if (movedRemembered && to != nullptr) {
      for (Entry& entry : m_entries) {
        if (entry.output == to) {
          entry.lastFocused = false;
        }
      }
    }
    const bool wasVisible = std::ranges::find(m_visibleOutputs, from) != m_visibleOutputs.end();
    if (wasVisible) {
      setVisible(from, false);
    }
    for (Entry& entry : m_entries) {
      if (entry.output == from) {
        if (entry.displacedOutput.empty() && from != nullptr && from->wlr()->name != nullptr) {
          entry.displacedOutput = from->wlr()->name;
        }
        entry.output = to;
        if (from != nullptr && to != nullptr && entry.view != nullptr) {
          const wlr_box srcArea = from->usableArea();
          const wlr_box dstArea = to->usableArea();
          if (srcArea.width > 0 && srcArea.height > 0 && dstArea.width > 0 && dstArea.height > 0) {
            const double xFrac = static_cast<double>(entry.view->sceneTree()->node.x - srcArea.x) / srcArea.width;
            const double yFrac = static_cast<double>(entry.view->sceneTree()->node.y - srcArea.y) / srcArea.height;
            const int newX = dstArea.x + static_cast<int>(std::lround(xFrac * dstArea.width));
            const int newY = dstArea.y + static_cast<int>(std::lround(yFrac * dstArea.height));
            entry.view->cancelPositionAnimation();
            entry.view->setPosition(
                std::clamp(
                    newX, dstArea.x, dstArea.x + std::max(0, dstArea.width - entry.view->toplevel()->current.width)
                ),
                std::clamp(
                    newY, dstArea.y, dstArea.y + std::max(0, dstArea.height - entry.view->toplevel()->current.height)
                )
            );
          }
        }
      }
    }
    if (wasVisible && to != nullptr) {
      setVisible(to, true);
    }
  }

  size_t ScratchpadManager::restoreDisplaced(Output* fallback) {
    if (fallback == nullptr || m_server == nullptr) {
      return 0;
    }
    size_t restored = 0;
    for (Entry& entry : m_entries) {
      Output* home = entry.displacedOutput.empty() ? nullptr : m_server->outputFromName(entry.displacedOutput);
      if (home != nullptr) {
        entry.displacedOutput.clear();
      }
      Output* target = home != nullptr ? home : (entry.output == nullptr ? fallback : nullptr);
      if (target == nullptr || target == entry.output) {
        continue;
      }
      entry.output = target;
      if (entry.view != nullptr) {
        const bool visible = std::ranges::find(m_visibleOutputs, target) != m_visibleOutputs.end();
        entry.view->setOnActiveWorkspace(visible);
        entry.view->setNodeEnabled(visible);
      }
      ++restored;
    }
    return restored;
  }

} // namespace umbriel
