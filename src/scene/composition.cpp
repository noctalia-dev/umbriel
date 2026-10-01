#include "scene/composition.h"

#include "wlr.h"

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace umbriel {
  struct SceneComposition::State {
    struct Target {
      wlr_buffer* buffer = nullptr;
      fx_scene_target* target = nullptr;
      ~Target() { reset(); }
      void reset() {
        fx_scene_target_destroy(target);
        if (buffer != nullptr) {
          wlr_buffer_drop(buffer);
        }
        buffer = nullptr;
        target = nullptr;
      }
      bool prepare(
          wlr_renderer* renderer, wlr_allocator* allocator, int width, int height, bool workingSpace, bool floatingPoint
      ) {
        buffer = fx_scene_buffer_create(renderer, allocator, width, height, floatingPoint);
        target = buffer != nullptr ? fx_scene_target_create_with_color(renderer, buffer, workingSpace) : nullptr;
        return target != nullptr;
      }
    };
    struct Version {
      std::array<Target, 2> output;
      bool alias = false;
    };
    // Declare the reservation first so every GPU object dies before its bytes
    // are released. Destruction is explicit because pools are externally owned.
    fx_scene_reservation reservation{};
    fx_scene_reservation extraReservation{};
    wlr_allocator* allocator = nullptr;
    int width = 0, height = 0;
    bool workingSpace = false, floatingPoint = false;
    uint64_t imageBytes = 0;
    wlr_renderer* renderer = nullptr;
    wl_listener rendererDestroy{};
    std::shared_ptr<const scene_experiment::ProgramBundle> bundle;
    std::array<Version, 2> versions;
    bool pending = false;
    int committed = -1;
    unsigned candidate = 0;
    State() { wl_list_init(&rendererDestroy.link); }
    ~State() { wl_list_remove(&rendererDestroy.link); }
  };

  SceneComposition::SceneComposition(std::unique_ptr<State> state) : m_state(std::move(state)) {}
  SceneComposition::~SceneComposition() {
    // Keep the external pool pointers until all targets have released their
    // renderer/buffer references, including a failed unsubmitted candidate.
    auto reservation = m_state->reservation;
    auto extraReservation = m_state->extraReservation;
    m_state->reservation = {};
    m_state.reset();
    fx_scene_release(&reservation);
    fx_scene_release(&extraReservation);
  }

  std::unique_ptr<SceneComposition> SceneComposition::create(
      wlr_renderer* renderer, wlr_allocator* allocator, fx_scene_resource_pool& outputPool,
      fx_scene_resource_pool& aggregatePool, std::shared_ptr<const scene_experiment::ProgramBundle> bundle, int width,
      int height, bool workingSpace, bool floatingPoint
  ) {
    if (renderer == nullptr
        || allocator == nullptr
        || bundle == nullptr
        || !bundle->program
        || width <= 0
        || height <= 0) {
      return nullptr;
    }
    fx_scene_limits limits{};
    if (!fx_scene_program_get_limits(renderer, &limits)
        || static_cast<unsigned>(width) > limits.texture_size
        || static_cast<unsigned>(height) > limits.texture_size
        || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > FX_SCENE_OUTPUT_BUDGET / 4) {
      return nullptr;
    }
    auto state = std::make_unique<State>();
    state->renderer = renderer;
    state->allocator = allocator;
    state->width = width;
    state->height = height;
    state->workingSpace = workingSpace;
    state->floatingPoint = floatingPoint;
    state->rendererDestroy.notify = [](wl_listener* listener, void*) {
      State* state;
      state = wl_container_of(listener, state, rendererDestroy);
      state->renderer = nullptr;
      state->pending = false;
      state->committed = -1;
      wl_list_remove(&state->rendererDestroy.link);
      wl_list_init(&state->rendererDestroy.link);
    };
    wl_signal_add(&renderer->events.destroy, &state->rendererDestroy);
    state->bundle = std::move(bundle);
    // Conservative row/page alignment allowance, charged for all double-buffered
    // role targets. No independent budget.
    const uint64_t stride = (static_cast<uint64_t>(width) * (floatingPoint ? 8 : 4) + 255) & ~uint64_t{255};
    const uint64_t image = (stride * static_cast<uint64_t>(height) + 4095) & ~uint64_t{4095};
    const uint64_t count = state->bundle->readsRole ? 4 : 2;
    state->imageBytes = image;
    if (image > (std::numeric_limits<uint64_t>::max() - sizeof(State)) / count
        || !fx_scene_reserve(&state->reservation, &outputPool, &aggregatePool, sizeof(State) + count * image)) {
      return nullptr;
    }
    auto composition = std::unique_ptr<SceneComposition>(new SceneComposition(std::move(state)));

    for (auto& version : composition->m_state->versions) {
      for (unsigned role = 0; role < count / 2; ++role) {
        if (!version.output[role].prepare(renderer, allocator, width, height, workingSpace, floatingPoint)) {
          return nullptr;
        }
      }
    }
    return composition;
  }

  bool SceneComposition::render(const fx_scene_frame& frame, std::span<const Source> sources) {
    auto& state = *m_state;
    if (state.renderer == nullptr) {
      return false;
    }
    if (state.pending) {
      return true; // Retry exactly the already-completed pair, with no new inputs.
    }
    if (sources.size() != 2)
      return false;
    struct DestroyTexture {
      void operator()(wlr_texture* texture) const { wlr_texture_destroy(texture); }
    };
    using Texture = std::unique_ptr<wlr_texture, DestroyTexture>;
    std::array<std::array<Texture, 2>, 2> textures;
    for (size_t i = 0; i < sources.size(); ++i) {
      const auto& source = sources[i];
      if (source.display == nullptr || source.unfiltered == nullptr)
        return false;
      textures[i][0].reset(wlr_texture_from_buffer(state.renderer, source.display));
      if (!textures[i][0])
        return false;
      if (source.unfiltered != source.display) {
        textures[i][1].reset(wlr_texture_from_buffer(state.renderer, source.unfiltered));
        if (!textures[i][1])
          return false;
      }
    }
    const bool alias = !state.bundle->readsRole
        && std::ranges::all_of(sources, [](const auto& source) { return source.display == source.unfiltered; });
    if (!alias && !state.versions[0].output[1].target) {
      if (!fx_scene_reserve(
              &state.extraReservation, state.reservation.output, state.reservation.aggregate, 2 * state.imageBytes
          ))
        return false;
      for (auto& version : state.versions) {
        if (!version.output[1].prepare(
                state.renderer, state.allocator, state.width, state.height, state.workingSpace, state.floatingPoint
            )) {
          for (auto& failedVersion : state.versions)
            failedVersion.output[1].reset();
          fx_scene_release(&state.extraReservation);
          return false;
        }
      }
    }
    auto& version = state.versions[state.candidate];
    version.alias = alias;
    for (unsigned role = 0; role < (alias ? 1U : 2U); ++role) {
      auto roleFrame = frame;
      roleFrame.role = static_cast<int>(role);
      std::array<fx_scene_input, 2> inputs{};
      inputs[0].texture = (role && textures[0][1] ? textures[0][1] : textures[0][0]).get();
      inputs[1].texture = (role && textures[1][1] ? textures[1][1] : textures[1][0]).get();
      inputs[0].sample_matrix = sources[0].sampleMatrix;
      inputs[1].sample_matrix = sources[1].sampleMatrix;
      if (!fx_scene_program_render(
              state.bundle->program.get(), version.output[role].target, &roleFrame, inputs.data()
          )) {
        return false;
      }
    }

    state.pending = true;
    return true;
  }

  void SceneComposition::submitted(bool success) {
    if (success && m_state->pending) {
      m_state->committed = static_cast<int>(m_state->candidate);
      m_state->candidate ^= 1;
      m_state->pending = false;
    }
  }
  SceneComposition::Source SceneComposition::candidate() const {
    if (!m_state->pending) {
      return {};
    }
    const auto& version = m_state->versions[m_state->candidate];
    return {version.output[0].buffer, version.output[version.alias ? 0 : 1].buffer};
  }
  SceneComposition::Source SceneComposition::committed() const {
    if (m_state->committed < 0) {
      return {};
    }
    const auto& version = m_state->versions[static_cast<unsigned>(m_state->committed)];
    return {version.output[0].buffer, version.output[version.alias ? 0 : 1].buffer};
  }
  bool SceneComposition::pending() const { return m_state->pending; }

  uint64_t SceneComposition::reservedBytes() const {
    return m_state->reservation.bytes + m_state->extraReservation.bytes;
  }
} // namespace umbriel
