#include "scene/quit_confirm.h"

#include "config/config.h"
#include "input/global_shortcuts_controller.h"
#include "input/input_capture.h"
#include "output/output.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

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

  void QuitConfirm::show(Kind kind) {
    if (GlobalShortcuts* shortcuts = m_server.globalShortcuts()) {
      shortcuts->suspend();
    }
    if (InputCapture* capture = m_server.inputCapture(); capture != nullptr && capture->active()) {
      capture->suspend();
    }
    m_kind = kind;
    render();
  }

  void QuitConfirm::showInputEmulation(std::string_view application) {
    m_application = application;
    show(Kind::InputEmulation);
  }

  void QuitConfirm::showInputCapture(std::string_view application, uint32_t persistMode) {
    m_application = application;
    m_inputCapturePersistMode = persistMode;
    show(Kind::InputCapture);
  }

  void QuitConfirm::showGlobalShortcuts(
      std::string_view application, std::span<const GlobalShortcutRegistration> shortcuts
  ) {
    m_application = application;
    m_shortcuts.assign(shortcuts.begin(), shortcuts.end());
    m_shortcutLayout.reset();
    m_shortcutPage = 0;
    show(Kind::GlobalShortcuts);
  }

  bool QuitConfirm::updateShortcutLayout() {
    wlr_output* output = m_server.preferredOutput();
    const Output* owner = m_server.outputFromWlr(output);
    if (output == nullptr || owner == nullptr || !output->enabled) {
      m_shortcutLayout.reset();
      m_shortcutPage = 0;
      return true;
    }
    const auto area = owner->usableArea();
    const std::array bounds{area.x, area.y, area.width, area.height};
    const double scale = output->scale;
    if (m_shortcutLayout && m_shortcutOutput == output && m_shortcutArea == bounds && m_shortcutScale == scale) {
      return false;
    }
    m_shortcutOutput = output;
    m_shortcutArea = bounds;
    m_shortcutScale = scale;
    m_shortcutPage = 0;
    m_shortcutLayout = layoutShortcutApproval(
        m_application, m_shortcuts, area.width - 2 * kMargin, area.height - 2 * kMargin, std::max(1.0, std::ceil(scale))
    );
    return true;
  }

  bool QuitConfirm::refreshShortcutApproval() {
    if (m_kind != Kind::GlobalShortcuts || !updateShortcutLayout()) {
      return false;
    }
    render();
    if (!visible()) {
      m_server.failGlobalShortcuts();
    }
    return true;
  }

  bool QuitConfirm::shortcutPageFinal() const {
    return m_shortcutLayout && m_shortcutLayout->pages.at(m_shortcutPage).final;
  }

  void QuitConfirm::advanceShortcutPage() {
    if (!m_shortcutLayout || shortcutPageFinal()) {
      return;
    }
    ++m_shortcutPage;
    render();
    if (!visible()) {
      m_server.failGlobalShortcuts();
    }
  }

  void QuitConfirm::hide() {
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
    m_shortcuts.clear();
    m_shortcutLayout.reset();
    m_shortcutPage = 0;
    m_shortcutOutput = nullptr;
  }

  void QuitConfirm::relayout() {
    if (m_tree != nullptr) {
      render();
      if (m_kind == Kind::GlobalShortcuts && !visible()) {
        m_server.failGlobalShortcuts();
      }
    }
  }

  void QuitConfirm::render() {
    // Destroy previous subtree.
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
    if (m_kind == Kind::GlobalShortcuts) {
      renderShortcutApproval();
      return;
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
    const auto& accent = m_kind == Kind::SessionQuit ? colors.error : colors.warning;
    const std::string headingColor = rgbaHex(accent);
    const std::string textColor = rgbaHex(colors.textPrimary);

    const std::string_view heading = m_kind == Kind::InputCapture ? "Allow input capture?"
        : m_kind == Kind::InputEmulation                          ? "Allow desktop control?"
        : m_kind == Kind::SessionQuit                             ? "Quit Umbriel?"
                                                                  : "Allow screencast target changes?";
    std::string_view captureChoices = "Enter allows this capture session.\n"
                                      "Any other key or click denies it.";
    if (m_inputCapturePersistMode == 1) {
      captureChoices = "Enter allows once.\n"
                       "R remembers approval while the application runs.\n"
                       "Any other key or click denies it.";
    } else if (m_inputCapturePersistMode == 2) {
      captureChoices = "Enter allows once.\n"
                       "R remembers approval until you revoke it.\n"
                       "Any other key or click denies it.";
    }
    const std::string detail = m_kind == Kind::InputCapture
        ? std::format(
              "A client requesting capture for '{}' wants to send your input to another application.\n"
              "The application label is supplied by the requester.\n\n"
              "{}\n\n"
              "Press Ctrl+Alt+Escape to stop capture and restore local control.",
              m_application, captureChoices
          )
        : m_kind == Kind::InputEmulation
        ? std::format(
              "An X11 application claiming to be '{}' wants to control your pointer and keyboard.\n"
              "Enter allows it for this Umbriel session; any other key or click denies it.",
              m_application
          )
        : m_kind == Kind::SessionQuit
        ? "Enter confirms; any other key or click cancels"
        : "This can reveal another window or screen. Enter confirms; any other key or click cancels";
    std::string markup =
        std::format("<span size='14pt' weight='bold' foreground='{}'>{}</span>", headingColor, heading);
    markup += std::format(
        "{}<span foreground='{}'>{}</span>", m_kind == Kind::InputCapture ? "\n\n" : "\n", textColor,
        escapeMarkup(detail)
    );
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

    renderPanel(rendered, outputBox, haveOutput);
  }

  void QuitConfirm::renderShortcutApproval() {
    updateShortcutLayout();
    if (!m_shortcutLayout) {
      return;
    }
    auto params = m_shortcutLayout->pageParams(m_shortcutPage);
    params.markup = std::format("<span foreground='{}'>{}</span>", rgbaHex(config().colors.textPrimary), params.markup);
    auto rendered = renderTextBuffer(params);
    if (rendered.buffer == nullptr) {
      return;
    }
    if (rendered.logicalWidth > m_shortcutLayout->width || rendered.logicalHeight > m_shortcutLayout->height) {
      wlr_buffer_drop(rendered.buffer);
      return;
    }
    const wlr_box area{m_shortcutArea[0], m_shortcutArea[1], m_shortcutArea[2], m_shortcutArea[3]};
    renderPanel(rendered, area, true);
  }

  void QuitConfirm::renderPanel(TextBufferResult rendered, const wlr_box& outputBox, bool haveOutput) {
    m_tree = wlr_scene_tree_create(m_parent);
    if (m_tree == nullptr) {
      wlr_buffer_drop(rendered.buffer);
      return;
    }
    const auto& colors = config().colors;
    const auto& accent = m_kind == Kind::SessionQuit ? colors.error : colors.warning;

    // Shadow, colored border, rounded panel, then text.
    const int cornerRadius = config().appearance.cornerRadius;
    m_shadow.update(m_tree, rendered.logicalWidth, rendered.logicalHeight, kBorderWidth, cornerRadius);

    float borderColor[4]{};
    premultiplied(borderColor, accent, 1.0F);
    wlr_scene_border* panelBorder = wlr_scene_border_create(m_tree, borderColor, borderColor);
    if (panelBorder == nullptr) {
      wlr_buffer_drop(rendered.buffer);
      hide();
      return;
    }
    applyBorderGeometry(
        panelBorder, makeBorderRing(rendered.logicalWidth, rendered.logicalHeight, cornerRadius, kBorderWidth, 0),
        kBorderWidth, 0
    );

    float panelColor[4]{};
    premultiplied(panelColor, colors.background, 1.0F);
    wlr_scene_rect* panelRect =
        wlr_scene_rect_create(m_tree, rendered.logicalWidth, rendered.logicalHeight, panelColor);
    if (panelRect == nullptr) {
      wlr_buffer_drop(rendered.buffer);
      hide();
      return;
    }
    wlr_scene_rect_set_corner_radius(panelRect, nestedRadius(cornerRadius, kBorderWidth));
    (void)panelRect;

    wlr_scene_buffer* sceneBuf = wlr_scene_buffer_create(m_tree, rendered.buffer);
    wlr_buffer_drop(rendered.buffer); // scene holds the lock
    if (sceneBuf != nullptr) {
      // Map device-pixel buffer to logical output size; pass clicks through
      // (the click-cancel is global in Cursor::handleButton).
      wlr_scene_buffer_set_dest_size(sceneBuf, rendered.logicalWidth, rendered.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
    } else {
      hide();
      return;
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
