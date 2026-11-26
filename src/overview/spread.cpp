#include "config/config.h"
#include "input/cursor.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {
  namespace {
    SpreadBox spreadBox(const wlr_box& box) {
      return {
          static_cast<double>(box.x), static_cast<double>(box.y), static_cast<double>(box.width),
          static_cast<double>(box.height)
      };
    }

    constexpr int kCloseSize = 28;
  } // namespace

  void Overview::toggleSpread() {
    if (m_active && m_spread) {
      if (m_closing) {
        open();
      } else {
        close();
      }
      return;
    }
    if (m_active) {
      forceClose();
    }
    m_spread = true;
    if (!beginPresentation()) {
      m_spread = false;
      return;
    }
    startAnimation(1.0, false);
  }

  Output* Overview::spreadCardOutput(const Card& card) const {
    return config().overview.spread.allOutputs && m_spreadOutput != nullptr ? m_spreadOutput : card.owner->output;
  }

  SpreadBox Overview::spreadCardOrigin(const Card& card) const {
    wlr_box output{};
    wlr_output_layout_get_box(m_server->outputLayout(), card.owner->output->wlr(), &output);
    const WorkspaceGroup* group = card.owner->output->workspaceGroup();
    const double active = group != nullptr && group->active() != nullptr
        ? static_cast<double>(group->active()->index())
        : static_cast<double>(card.owner->activeWorkspaceIndex);
    return spreadOrigin(
        spreadBox(card.view->presentedBox()), spreadBox(output),
        card.view->pinned() ? 0.0 : static_cast<double>(card.workspaceIndex) - active,
        group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal
    );
  }

  void Overview::refreshSpreadLayout() {
    m_spreadLayoutDirty = false;
    m_spreadOrder.clear();
    std::vector<SpreadBox> outputs;
    outputs.reserve(m_outputs.size());
    size_t invoking = m_outputs.size();
    for (const auto& state : m_outputs) {
      if (state->output == m_spreadOutput) {
        invoking = outputs.size();
      }
      PreviewMetrics metrics;
      outputs.push_back(previewMetrics(*state, *m_server, 1.0, metrics) ? spreadBox(metrics.usableBox) : SpreadBox{});
    }
    std::vector<Card*> cards;
    std::vector<SpreadBox> origins;
    std::vector<size_t> owners;
    const auto& settings = config().overview.spread;
    for (size_t output = 0; output < m_outputs.size(); ++output) {
      const auto& state = m_outputs[output];
      for (const auto& card : state->cards) {
        card->spreadIncluded =
            settings.allWorkspaces || card->view->pinned() || card->workspaceIndex == state->activeWorkspaceIndex;
        if (!card->spreadIncluded) {
          continue;
        }
        cards.push_back(card.get());
        origins.push_back(spreadCardOrigin(*card));
        owners.push_back(output);
      }
    }
    const SpreadLayout layout = spreadLayoutOnOutputs(
        outputs, origins, owners, invoking, settings.allOutputs,
        {.spacing = settings.spacing,
         .maxScale = settings.maxScale,
         .caption = settings.showTitles || settings.showWorkspaceNumbers}
    );
    for (size_t index = 0; index < cards.size() && index < layout.cells.size(); ++index) {
      cards[index]->spreadCellFrom = cards[index]->spreadCell.width > 0
          ? spreadInterpolate(cards[index]->spreadCellFrom, cards[index]->spreadCell, m_spreadReflow.current())
          : layout.cells[index];
      cards[index]->spreadCell = layout.cells[index];
    }
    m_spreadReflow.snap(1.0);
    if (m_progress >= 1.0 && !m_closing && config().animation.enabled && config().animation.overview.enabled) {
      m_spreadReflow.snap(0.0);
      m_spreadReflow.retarget(1.0, config().animation.overview.durationMs, config().animation.overview.curve);
      scheduleFrames();
    }
    for (const size_t index : layout.order) {
      m_spreadOrder.push_back(cards[index]);
    }
    m_spreadColumns = layout.columns;
    if (std::ranges::none_of(cards, [this](const Card* card) { return card->view == m_spreadSelected; })) {
      m_spreadSelected = m_spreadOrder.empty() ? nullptr : m_spreadOrder.front()->view;
    }
  }

  void Overview::layoutSpreadControls(Card& card) {
    if (m_spreadOutput == nullptr) {
      return;
    }
    if (!card.spreadIncluded) {
      if (card.spreadControls != nullptr) {
        wlr_scene_node_set_enabled(&card.spreadControls->node, false);
      }
      return;
    }
    const auto& settings = config().overview.spread;
    const double scale = std::max(1.0, std::ceil(static_cast<double>(spreadCardOutput(card)->wlr()->scale)));
    if (card.spreadControls == nullptr) {
      card.spreadControls = wlr_scene_tree_create(card.tree);
      if (card.spreadControls == nullptr) {
        return;
      }
      const float background[4] = {0.08F, 0.08F, 0.08F, 0.95F};
      card.closeBackground = wlr_scene_rect_create(card.spreadControls, kCloseSize, kCloseSize, background);
      if (card.closeBackground != nullptr) {
        wlr_scene_rect_set_corner_radius(card.closeBackground, kCloseSize / 2);
      }
      TextBufferResult label = renderTextBuffer({
          .markup = "<span foreground='#ffffff'>×</span>",
          .font = "sans 16",
          .maxWidth = kCloseSize,
          .padding = 0,
          .scale = scale,
          .bgA = 0.0,
      });
      if (label.buffer != nullptr) {
        card.closeLabel = wlr_scene_buffer_create(card.spreadControls, label.buffer);
        wlr_buffer_drop(label.buffer);
        if (card.closeLabel != nullptr) {
          wlr_scene_buffer_set_dest_size(card.closeLabel, label.logicalWidth, label.logicalHeight);
        }
      }
    }

    const char* title = card.view->title();
    std::string caption = settings.showTitles ? (title != nullptr ? title : "Window") : "";
    if (settings.showWorkspaceNumbers) {
      caption += std::format("{}{}", caption.empty() ? "" : "  ·  ", card.workspaceIndex + 1);
    }
    const int maxWidth = std::max(1, static_cast<int>(std::lround(card.spreadCell.width)));
    if (caption != card.captionTitle || maxWidth != card.captionMaxWidth) {
      card.captionTitle = caption;
      card.captionMaxWidth = maxWidth;
      if (card.caption != nullptr) {
        wlr_scene_node_destroy(&card.caption->node);
        card.caption = nullptr;
      }
      TextBufferResult rendered = renderTextBuffer({
          .markup = std::format(
              "<span foreground='{}'>{}</span>", rgbaHex(config().colors.textPrimary), escapeMarkup(caption)
          ),
          .font = "sans 11",
          .maxWidth = maxWidth,
          .padding = 0,
          .scale = scale,
          .ellipsize = true,
          .bgA = 0.0,
      });
      if (rendered.buffer != nullptr) {
        card.caption = wlr_scene_buffer_create(card.spreadControls, rendered.buffer);
        wlr_buffer_drop(rendered.buffer);
        if (card.caption != nullptr) {
          wlr_scene_buffer_set_dest_size(card.caption, rendered.logicalWidth, rendered.logicalHeight);
        }
      }
    }
    const float alpha = static_cast<float>(std::clamp((m_progress - 0.55) / 0.45, 0.0, 1.0));
    wlr_scene_node_set_enabled(&card.spreadControls->node, alpha > 0.001F && !m_closing);
    wlr_scene_node_raise_to_top(&card.spreadControls->node);
    const int closeX = card.box.width - kCloseSize / 2;
    const int closeY = -kCloseSize / 2;
    card.closeBox = {card.box.x + closeX, card.box.y + closeY, kCloseSize, kCloseSize};
    if (card.closeBackground != nullptr) {
      wlr_scene_node_set_enabled(&card.closeBackground->node, settings.showCloseButtons);
      std::array<float, 4> background{};
      premultiplied(
          background.data(),
          card.view == m_spreadSelected ? std::array<float, 4>{0.7F, 0.18F, 0.18F, 0.95F}
                                        : std::array<float, 4>{0.08F, 0.08F, 0.08F, 0.95F},
          alpha
      );
      wlr_scene_rect_set_color(card.closeBackground, background.data());
      wlr_scene_node_set_position(&card.closeBackground->node, closeX, closeY);
    }
    if (card.closeLabel != nullptr) {
      wlr_scene_node_set_enabled(&card.closeLabel->node, settings.showCloseButtons);
      wlr_scene_node_set_position(
          &card.closeLabel->node, closeX + (kCloseSize - card.closeLabel->dst_width) / 2,
          closeY + (kCloseSize - card.closeLabel->dst_height) / 2
      );
      wlr_scene_buffer_set_opacity(card.closeLabel, alpha);
    }
    if (card.caption != nullptr) {
      wlr_scene_node_set_enabled(&card.caption->node, settings.showTitles || settings.showWorkspaceNumbers);
      wlr_scene_node_set_position(
          &card.caption->node, (card.box.width - card.caption->dst_width) / 2, card.box.height + 12
      );
      wlr_scene_buffer_set_opacity(card.caption, alpha);
    }
  }

  void Overview::selectSpreadCard(Card* card) {
    if (card == nullptr || card->view == m_spreadSelected) {
      return;
    }
    m_spreadSelected = card->view;
    // Selection is local to the overview. Do not activate a workspace or send client focus while browsing cards.
    clearShortcutInput();
    applyProgress();
  }

  bool Overview::handleSpreadButton(uint32_t button, bool pressed, double lx, double ly) {
    if (!interactive()) {
      return true;
    }
    if (m_spreadLayoutDirty) {
      refreshSpreadLayout();
    }
    Card* hit = nullptr;
    bool closeHit = false;
    for (Card* card : m_spreadOrder) {
      if (config().overview.spread.showCloseButtons
          && card->spreadControls != nullptr
          && card->spreadControls->node.enabled
          && wlr_box_contains_point(&card->closeBox, lx, ly)) {
        hit = card;
        closeHit = true;
        break;
      }
    }
    if (hit == nullptr) {
      hit = cardAt(lx, ly);
    }
    if (button != BTN_LEFT && button != BTN_MIDDLE) {
      return true;
    }
    if (pressed) {
      m_pressCard = hit;
      m_spreadClosePressed = closeHit || button == BTN_MIDDLE;
      m_spreadBackgroundPressed = hit == nullptr && button == BTN_LEFT;
      selectSpreadCard(hit);
      return true;
    }
    Card* card = m_pressCard;
    const bool closePressed = m_spreadClosePressed;
    const bool backgroundPressed = m_spreadBackgroundPressed;
    m_pressCard = nullptr;
    m_spreadClosePressed = false;
    m_spreadBackgroundPressed = false;
    if (card != nullptr && card == hit && card->view->mapped()) {
      if (closePressed && (closeHit || button == BTN_MIDDLE)) {
        card->view->requestClose();
      } else if (!closePressed && !closeHit && button == BTN_LEFT) {
        closeToWorkspace(card->view->workspace(), card->view, true);
      }
    } else if (backgroundPressed && hit == nullptr && button == BTN_LEFT) {
      close();
    }
    return true;
  }

  bool Overview::handleSpreadKey(uint32_t keysym) {
    if (m_spreadLayoutDirty) {
      refreshSpreadLayout();
    }
    if (keysym == XKB_KEY_Escape) {
      close();
      return true;
    }
    if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
      if (m_spreadSelected != nullptr) {
        closeToWorkspace(m_spreadSelected->workspace(), m_spreadSelected, true);
      } else {
        close();
      }
      return true;
    }
    if (keysym == XKB_KEY_Delete) {
      if (m_spreadSelected != nullptr) {
        m_spreadSelected->requestClose();
      }
      return true;
    }
    const bool horizontal =
        keysym == XKB_KEY_Left || keysym == XKB_KEY_Right || keysym == XKB_KEY_Tab || keysym == XKB_KEY_ISO_Left_Tab;
    const bool vertical = keysym == XKB_KEY_Up || keysym == XKB_KEY_Down;
    if (!horizontal && !vertical) {
      return false;
    }
    if (m_spreadOrder.empty() || m_spreadColumns == 0) {
      return true;
    }
    const bool backwards = keysym == XKB_KEY_Left || keysym == XKB_KEY_Up || keysym == XKB_KEY_ISO_Left_Tab;
    const auto current =
        std::ranges::find_if(m_spreadOrder, [this](const Card* card) { return card->view == m_spreadSelected; });
    const size_t index = current != m_spreadOrder.end() ? static_cast<size_t>(current - m_spreadOrder.begin()) : 0;
    const size_t count = m_spreadOrder.size();
    if (!config().overview.spread.allOutputs && keysym != XKB_KEY_Tab && keysym != XKB_KEY_ISO_Left_Tab) {
      std::vector<SpreadBox> cells;
      cells.reserve(count);
      for (const Card* card : m_spreadOrder) {
        cells.push_back(card->spreadCell);
      }
      const int direction = backwards ? -1 : 1;
      const size_t next = spreadNeighbor(cells, index, horizontal ? direction : 0, vertical ? direction : 0);
      selectSpreadCard(m_spreadOrder[next]);
      return true;
    }
    size_t next = backwards ? (index + count - 1) % count : (index + 1) % count;
    if (vertical) {
      const size_t rows = (count + m_spreadColumns - 1) / m_spreadColumns;
      const size_t row = index / m_spreadColumns;
      const size_t nextRow = backwards ? (row + rows - 1) % rows : (row + 1) % rows;
      const SpreadBox& origin = m_spreadOrder[index]->spreadCell;
      double distance = std::numeric_limits<double>::infinity();
      for (size_t i = nextRow * m_spreadColumns; i < std::min(count, (nextRow + 1) * m_spreadColumns); ++i) {
        const SpreadBox& candidate = m_spreadOrder[i]->spreadCell;
        const double dx = std::abs(candidate.x + candidate.width / 2 - origin.x - origin.width / 2);
        if (dx < distance) {
          distance = dx;
          next = i;
        }
      }
    }
    selectSpreadCard(m_spreadOrder[next]);
    return true;
  }

} // namespace umbriel
