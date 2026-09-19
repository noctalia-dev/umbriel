#include "scene/quit_confirm.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace {

  constexpr int kPadding = 22;
  constexpr int kBorderWidth = 2;
  constexpr int kDefaultMaxWidth = 800;
  constexpr int kAbsMaxWidth = 960;
  constexpr int kMargin = 24;

} // namespace

namespace umbriel {

  QuitConfirm::QuitConfirm(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  QuitConfirm::~QuitConfirm() { hide(); }

  void QuitConfirm::show() { render(); }

  void QuitConfirm::hide() {
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
  }

  void QuitConfirm::relayout() {
    if (m_tree != nullptr) {
      render();
    }
  }

  void QuitConfirm::render() {
    // Destroy previous subtree.
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }

    // Determine output scale and dimensions.
    wlr_output* output = m_server.preferredOutput();
    double scale = 1.0;
    int outputLogicalWidth = 0;
    wlr_box outputBox{};
    bool haveOutput = false;
    if (output != nullptr) {
      scale = std::max(1.0, std::ceil(static_cast<double>(output->scale)));
      wlr_output_layout_get_box(m_server.outputLayout(), output, &outputBox);
      outputLogicalWidth = outputBox.width;
      haveOutput = true;
    }

    int maxTextWidth = kDefaultMaxWidth;
    if (outputLogicalWidth > 0) {
      maxTextWidth = std::min(outputLogicalWidth - 80, kAbsMaxWidth);
      maxTextWidth = std::max(maxTextWidth, 200);
    }

    const auto& colors = config().colors;
    const std::string headingColor = rgbaHex(colors.error);
    const std::string textColor = rgbaHex(colors.textPrimary);

    // Static strings, no user input, so no escaping needed. Transparent background: the panel rect behind provides the
    // surface. The title is sized like the cheatsheet's heading.
    std::string markup =
        std::format("<span size='14pt' weight='bold' foreground='{}'>Quit Umbriel?</span>", headingColor);
    markup += std::format("\n<span foreground='{}'>Enter confirms; any other key or click cancels</span>", textColor);
    TextBufferResult rendered = renderTextBuffer({
        .markup = std::move(markup),
        .font = "monospace 11",
        .maxWidth = maxTextWidth,
        .padding = kPadding,
        .scale = scale,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });
    if (rendered.buffer == nullptr) {
      return;
    }

    m_tree = wlr_scene_tree_create(m_parent);

    // Shadow, colored border, rounded panel, then text.
    const int cornerRadius = config().appearance.cornerRadius;
    m_shadow.update(
        m_tree, rendered.logicalWidth, rendered.logicalHeight, kBorderWidth, cornerRadius, globalShadowOptions()
    );

    float borderColor[4]{};
    premultiplied(borderColor, colors.error, 1.0F);
    wlr_scene_border* panelBorder = wlr_scene_border_create(m_tree, borderColor, borderColor);
    applyBorderGeometry(
        panelBorder, makeBorderRing(rendered.logicalWidth, rendered.logicalHeight, cornerRadius, kBorderWidth, 0),
        kBorderWidth, 0
    );

    float panelColor[4]{};
    premultiplied(panelColor, colors.background, 1.0F);
    wlr_scene_rect* panelRect =
        wlr_scene_rect_create(m_tree, rendered.logicalWidth, rendered.logicalHeight, panelColor);
    wlr_scene_rect_set_corner_radius(panelRect, nestedRadius(cornerRadius, kBorderWidth));
    (void)panelRect;

    wlr_scene_buffer* sceneBuf = wlr_scene_buffer_create(m_tree, rendered.buffer);
    wlr_buffer_drop(rendered.buffer); // scene holds the lock
    if (sceneBuf != nullptr) {
      // Map device-pixel buffer to logical output size; pass clicks through
      // (the click-cancel is global in Cursor::handleButton).
      wlr_scene_buffer_set_dest_size(sceneBuf, rendered.logicalWidth, rendered.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
    }

    // Position: centered on both axes of the preferred output.
    if (haveOutput) {
      const int x = outputBox.x + (outputBox.width - rendered.logicalWidth) / 2;
      const int y = outputBox.y + (outputBox.height - rendered.logicalHeight) / 2;
      wlr_scene_node_set_position(&m_tree->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_tree->node, kMargin, kMargin);
    }
  }

} // namespace umbriel
