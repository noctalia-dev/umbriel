#include "config/fields.h"

#include "config/keybind_parse.h"
#include "config/store.h"
#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");

  } // namespace

  void emitDiag(ConfigDiagnostic::Severity severity, const toml::source_region* src, std::string msg) {
    ConfigDiagnostic diag;
    diag.severity = severity;
    diag.message = msg;
    if (src != nullptr) {
      diag.line = src->begin.line;
      diag.column = src->begin.column;
      if (src->path != nullptr) {
        diag.file = *src->path;
      }
    }
    const std::string loc = diag.location();
    if (severity == ConfigDiagnostic::Severity::Error) {
      kLog.error("{}{}", loc.empty() ? "" : loc + ": ", msg);
    } else {
      kLog.warn("{}{}", loc.empty() ? "" : loc + ": ", msg);
    }
    configStore().addDiagnostic(std::move(diag));
  }

  std::string lowercase(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return lowered;
  }

  const registry::Choices<VrrMode>& vrrModes() {
    static const registry::Choices<VrrMode> choices{
        {.name = "disabled", .value = VrrMode::Disabled},
        {.name = "always", .value = VrrMode::Always},
        {.name = "fullscreen", .value = VrrMode::Fullscreen},
    };
    return choices;
  }

  const registry::Choices<HdrMode>& hdrModes() {
    static const registry::Choices<HdrMode> choices{
        {.name = "off", .value = HdrMode::Off},
        {.name = "on", .value = HdrMode::On},
        {.name = "auto", .value = HdrMode::Auto},
        {.name = "fullscreen", .value = HdrMode::Fullscreen},
    };
    return choices;
  }

  const registry::Choices<ContentType>& contentTypes() {
    static const registry::Choices<ContentType> choices{
        {.name = "none", .value = ContentType::None},
        {.name = "photo", .value = ContentType::Photo},
        {.name = "video", .value = ContentType::Video},
        {.name = "game", .value = ContentType::Game},
    };
    return choices;
  }

  std::optional<std::string> scratchpadSelectorError(const Config& loaded, const Keybind& binding) {
    const auto* scratchpad = payloadIf<ScratchpadArg>(binding);
    if (scratchpad == nullptr) {
      return std::nullopt;
    }
    if (loaded.scratchpads.empty()) {
      if (!scratchpad->name.empty() && scratchpad->name != "default") {
        return std::format("unknown scratchpad '{}'", scratchpad->name);
      }
      return std::nullopt;
    }
    if (scratchpad->name.empty()) {
      return std::string{"scratchpad name required"};
    }
    const bool configured = std::ranges::any_of(loaded.scratchpads, [&](const ScratchpadConfig& candidate) {
      return candidate.name == scratchpad->name;
    });
    if (configured) {
      return std::nullopt;
    }
    return std::format("unknown scratchpad '{}'", scratchpad->name);
  }

  std::optional<std::string> scratchpadTargetError(const Config& loaded, std::string_view name) {
    if (loaded.scratchpads.empty()) {
      return name == "default" ? std::nullopt : std::optional{std::format("unknown scratchpad '{}'", name)};
    }
    const bool configured = std::ranges::any_of(loaded.scratchpads, [name](const ScratchpadConfig& candidate) {
      return candidate.name == name;
    });
    return configured ? std::nullopt : std::optional{std::format("unknown scratchpad '{}'", name)};
  }

  std::optional<std::string> effectActionError(const Config& loaded, const Keybind& binding) {
    const auto reference = effectActionReference(binding);
    if (!reference) {
      return std::nullopt;
    }
    const auto constraint =
        reference->poolRequired ? EffectReferenceConstraint::PoolRequired : EffectReferenceConstraint::PresetOrPool;
    return effectReferenceError(loaded.effects, reference->name, reference->kind, false, constraint);
  }

  void addEffectReference(
      std::vector<EffectReference>& references, std::string context,
      const std::pair<std::string, toml::source_region>& selector, EffectKind kind, bool allowOff,
      std::function<void()> clear, EffectReferenceConstraint constraint, scene_experiment::Binding sceneBinding
  ) {
    references.push_back({
        .context = std::move(context),
        .name = selector.first,
        .kind = kind,
        .allowOff = allowOff,
        .constraint = constraint,
        .sceneBinding = sceneBinding,
        .source = selector.second,
        .clear = std::move(clear),
    });
  }

  void recordRuleEffect(
      registry::ReadContext& context, Section& keys, std::string_view key, EffectKind kind, std::function<void()> clear
  ) {
    const toml::node* node = keys.node(key);
    if (node == nullptr || !node->is_string()) {
      return;
    }
    addEffectReference(
        context.effectReferences, keys.qualified(key), {*node->value<std::string>(), node->source()}, kind, true,
        std::move(clear), EffectReferenceConstraint::PresetOrPool
    );
  }

} // namespace umbriel
