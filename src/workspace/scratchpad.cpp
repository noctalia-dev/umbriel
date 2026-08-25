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
  }

  bool ScratchpadManager::tickAnimations(uint64_t /*nowMsec*/) {
    if (m_hidingViews.empty()) {
      return false;
    }
    std::erase_if(m_hidingViews, [](View* view) {
      if (view->presentedOpacity() > 0.002F) {
        return false;
      }
      view->setNodeEnabled(false);
      return true;
    });
    return true;
  }

  bool ScratchpadManager::animatesOn(const Output* output) const {
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
    // Geometry is left as-is, not resized or centered - a tiled window floats in place at its current position.
    const int x = view->sceneTree()->node.x;
    const int y = view->sceneTree()->node.y;
    view->setFloating(true);
    view->cancelPositionAnimation();
    view->setPosition(x, y);

    view->setWorkspace(nullptr);
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
    const auto& scAnim = config().appearance.animations.scratchpad;
    for (const Entry& entry : m_entries) {
      if (entry.output != output || entry.view == nullptr) {
        continue;
      }
      View* view = entry.view;
      view->setOnActiveWorkspace(visible);
      if (visible) {
        std::erase(m_hidingViews, view);
        view->cancelPositionAnimation();
        view->setNodeEnabled(true);
        // Reposition only if the window's center would land off this output's usable area; never resize.
        const int width = view->presentation().width();
        const int height = view->presentation().height();
        if (width > 0 && height > 0) {
          const int centerX = view->sceneTree()->node.x + width / 2;
          const int centerY = view->sceneTree()->node.y + height / 2;
          const bool centerOnTarget = centerX >= targetArea.x && centerX < targetArea.x + targetArea.width
              && centerY >= targetArea.y && centerY < targetArea.y + targetArea.height;
          if (!centerOnTarget) {
            const int newX = targetArea.x + std::max(0, (targetArea.width - width) / 2);
            const int newY = targetArea.y + std::max(0, (targetArea.height - height) / 2);
            view->snapPosition(newX, newY);
          }
        }
        if (scAnim.enabled && scAnim.durationMs > 0) {
          view->setFadeAlpha(0.0F);
          view->animateFadeTo(1.0F, scAnim.durationMs, scAnim.curve);
        } else {
          view->setFadeAlpha(1.0F);
        }
      } else {
        if (scAnim.enabled && scAnim.durationMs > 0) {
          view->animateFadeTo(0.0F, scAnim.durationMs, scAnim.curve);
          if (std::ranges::find(m_hidingViews, view) == m_hidingViews.end()) {
            m_hidingViews.push_back(view);
          }
        } else {
          view->setFadeAlpha(0.0F);
          view->setNodeEnabled(false);
        }
      }
    }
    updateDim(output);
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

  void ScratchpadManager::updateDim(Output* output) {
    if (output == nullptr) {
      return;
    }
    wlr_scene_rect* rect = dimRectFor(output);
    const bool visible = std::ranges::find(m_visibleOutputs, output) != m_visibleOutputs.end();
    const double dim = config().appearance.animations.scratchpad.dim;
    if (!visible || dim <= 0.0) {
      wlr_scene_node_set_enabled(&rect->node, false);
      return;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &box);
    wlr_scene_node_set_position(&rect->node, box.x, box.y);
    wlr_scene_rect_set_size(rect, box.width, box.height);
    const float color[4] = {0.0F, 0.0F, 0.0F, static_cast<float>(dim)};
    wlr_scene_rect_set_color(rect, color);
    wlr_scene_node_set_enabled(&rect->node, true);
  }

  void ScratchpadManager::releaseOutput(Output* output) {
    const auto it = m_dimRects.find(output);
    if (it == m_dimRects.end()) {
      return;
    }
    wlr_scene_node_destroy(&it->second->node);
    m_dimRects.erase(it);
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
    view->setWorkspace(workspace, false);
    if (entry.returnTiled) {
      view->setFloating(false);
    } else {
      view->setFloating(true);
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
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);
    m_entries.erase(entry);
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
        entry.output = to;
      }
    }
    if (wasVisible && to != nullptr) {
      setVisible(to, true);
    }
  }

} // namespace umbriel
