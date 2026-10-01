#pragma once

#include "scene/presentation.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

struct timespec;
struct wlr_buffer;
struct fx_scene_resource_pool;

namespace umbriel {
  class Output;
  class Server;
  class View;

  fx_scene_resource_pool& presentationAggregatePool();

  // Borrowed immutable role images. Valid until the next prepareFrame,
  // frameSubmitted(true), or cancellation; the provider owns their buffers.
  struct WorkspaceSourceFace {
    wlr_buffer* display = nullptr;
    wlr_buffer* unfiltered = nullptr;
  };

  enum class WorkspaceSourceResult { Preparing, Ready, Failed };

  // Paired source images borrowed from the native workspace transition. The caller
  // owns composition and final output replacement.
  class WorkspaceSources {
  public:
    WorkspaceSources(Server& server, Output& output);
    ~WorkspaceSources();
    // The native slide retains both workspaces until this source owner is released.
    bool beginPair(std::string_view from, std::string_view to);
    bool freezeOutgoing();
    WorkspaceSourceResult prepareFrame(bool animate);
    [[nodiscard]] std::span<const WorkspaceSourceFace> faces() const;
    void frameSubmitted(bool success);
    void sendFrameDone(const timespec& when);
    void cancel(PresentationFallback reason);
    void contentChanged();
    void viewMapped(View& view);
    void viewWillUnmap(View& view);
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool renderLocked() const;
    [[nodiscard]] uint64_t reservedBytes() const;
    [[nodiscard]] uint64_t revision() const;
    [[nodiscard]] PresentationFallback lastFallback() const;
    // Final composition reserves output targets in this same output
    // arena and presentationAggregatePool before acquisition. Release those
    // reservations before destroying this provider.
    [[nodiscard]] fx_scene_resource_pool& resourcePool();

    void tick(bool animate);
    void frameCommitted();

  private:
    bool open(std::string_view from, std::string_view to);
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace umbriel
