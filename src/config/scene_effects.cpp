#include "config/scene_effects.h"

#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
#include "config/section.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace umbriel::scene_experiment {

  std::optional<Scope> parseScope(std::string_view text) {
    if (text == "workspace_pair")
      return Scope::WorkspacePair;
    return std::nullopt;
  }

  std::string_view scopeName(Scope scope) {
    switch (scope) {
    case Scope::WorkspacePair:
      return "workspace_pair";
    }
    return {};
  }

  bool acceptsBinding(Scope scope, Binding binding) {
    switch (scope) {
    case Scope::WorkspacePair:
      return binding == Binding::WorkspaceSwitch;
    }
    return false;
  }

  SourceReadResult readSources(Section& section, Scope scope, std::vector<ConfigDiagnostic>& diagnostics) {
    constexpr std::array<std::string_view, kStageCount> keys{"common_shader", "shader"};
    SourceReadResult result;
    Sources sources{.scope = scope, .stages = {}};
    bool valid = true;
    std::size_t total = 0;
    for (auto key : {"vertex_shader", "composite_shader"}) {
      if (const auto* node = section.node(key)) {
        diagnostics.push_back(makeDiagnostic(
            ConfigDiagnostic::Severity::Warning, node->source(),
            std::string(key) + " is incompatible with workspace_pair"
        ));
        valid = false;
      }
    }
    const auto warn = [&](const toml::source_region& location, std::string message) {
      diagnostics.push_back(makeDiagnostic(ConfigDiagnostic::Severity::Warning, location, std::move(message)));
      valid = false;
    };
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const auto stage = static_cast<Stage>(i);
      const bool required = stage == Stage::Fragment;
      const toml::node* node = section.node(keys[i]);
      // Read every declared stage even when another fails: all dependencies must
      // remain watched, and no prefix of a changed bundle may be installed.
      auto read = readShaderSource(section, keys[i], diagnostics);
      for (auto& path : read.watchPaths) {
        if (std::ranges::find(result.watchPaths, path) == result.watchPaths.end()) {
          result.watchPaths.push_back(std::move(path));
        }
      }

      if (required && node == nullptr) {
        warn(section.table().source(), std::string(keys[i]) + " is required for this scene scope");
      }
      if (node != nullptr && !read.source) {
        valid = false;
      }
      if (read.source) {
        total += read.source->code.size(); // Two individually bounded sources.
        sources.stages[i] = std::move(read.source);
      }
    }
    if (total > kAggregateSourceLimit) {
      warn(section.table().source(), "aggregate scene source exceeds experimental 512 KiB limit");
    }
    if (valid) {
      result.sources = std::move(sources);
    }
    return result;
  }

  std::optional<std::string> validateParameters(std::span<const Parameter> parameters) {
    if (parameters.size() > kParameterLimit) {
      return "too many scene parameters";
    }
    std::set<std::string_view> names;
    for (const auto& parameter : parameters) {
      const auto& name = parameter.name;
      if (name.size() > kParameterNameLimit || name.contains('\0') || !fx_scene_parameter_identifier(name.c_str())) {
        return "invalid or reserved scene parameter identifier";
      }
      if (!names.insert(name).second) {
        return "duplicate scene parameter identifier";
      }
      if (parameter.components < 1 || parameter.components > 4) {
        return "scene parameters must have one to four components";
      }
      for (unsigned i = 0; i < parameter.values.size(); ++i) {
        if (!std::isfinite(parameter.values[i]) || (i >= parameter.components && parameter.values[i] != 0.0F)) {
          return "scene parameter values must be finite with zero unused components";
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::vector<Parameter>> readParameters(Section& section) {
    std::vector<Parameter> result;
    const auto* node = section.take("parameters");
    if (node == nullptr) {
      return result;
    }
    const auto* table = node->as_table();
    if (table == nullptr || table->size() > kParameterLimit) {
      section.warn(*node, "scene parameters must be a table of at most 32 scalar/vector values");
      return std::nullopt;
    }
    result.reserve(table->size());
    for (const auto& [key, value] : *table) {
      Parameter parameter{.name = std::string(key.str()), .components = 1, .values = {}};
      const auto number = [](const toml::node& component) -> std::optional<float> {
        if (!component.is_number()) {
          return std::nullopt;
        }
        const auto value = component.value<double>();
        if (!value || !std::isfinite(*value) || std::abs(*value) > std::numeric_limits<float>::max()) {
          return std::nullopt;
        }
        return static_cast<float>(*value);
      };
      bool valid = true;
      if (const auto* array = value.as_array()) {
        valid = !array->empty() && array->size() <= 4;
        if (valid) {
          parameter.components = static_cast<unsigned>(array->size());
          for (unsigned i = 0; i < parameter.components; ++i) {
            const auto component = number(*array->get(i));
            valid = valid && component.has_value();
            parameter.values[i] = component.value_or(0);
          }
        }
      } else if (const auto component = number(value)) {
        parameter.values[0] = *component;
      } else {
        valid = false;
      }
      if (!valid) {
        section.warn(value, "scene parameter '" + parameter.name + "' must have one to four finite float components");
        return std::nullopt;
      }
      result.push_back(std::move(parameter));
    }
    if (const auto error = validateParameters(result)) {
      section.warn(*node, *error);
      return std::nullopt;
    }
    return result;
  }

} // namespace umbriel::scene_experiment
