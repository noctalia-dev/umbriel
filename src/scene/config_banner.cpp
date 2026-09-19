#include "scene/config_banner.h"

#include "config/config.h"
#include "config/config_diag.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>
#include <wayland-server-core.h>

namespace {

  constexpr int kPadding = 20;
  constexpr int kBorderWidth = 2;
  constexpr int kMaxLines = 6;
  constexpr int kDefaultMaxWidth = 800;
  constexpr int kAbsMaxWidth = 960;
  constexpr int kTopMargin = 24;
  constexpr int kAutoHideMs = 10000;
  // Roughly the width of the bullet and its trailing space in monospace 11, so a
  // wrapped message continues under its own first character.
  constexpr int kHangingIndent = 18;

  // Make a diagnostic file path short relative to the config root's directory.
  std::string shortPath(const umbriel::ConfigDiagnostic& diag, const std::filesystem::path& configDir) {
    if (diag.file.empty()) {
      return {};
    }
    std::string dirStr = configDir.string();
    if (!dirStr.empty() && dirStr.back() != '/') {
      dirStr += '/';
    }
    std::string fileStr = diag.file;
    if (fileStr.starts_with(dirStr)) {
      fileStr = fileStr.substr(dirStr.size());
    }
    std::string loc = fileStr;
    if (diag.line > 0) {
      loc += std::format(":{}", diag.line);
      if (diag.column > 0) {
        loc += std::format(":{}", diag.column);
      }
    }
    return loc;
  }

  int onHideTimer(void* data) {
    auto* banner = static_cast<umbriel::ConfigBanner*>(data);
    banner->hide();
    return 0; // disarm
  }

} // namespace

namespace umbriel {

  ConfigBanner::ConfigBanner(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  ConfigBanner::~ConfigBanner() {
    hide();
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
    }
  }

  void ConfigBanner::show(const std::vector<ConfigDiagnostic>& diagnostics) {
    // Disarm any pending auto-hide.
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
      m_hideTimer = nullptr;
    }

    if (diagnostics.empty()) {
      hide();
      return;
    }

    m_lastDiagnostics = diagnostics;
    m_persistent = std::ranges::any_of(diagnostics, [](const ConfigDiagnostic& d) {
      return d.severity == ConfigDiagnostic::Severity::Error;
    });

    render(diagnostics);

    if (!m_persistent) {
      wl_event_loop* loop = wl_display_get_event_loop(m_server.display());
      m_hideTimer = wl_event_loop_add_timer(loop, onHideTimer, this);
      wl_event_source_timer_update(m_hideTimer, kAutoHideMs);
    }
  }

  void ConfigBanner::hide() {
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
    m_lastDiagnostics.clear();
    m_persistent = false;
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
      m_hideTimer = nullptr;
    }
  }

  void ConfigBanner::relayout() {
    if (m_lastDiagnostics.empty()) {
      return;
    }
    render(m_lastDiagnostics);
  }

  void ConfigBanner::render(const std::vector<ConfigDiagnostic>& diagnostics) {
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

    // Errors first: they are the ones that kept the configuration from applying,
    // and the list is truncated from the end.
    std::vector<const ConfigDiagnostic*> ordered;
    ordered.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
      ordered.push_back(&diagnostic);
    }
    std::ranges::stable_partition(ordered, [](const ConfigDiagnostic* d) {
      return d->severity == ConfigDiagnostic::Severity::Error;
    });

    const bool hasError = !ordered.empty() && ordered.front()->severity == ConfigDiagnostic::Severity::Error;
    const auto& colors = config().colors;
    const std::string severityColor = rgbaHex(hasError ? colors.error : colors.warning);
    const std::string errorColor = rgbaHex(colors.error);
    const std::string warningColor = rgbaHex(colors.warning);
    const std::string textColor = rgbaHex(colors.textPrimary);
    const std::string mutedColor = rgbaHex(colors.textMuted);

    const int total = static_cast<int>(ordered.size());
    const std::string heading = total == 1
        ? std::string(hasError ? "Configuration error" : "Configuration warning")
        : std::format("{} configuration {}", total, hasError ? "problems" : "warnings");

    // Build pango markup: heading, a blank spacer, one bulleted entry per
    // diagnostic, then a muted footer.
    const std::filesystem::path configDir = configRootPath().parent_path();
    std::string markup = std::format(
        "<span size='13pt' weight='bold' foreground='{}'>{}</span>\n", severityColor, escapeMarkup(heading)
    );

    const int shown = std::min(total, kMaxLines);
    for (int i = 0; i < shown; ++i) {
      const ConfigDiagnostic& diagnostic = *ordered[static_cast<size_t>(i)];
      const bool isError = diagnostic.severity == ConfigDiagnostic::Severity::Error;
      const std::string loc = shortPath(diagnostic, configDir);
      markup += std::format("\n<span foreground='{}'>\xe2\x80\xa2</span> ", isError ? errorColor : warningColor);
      if (!loc.empty()) {
        markup += std::format("<span foreground='{}'>{}</span>  ", mutedColor, escapeMarkup(loc));
      }
      markup += std::format("<span foreground='{}'>{}</span>", textColor, escapeMarkup(diagnostic.message));
    }

    std::string footer;
    if (total > shown) {
      footer += std::format("+{} more \xc2\xb7 ", total - shown);
    }
    footer += hasError ? "configuration not applied \xc2\xb7 run `umbriel validate`" : "run `umbriel validate`";
    markup += std::format("\n\n<span foreground='{}'>{}</span>", mutedColor, escapeMarkup(footer));

    // Transparent background: the panel rect behind provides the surface.
    TextBufferResult rendered = renderTextBuffer({
        .markup = std::move(markup),
        .font = "monospace 11",
        .maxWidth = maxTextWidth,
        .padding = kPadding,
        .scale = scale,
        .hangingIndent = kHangingIndent,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });
    if (rendered.buffer == nullptr) {
      return;
    }

    m_tree = wlr_scene_tree_create(m_parent);

    // Shadow, severity-colored border, rounded panel, then text.
    const int cornerRadius = config().appearance.cornerRadius;
    m_shadow.update(
        m_tree, rendered.logicalWidth, rendered.logicalHeight, kBorderWidth, cornerRadius, globalShadowOptions()
    );

    float borderColor[4]{};
    premultiplied(borderColor, hasError ? colors.error : colors.warning, 1.0F);
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
      // Map device-pixel buffer to logical output size; the banner never takes input.
      wlr_scene_buffer_set_dest_size(sceneBuf, rendered.logicalWidth, rendered.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
    }

    // Position: top-center of preferred output.
    if (haveOutput) {
      const int x = outputBox.x + std::max(0, (outputBox.width - rendered.logicalWidth) / 2);
      const int y = outputBox.y + kTopMargin;
      wlr_scene_node_set_position(&m_tree->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_tree->node, kTopMargin, kTopMargin);
    }
  }

} // namespace umbriel
