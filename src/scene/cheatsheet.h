#pragma once

#include "scene/surface_shadow.h"

struct wlr_scene_tree;

namespace umbriel {
  class Server;

  class Cheatsheet {
  public:
    Cheatsheet(Server& server, wlr_scene_tree* parent);
    ~Cheatsheet();

    Cheatsheet(const Cheatsheet&) = delete;
    Cheatsheet& operator=(const Cheatsheet&) = delete;

    void show();
    void showOnStartup();
    void hide();
    void toggle();
    bool visible() const;
    void relayout();

  private:
    void render();

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_tree* m_tree = nullptr;
    bool m_showWhenConfigReady = false;
    SurfaceShadow m_shadow;
  };

} // namespace umbriel
