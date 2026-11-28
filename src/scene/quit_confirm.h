#pragma once

#include "scene/shortcut_approval.h"
#include "scene/surface_shadow.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

struct wlr_scene_tree;
struct wlr_box;
struct wlr_output;

namespace umbriel {
  class Server;

  // Modal confirmation panel. Rendering only: key and button handling live in
  // Keyboard::handleKey and Cursor::handleButton.
  class QuitConfirm {
  public:
    enum class Kind {
      SessionQuit,
      ScreenCastDynamic,
      InputEmulation,
      InputCapture,
      GlobalShortcuts,
    };

    QuitConfirm(Server& server, wlr_scene_tree* parent);
    ~QuitConfirm(); // calls hide()

    QuitConfirm(const QuitConfirm&) = delete;
    QuitConfirm& operator=(const QuitConfirm&) = delete;

    void show(Kind kind);
    void showInputEmulation(std::string_view application);
    void showInputCapture(std::string_view application, uint32_t persistMode);
    void showGlobalShortcuts(std::string_view application, std::span<const GlobalShortcutRegistration> shortcuts);
    bool refreshShortcutApproval();
    void advanceShortcutPage();
    bool shortcutPageFinal() const;
    size_t shortcutPage() const { return m_shortcutPage; }
    size_t shortcutPages() const { return m_shortcutLayout ? m_shortcutLayout->pages.size() : 0; }
    void hide();
    bool visible() const { return m_tree != nullptr; }
    Kind kind() const { return m_kind; }
    bool canRememberInputCapture() const {
      return m_kind == Kind::InputCapture && (m_inputCapturePersistMode == 1 || m_inputCapturePersistMode == 2);
    }
    void relayout(); // re-render if visible (output add/remove/scale change)

  private:
    void render();
    bool updateShortcutLayout();
    void renderShortcutApproval();
    void renderPanel(TextBufferResult rendered, const wlr_box& outputBox, bool haveOutput);

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    Kind m_kind = Kind::SessionQuit;
    std::string m_application;
    uint32_t m_inputCapturePersistMode = 0;
    std::vector<GlobalShortcutRegistration> m_shortcuts;
    std::optional<ShortcutApprovalLayout> m_shortcutLayout;
    size_t m_shortcutPage = 0;
    wlr_output* m_shortcutOutput = nullptr;
    std::array<int, 4> m_shortcutArea{};
    double m_shortcutScale = 1.0;
    SurfaceShadow m_shadow;
  };

} // namespace umbriel
