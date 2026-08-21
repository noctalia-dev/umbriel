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
      : m_server(&server), m_root(root), m_shadowRoot(shadowRoot) {}

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
    if (entry.returnTiled) {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      view->setFloating(true);
      view->cancelPositionAnimation();
      view->setPosition(x, y);
    }
    if (sourceOutput != nullptr && sourceOutput != output) {
      auto usableArea = [this](Output* candidate) {
        wlr_box area = candidate->usableArea();
        if (area.width <= 0 || area.height <= 0) {
          wlr_output_layout_get_box(m_server->outputLayout(), candidate->wlr(), &area);
        }
        return area;
      };
      const wlr_box sourceArea = usableArea(sourceOutput);
      const wlr_box targetArea = usableArea(output);
      if (sourceArea.width > 0 && sourceArea.height > 0 && targetArea.width > 0 && targetArea.height > 0) {
        const auto& scheduled = view->toplevel()->scheduled;
        const int width = std::max(1, scheduled.width);
        const int height = std::max(1, scheduled.height);
        const double scale = std::min(
            static_cast<double>(targetArea.width) / static_cast<double>(sourceArea.width),
            static_cast<double>(targetArea.height) / static_cast<double>(sourceArea.height)
        );
        const int scaledWidth = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * scale)));
        const int scaledHeight = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * scale)));
        if (scaledWidth != width || scaledHeight != height) {
          wlr_xdg_toplevel_set_size(view->toplevel(), scaledWidth, scaledHeight);
        }

        const double xFraction =
            static_cast<double>(view->sceneTree()->node.x - sourceArea.x) / static_cast<double>(sourceArea.width);
        const double yFraction =
            static_cast<double>(view->sceneTree()->node.y - sourceArea.y) / static_cast<double>(sourceArea.height);
        const int x = targetArea.x + static_cast<int>(std::lround(xFraction * static_cast<double>(targetArea.width)));
        const int y = targetArea.y + static_cast<int>(std::lround(yFraction * static_cast<double>(targetArea.height)));
        view->cancelPositionAnimation();
        view->setPosition(
            std::clamp(x, targetArea.x, targetArea.x + std::max(0, targetArea.width - scaledWidth)),
            std::clamp(y, targetArea.y, targetArea.y + std::max(0, targetArea.height - scaledHeight))
        );
      }
    }
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
    for (const Entry& entry : m_entries) {
      if (entry.output == output && entry.view != nullptr) {
        entry.view->setOnActiveWorkspace(visible);
        entry.view->setNodeEnabled(visible);
      }
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
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
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
