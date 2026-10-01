#pragma once
#include "core/animation.h"
#include "scene/presentation.h"

#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string_view>
struct timespec;
namespace umbriel {
  class Server;
  class Output;
  class Workspace;
  class WorkspaceTransition final {
  public:
    WorkspaceTransition(Server& server, Output& output);
    ~WorkspaceTransition();
    void update(Workspace& from, Workspace& to, double progress, const AnimatedValue& animation);
    void finish();
    void cancel(PresentationFallback reason);
    void prepareFrame(bool animate);
    void frameSubmitted(bool success);
    void sendFrameDone(const timespec& when);
    [[nodiscard]] bool active() const;
    [[nodiscard]] nlohmann::json status() const;

  private:
    bool begin(Workspace& from, Workspace& to);
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace umbriel
