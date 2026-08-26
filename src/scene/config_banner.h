#pragma once

#include <vector>

struct wl_event_source;
struct wlr_scene_buffer;
struct wlr_scene_tree;

namespace umbriel {
  struct ConfigDiagnostic;
  class Server;

  class ConfigBanner {
  public:
    ConfigBanner(Server& server, wlr_scene_tree* parent);
    ~ConfigBanner();

    ConfigBanner(const ConfigBanner&) = delete;
    ConfigBanner& operator=(const ConfigBanner&) = delete;

    void show(const std::vector<ConfigDiagnostic>& diagnostics);
    void hide();
    [[nodiscard]] bool visible() const { return m_sceneBuffer != nullptr; }
    void relayout();

  private:
    void render(const std::vector<ConfigDiagnostic>& diagnostics);

    Server& m_server;
    wlr_scene_tree* m_parent;
    wlr_scene_buffer* m_sceneBuffer = nullptr;
    wl_event_source* m_hideTimer = nullptr;
    std::vector<ConfigDiagnostic> m_lastDiagnostics;
    bool m_persistent = false; // true when errors present
  };

} // namespace umbriel
