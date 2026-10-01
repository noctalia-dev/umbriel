#pragma once

#include "scene/scene_program.h"

#include <array>
#include <memory>
#include <optional>
#include <span>

struct wlr_allocator;
struct wlr_buffer;
struct fx_scene_resource_pool;
struct fx_scene_frame;

namespace umbriel {

  // Two output candidates, each containing both capture roles. A failed native
  // submission holds the completed candidate and its inputs until retry.
  class SceneComposition {
  public:
    struct Source {
      wlr_buffer* display = nullptr;
      wlr_buffer* unfiltered = nullptr;
      const float* sampleMatrix = nullptr; // optional logical UV -> imported texture UV, nine floats
    };
    static std::unique_ptr<SceneComposition> create(
        wlr_renderer* renderer, wlr_allocator* allocator, fx_scene_resource_pool& outputPool,
        fx_scene_resource_pool& aggregatePool, std::shared_ptr<const scene_experiment::ProgramBundle> bundle, int width,
        int height, bool workingSpace, bool floatingPoint = false
    );
    ~SceneComposition();
    SceneComposition(const SceneComposition&) = delete;
    SceneComposition& operator=(const SceneComposition&) = delete;

    // Both sources share one immutable frame; only the capture role differs.
    bool render(const fx_scene_frame& frame, std::span<const Source> sources);
    void submitted(bool success);
    [[nodiscard]] Source candidate() const;
    [[nodiscard]] Source committed() const;
    [[nodiscard]] bool pending() const;
    [[nodiscard]] uint64_t reservedBytes() const;

  private:
    struct State;
    explicit SceneComposition(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
  };

} // namespace umbriel
