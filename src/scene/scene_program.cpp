#include "scene/scene_program.h"

#include <algorithm>

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
}

namespace umbriel::scene_experiment {
  namespace {
    static_assert(kParameterLimit == FX_SCENE_PARAMETERS);
    static_assert(kParameterNameLimit + 1 == sizeof(fx_scene_parameter::name));

    bool validSources(const Sources& sources) {
      std::size_t total = 0;
      for (std::size_t i = 0; i < sources.stages.size(); ++i) {
        const auto stage = static_cast<Stage>(i);
        const bool required = stage == Stage::Fragment;
        const auto& source = sources.stages[i];
        if (!source) {
          if (required) {
            return false;
          }
          continue;
        }
        if (source->code.empty() || source->code.size() > kShaderSourceLimit || source->code.contains('\0')) {
          return false;
        }
        total += source->code.size();
      }
      return total <= kAggregateSourceLimit;
    }

  } // namespace

  void ScenePrograms::clear() {
    m_entries.clear();
    m_renderer = nullptr;
  }

  void ScenePrograms::prepare(
      wlr_renderer* renderer, std::span<const ProgramDefinition> definitions, std::span<const std::string> roots
  ) {
    if (renderer != m_renderer) {
      clear();
      m_renderer = renderer;
    }
    if (renderer == nullptr) {
      return;
    }
    const auto referenced = [&](std::string_view name) { return std::ranges::find(roots, name) != roots.end(); };
    std::erase_if(m_entries, [&](const auto& item) {
      return !referenced(item.first)
          || std::ranges::find(definitions, item.first, &ProgramDefinition::name) == definitions.end();
    });
    for (const auto& definition : definitions) {
      if (!referenced(definition.name)) {
        continue;
      }
      auto [position, inserted] = m_entries.try_emplace(definition.name);
      auto& entry = position->second;
      if (!inserted && entry.definition == definition) {
        continue;
      }
      // A failed replacement is unavailable to new transactions. Existing
      // transactions keep their complete previous bundle through shared ownership.
      entry = Entry{.definition = definition, .bundle = {}, .state = ProgramState::Invalid};
      compile(entry);
    }
  }

  void ScenePrograms::compile(Entry& entry) {
    const auto& definition = entry.definition;
    if (!validSources(definition.sources) || validateParameters(definition.parameters)) {
      return;
    }
    fx_scene_limits limits{};
    const auto count = static_cast<unsigned>(definition.parameters.size());
    if (!fx_scene_program_get_limits(m_renderer, &limits)
        || limits.fragment_texture_units < 2
        || limits.fragment_vectors < FX_SCENE_FRAGMENT_VECTORS + count
        || limits.vertex_vectors < FX_SCENE_VERTEX_VECTORS) {
      entry.state = ProgramState::Unsupported;
      return;
    }
    const auto source = [&](Stage stage) -> const char* {
      const auto& value = definition.sources.stages[static_cast<std::size_t>(stage)];
      return value ? value->code.c_str() : nullptr;
    };
    const fx_scene_sources sources{
        .common = source(Stage::Common),
        .fragment = source(Stage::Fragment),
    };
    std::array<fx_scene_parameter, kParameterLimit> parameters{};
    for (std::size_t i = 0; i < definition.parameters.size(); ++i) {
      const auto& parameter = definition.parameters[i];
      std::ranges::copy(parameter.name, parameters[i].name);
      parameters[i].components = parameter.components;
      std::ranges::copy(parameter.values, parameters[i].value);
    }
    auto* program = fx_scene_program_create(m_renderer, &sources, parameters.data(), count);
    if (program == nullptr) {
      entry.state = ProgramState::CompileFailed;
      return;
    }
    auto bundle = std::make_shared<ProgramBundle>();
    bundle->definition = definition;
    bundle->program = std::shared_ptr<fx_scene_program>(program, fx_scene_program_unref);
    bundle->readsRole = fx_scene_program_reads_role(program);
    bundle->readsTime = fx_scene_program_reads_time(program);
    entry.bundle = std::move(bundle);
    entry.state = ProgramState::Ready;
  }

  std::shared_ptr<const ProgramBundle> ScenePrograms::find(std::string_view name) const {
    const auto position = m_entries.find(name);
    return position == m_entries.end() ? nullptr : position->second.bundle;
  }

  ProgramState ScenePrograms::state(std::string_view name) const {
    const auto position = m_entries.find(name);
    return position == m_entries.end() ? ProgramState::Unreferenced : position->second.state;
  }
} // namespace umbriel::scene_experiment
