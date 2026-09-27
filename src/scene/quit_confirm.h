#pragma once

#include "scene/surface_shadow.h"

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
    };

    QuitConfirm(Server& server, wlr_scene_tree* parent);
    ~QuitConfirm(); // calls hide()

    QuitConfirm(const QuitConfirm&) = delete;
    QuitConfirm& operator=(const QuitConfirm&) = delete;

    void show(Kind kind);
    void hide();
    [[nodiscard]] bool visible() const { return m_tree != nullptr; }
    [[nodiscard]] Kind kind() const { return m_kind; }
    void relayout(); // re-render if visible (output add/remove/scale change)

  private:
    void render();

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    Kind m_kind = Kind::SessionQuit;
    SurfaceShadow m_shadow;
  };

} // namespace umbriel
