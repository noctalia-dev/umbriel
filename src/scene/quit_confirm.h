#pragma once

#include "scene/surface_shadow.h"

#include <cstdint>
#include <string>
#include <string_view>

struct wlr_scene_tree;

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
    };

    QuitConfirm(Server& server, wlr_scene_tree* parent);
    ~QuitConfirm(); // calls hide()

    QuitConfirm(const QuitConfirm&) = delete;
    QuitConfirm& operator=(const QuitConfirm&) = delete;

    void show(Kind kind);
    void showInputEmulation(std::string_view application);
    void showInputCapture(std::string_view application, uint32_t persistMode);
    void hide();
    [[nodiscard]] bool visible() const { return m_tree != nullptr; }
    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] bool canRememberInputCapture() const {
      return m_kind == Kind::InputCapture && (m_inputCapturePersistMode == 1 || m_inputCapturePersistMode == 2);
    }
    void relayout(); // re-render if visible (output add/remove/scale change)

  private:
    void render();

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    Kind m_kind = Kind::SessionQuit;
    std::string m_application;
    uint32_t m_inputCapturePersistMode = 0;
    SurfaceShadow m_shadow;
  };

} // namespace umbriel
