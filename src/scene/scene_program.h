#pragma once

#include "config/scene_effects.h"

#include <map>
#include <memory>

struct fx_scene_program;
struct fx_scene_frame;
struct wlr_renderer;

namespace umbriel::scene_experiment {

  // Internal until the composition gates establish the public scene interface.
  // Source text, parameter layout/values and consumer metadata are one version.
  struct ProgramDefinition {
    std::string name;
    Sources sources;
    std::vector<Parameter> parameters;
    bool palette = false;
    bool operator==(const ProgramDefinition&) const = default;
  };

  struct ProgramBundle {
    ProgramDefinition definition;
    std::shared_ptr<fx_scene_program> program;
    bool readsTime = false;
    bool readsRole = false;
  };

  enum class ProgramState { Unreferenced, Invalid, Unsupported, CompileFailed, Ready };

  // Storage owned by EffectRegistry. prepare is the only compilation entry;
  // lookups are pure and transactions retain immutable bundles across reloads.
  class ScenePrograms {
  public:
    void
    prepare(wlr_renderer* renderer, std::span<const ProgramDefinition> definitions, std::span<const std::string> roots);
    void clear();
    [[nodiscard]] std::shared_ptr<const ProgramBundle> find(std::string_view name) const;
    [[nodiscard]] ProgramState state(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

  private:
    struct Entry {
      ProgramDefinition definition;
      std::shared_ptr<const ProgramBundle> bundle;
      ProgramState state = ProgramState::Invalid;
    };
    void compile(Entry& entry);
    wlr_renderer* m_renderer = nullptr;
    std::map<std::string, Entry, std::less<>> m_entries;
  };

} // namespace umbriel::scene_experiment
