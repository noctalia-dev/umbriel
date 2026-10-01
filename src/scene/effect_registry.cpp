#include "scene/effect_registry.h"

#include "config/change.h"
#include "config/config.h"
#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
#include "../../umbrielfx/internal/types/wlr_scene.h"

#include <umbrielfx/render/effect.h>
}

namespace umbriel {
  namespace {
    // Event loops here and in change.cpp end at Overview; the slot table must agree.
    static_assert(static_cast<unsigned>(AnimationEvent::Overview) + 1 == FX_ANIMATION_SLOTS);
    static_assert(static_cast<unsigned>(AnimationEvent::Window) == FX_SLOT_WINDOW);
    static_assert(static_cast<unsigned>(AnimationEvent::BorderEffect) == FX_SLOT_BORDER_EFFECT);
    static_assert(static_cast<unsigned>(AnimationEvent::Drag) == FX_SLOT_DRAG);
    static_assert(static_cast<unsigned>(AnimationEvent::WindowsIn) == FX_SLOT_WINDOWS_IN);
    static_assert(static_cast<unsigned>(AnimationEvent::WindowsOut) == FX_SLOT_WINDOWS_OUT);
    constexpr Logger kLog("effects");
    EffectRegistry* s_registry = nullptr;

    // Entering transitions fade in with progress and leaving ones fade out. Only alpha changes, uniformly, so the
    // window's shape and its analytic shadow are unaffected.
    constexpr const char* kBuiltinFade = R"(vec4 animation(vec2 uv) {
    float alpha = umbriel_direction < 0.0 ? 1.0 - umbriel_clamped_progress : umbriel_clamped_progress;
    return umbriel_sample(uv) * alpha;
})";

    // The drag slot's built-in program. uv spans the drawn rectangle (node plus
    // expand); the inverse lookup contracts by DragPhysics::kContraction per step,
    // so 22 steps reach 0.1 px on a 200 px displacement and most fragments stop
    // earlier. Uniform indices are constants for GLSL ES 1.00.
    constexpr const char* kDeformation = R"(uniform vec2 umbriel_deformation[16];
vec2 physics_row(float t, vec2 a, vec2 b, vec2 c, vec2 d) {
  float u = 1.0 - t;
  return u * u * (u * a + 3.0 * t * b) + t * t * (3.0 * u * c + t * d);
}
vec2 physics_offset(vec2 p) {
  p = clamp(p, 0.0, 1.0);
  vec2 a = physics_row(p.x, umbriel_deformation[0], umbriel_deformation[1], umbriel_deformation[2], umbriel_deformation[3]);
  vec2 b = physics_row(p.x, umbriel_deformation[4], umbriel_deformation[5], umbriel_deformation[6], umbriel_deformation[7]);
  vec2 c = physics_row(p.x, umbriel_deformation[8], umbriel_deformation[9], umbriel_deformation[10], umbriel_deformation[11]);
  vec2 d = physics_row(p.x, umbriel_deformation[12], umbriel_deformation[13], umbriel_deformation[14], umbriel_deformation[15]);
  return physics_row(p.y, a, b, c, d);
}
vec4 animation(vec2 uv) {
  vec2 inner = (uv - umbriel_expand) / (1.0 - 2.0 * umbriel_expand);
  vec2 source = inner;
  for (int i = 0; i < 22; i++) {
    vec2 next = inner - physics_offset(source);
    bool done = all(lessThan(abs(next - source), vec2(1e-5)));
    source = next;
    if (done) break;
  }
  return umbriel_sample(source * (1.0 - 2.0 * umbriel_expand) + umbriel_expand);
})";

    // Slide keeps per-buffer alpha: its opacity curve differs from the lifecycle progress.
    bool builtinFadeApplies(const Config::Animation& settings, AnimationEvent event) {
      return settings.enabled
          && ((event == AnimationEvent::WindowsIn && settings.windowsIn.enabled && settings.windowsIn.style != "slide")
              || (event == AnimationEvent::WindowsOut
                  && settings.windowsOut.enabled
                  && settings.windowsOut.style != "slide"));
    }

    fx_effect_kind toFxKind(EffectKind kind) {
      switch (kind) {
      case EffectKind::Animation:
        return FX_EFFECT_ANIMATION;
      case EffectKind::Border:
        return FX_EFFECT_BORDER;
      case EffectKind::Window:
        return FX_EFFECT_WINDOW;
      case EffectKind::Screen:
        return FX_EFFECT_SCREEN;
      case EffectKind::Cursor:
        return FX_EFFECT_CURSOR;
      }
      return FX_EFFECT_ANIMATION;
    }

    template <typename Value>
    void bind(wlr_scene_node* node, AnimationEvent event, const Value& value, float progress, float direction) {
      if (node == nullptr) {
        return;
      }
      fx_animation_parameters parameters{};
      parameters.progress = progress;
      parameters.linear_progress = static_cast<float>(value.progress());
      parameters.direction = direction;
      parameters.transition_id = value.transitionId();
      std::ranges::copy(value.shaderSeed(), parameters.random_seed);
      EffectRegistry& registry = effectRegistry();
      fx_effect_shader* shader = value.animating() ? registry.lifecycleEffect(event) : nullptr;
      const EffectPreset* preset = shader != nullptr ? registry.animationPreset(event) : nullptr;
      if (shader != nullptr && preset != nullptr) {
        registry.fillTimeUniforms(
            parameters, registry.compositionSeconds(node, registry.clockSeconds()), *preset, shader
        );
      }
      registry.setAnimationParameters(node, static_cast<unsigned>(event), shader, parameters, preset);
    }
  } // namespace

  EffectRegistry& effectRegistry() { return *s_registry; }

  EffectRegistry::EffectRegistry(Server& server) : m_server(&server) { s_registry = this; }

  EffectRegistry::~EffectRegistry() {
    clear();
    if (s_registry == this) {
      s_registry = nullptr;
    }
  }

  void EffectRegistry::clear() {
    m_timeInstances.clear();
    for (const auto& [owner, token] : m_sceneTime) {
      (void)owner;
      m_ledger.remove(token.get());
    }
    m_sceneTime.clear();
    m_sourceOccurrences.clear();
    m_compositionOutput = nullptr;
    m_compositionAdvance = false;
    m_submittedEffectTimes.clear();
    m_programs.clear();
    m_scenePrograms.clear();
    m_builtinFade.reset();
    dropDeformation();
    m_persistentReferenced = false;
    m_inPlaceReferenced = false;
    m_cursorActive = false;
    m_renderer = nullptr;
  }

  void EffectRegistry::updateCursorActive() {
    m_cursorActive =
        preset(m_server->cursorEffectSlot().effectiveName(), EffectKind::Cursor) != nullptr && !m_ledger.suspended();
  }

  void EffectRegistry::setSuspended(bool suspended) {
    m_ledger.setSuspended(suspended);

    updateCursorActive();
  }

  void EffectRegistry::removeOutput(const Output* output) {
    m_submittedEffectTimes.erase(output);
    std::erase_if(m_sceneTime, [output](const auto& entry) { return entry.second->output == output; });
    std::erase_if(m_sourceOccurrences, [output](const auto& entry) { return entry.second.output == output; });
    m_ledger.removeOutput(output);

    if (m_pointerWlrOutput == output->wlr()) {
      m_pointerWlrOutput = nullptr;
    }
  }

  void EffectRegistry::referencedNames(std::vector<std::string>& names) const {
    const Config& settings = config();
    const auto add = [&](std::string_view name) {
      if (!name.empty() && name != kEffectOff && std::ranges::find(names, name) == names.end()) {
        names.emplace_back(name);
      }
    };
    add(settings.effects.border);
    add(settings.effects.window);
    add(settings.effects.screen);
    add(settings.effects.cursor);
    for (const WindowRule& rule : settings.windowRules) {
      add(rule.borderEffect.value_or(""));
      add(rule.windowEffect.value_or(""));
    }
    for (const OutputRule& rule : settings.outputs) {
      add(rule.screenEffect.value_or(""));
    }
    for (unsigned slot = 0; slot < FX_ANIMATION_SLOTS; ++slot) {
      const auto binding = settings.animation.eventEffect(static_cast<AnimationEvent>(slot));
      if (binding.effect != nullptr && settings.animation.enabled && binding.enabled) {
        add(*binding.effect);
      }
    }
    for (const std::string& name : configuredEffectActionRoots(settings)) {
      add(name);
    }
    for (const std::string& name : m_server->runtimeEffectSelectors()) {
      add(name);
    }
    for (const EffectPool& pool : settings.effects.pools) {
      if (std::ranges::find(names, pool.name) != names.end()) {
        for (const std::string& member : pool.members) {
          add(member);
        }
      }
    }
    // A referenced border preset pulls its overlay in. Overlays name window presets, which carry none of their own.
    for (const EffectPreset& preset : settings.effects.presets) {
      if (!preset.overlay.empty() && std::ranges::find(names, preset.name) != names.end()) {
        add(preset.overlay);
      }
    }
  }

  void EffectRegistry::compile(const EffectPreset& preset) {
    if (preset.scene) {
      return;
    }
    auto [slot, inserted] = m_programs.try_emplace(preset.name);
    Entry& entry = slot->second;
    if (!inserted && entry.kind == preset.kind && entry.code == preset.shader.code) {
      return;
    }
    entry.kind = preset.kind;
    entry.code = preset.shader.code;
    entry.shader.reset();
    if (preset.inert()) {
      return;
    }
    const std::string label =
        preset.shader.file.empty() ? "effects.preset." + preset.name : preset.shader.file.string();
    entry.shader = {
        fx_effect_shader_create(m_renderer, toFxKind(preset.kind), preset.shader.code.c_str(), label.c_str()),
        fx_effect_shader_unref
    };
    if (entry.shader == nullptr) {
      kLog.error(
          "effect preset '{}' ({}) failed to compile; rendering plainly", preset.name, effectKindName(preset.kind)
      );
    }
  }

  void EffectRegistry::prepare(wlr_renderer* renderer) {
    if (renderer != m_renderer) {
      // Programs belong to one GL context. A new renderer starts from nothing.
      m_programs.clear();
      m_scenePrograms.clear();
      m_builtinFade.reset();
      dropDeformation();
      m_renderer = renderer;
    }
    const Config& settings = config();
    std::vector<std::string> names;
    referencedNames(names);
    std::erase_if(m_programs, [&](const auto& item) {
      const auto* preset = findEffectPreset(settings.effects, item.first);
      return std::ranges::find(names, item.first) == names.end() || preset == nullptr || preset->scene.has_value();
    });
    std::vector<scene_experiment::ProgramDefinition> sceneDefinitions;
    sceneDefinitions.reserve(settings.effects.presets.size());
    for (const auto& preset : settings.effects.presets) {
      if (preset.scene) {
        sceneDefinitions.push_back(
            {.name = preset.name,
             .sources = preset.scene->sources,
             .parameters = preset.scene->parameters,
             .palette = preset.palette}
        );
      }
    }
    m_scenePrograms.prepare(renderer, sceneDefinitions, names);
    for (const std::string& name : names) {
      if (const EffectPreset* preset = findEffectPreset(settings.effects, name)) {
        compile(*preset);
      }
    }
    m_persistentReferenced = std::ranges::any_of(m_programs, [](const auto& item) {
      return item.second.kind != EffectKind::Animation && item.second.shader != nullptr;
    });
    m_inPlaceReferenced = std::ranges::any_of(m_programs, [](const auto& item) {
      const EffectKind kind = item.second.kind;
      return (kind == EffectKind::Window || kind == EffectKind::Screen || kind == EffectKind::Cursor)
          && item.second.shader != nullptr;
    });
    updateCursorActive();
    const bool fadeNeeded = builtinFadeApplies(settings.animation, AnimationEvent::WindowsIn)
        || builtinFadeApplies(settings.animation, AnimationEvent::WindowsOut);
    if (fadeNeeded && m_builtinFade == nullptr) {
      m_builtinFade = {
          fx_effect_shader_create(m_renderer, FX_EFFECT_ANIMATION, kBuiltinFade, "animation.builtin_fade"),
          fx_effect_shader_unref
      };
      fx_effect_shader_set_shape_preserving(m_builtinFade.get(), true);
    } else if (!fadeNeeded) {
      m_builtinFade.reset();
    }
    if (!settings.animation.enabled || !settings.animation.windowsDrag.physics) {
      dropDeformation();
    } else if (!m_deformationCompiled) {
      m_deformationCompiled = true;
      m_deformation = {
          fx_effect_shader_create(m_renderer, FX_EFFECT_ANIMATION, kDeformation, "animation.windows_drag"),
          fx_effect_shader_unref
      };
      if (m_deformation == nullptr) {
        kLog.error("the drag physics program failed to compile; dragged windows stay rigid");
      }
    }
    syncLightLayer();
    applyOutputEffects();
    for (const auto& output : m_server->outputs()) {
      wlr_output_schedule_frame(output->wlr());
    }
  }

  void EffectRegistry::applyOutputEffects() {
    for (const auto& output : m_server->outputs()) {
      output->applyOutputEffects();
    }
  }

  void EffectRegistry::pointerMoved(double lx, double ly, bool visible) {
    for (const auto& output : m_server->outputs()) {
      wlr_scene_output_set_effect_pointer(output->sceneOutput(), lx, ly, visible);
    }
    // The cursor instance's visibility follows the output under the pointer.
    const wlr_output* under = wlr_output_layout_output_at(m_server->outputLayout(), lx, ly);
    if (under != m_pointerWlrOutput || visible != m_pointerVisible) {
      m_pointerWlrOutput = under;
      m_pointerVisible = visible;
      applyOutputEffects();
    }
  }

  void EffectRegistry::updateInstance(const void* owner, const EffectInstanceState& state, wlr_scene_node* node) {

    if (node != nullptr) {
      m_timeInstances[owner] = {.node = node, .state = state};
    } else {
      m_timeInstances.erase(owner);
    }
    updateTimeOccurrence(owner, state, node);
  }

  void EffectRegistry::updateTimeOccurrence(const void* owner, const EffectInstanceState& state, wlr_scene_node* node) {
    std::vector<const void*> wasIdle;
    for (const auto& output : m_server->outputs()) {
      if (m_ledger.eligible(output.get()) == 0) {
        wasIdle.push_back(output.get());
      }
    }
    if (node != nullptr) {
      m_ledger.updateOccurrences(owner, state, compositionOutputs(node));
    } else {
      m_ledger.update(owner, state);
    }
    for (const auto& output : m_server->outputs()) {
      if (std::ranges::find(wasIdle, output.get()) != wasIdle.end() && m_ledger.eligible(output.get()) > 0) {
        output->scheduleEffectFrame();
      }
    }
  }

  void EffectRegistry::refreshTimeOccurrences() {
    for (const auto& [owner, instance] : m_timeInstances) {
      updateTimeOccurrence(owner, instance.state, instance.node);
    }
  }

  void EffectRegistry::removeInstance(const void* owner) {
    m_timeInstances.erase(owner);
    m_ledger.remove(owner);
  }

  void EffectRegistry::updateSceneTime(
      const void* owner, Output* output, const scene_experiment::ProgramBundle* bundle, bool eligible
  ) {
    if (owner == nullptr) {
      return;
    }
    if (output == nullptr || bundle == nullptr || !bundle->readsTime) {
      clearSceneTime(owner);
      return;
    }
    auto& token = m_sceneTime[owner];
    if (!token) {
      token = std::make_unique<SceneTimeOwner>();
    }
    token->output = output;
    token->readsTime = bundle->readsTime && eligible;
    bool advancing = true;
#ifdef UMBRIEL_TEST_IPC
    advancing = !m_server->animationClockFrozen();
#endif
    updateTimeOccurrence(
        token.get(),
        {.output = output, .visible = output->wlr()->enabled, .readsTime = token->readsTime, .advancing = advancing},
        nullptr
    );
  }

  void EffectRegistry::clearSceneTime(const void* owner) {
    const auto found = m_sceneTime.find(owner);
    if (found != m_sceneTime.end()) {
      m_ledger.remove(found->second.get());

      m_sceneTime.erase(found);
    }
  }

  void EffectRegistry::fillScenePalette(fx_scene_frame& frame, const scene_experiment::ProgramBundle& bundle) const {
    std::ranges::fill(frame.palette, 0.0F);
    frame.palette_count = 0;
    if (bundle.definition.palette) {
      const auto palette = effectPalette(config().colors);
      for (size_t i = 0; i < palette.size(); ++i) {
        std::ranges::copy(palette[i], &frame.palette[i * 4]);
      }
      frame.palette_count = static_cast<int>(palette.size());
    }
  }

  void EffectRegistry::beginCompositionFrame(const Output* output, bool advance) {
    bool frozen = false;
#ifdef UMBRIEL_TEST_IPC
    frozen = m_server->animationClockFrozen();
#endif

    for (const auto& [owner, token] : m_sceneTime) {
      (void)owner;
      updateTimeOccurrence(
          token.get(),
          {.output = token->output,
           .visible = token->output->wlr()->enabled,
           .readsTime = token->readsTime,
           .advancing = !frozen},
          nullptr
      );
    }
    m_compositionOutput = output;
    // Explicit frozen-clock steps also change TIME, even though the periodic
    // effect clock is stopped. Compare against this output's last submitted
    // instant, preserving damage across failed submissions and quiet rebinds
    // of a shared node for another output.
    const auto submitted = m_submittedEffectTimes.find(output);
    m_compositionAdvance =
        advance || submitted == m_submittedEffectTimes.end() || submitted->second != output->effectSeconds();
    // Ordinary frames without authored node effects need no view traversal.
    if (m_timeInstances.empty() && m_sourceOccurrences.empty()) {
      return;
    }

    // The animation clock may be frozen or already ticked by another output.
    // Rebind visible timed effects for this output before source composition.
    for (const auto& view : m_server->views()) {
      if (view->mapped()) {
        view->syncAnimationEffects();
      }
    }
  }

  void EffectRegistry::finishCompositionFrame(const Output* output, bool success) {
    if (success) {
      m_submittedEffectTimes[output] = output->effectSeconds();
    }

    m_compositionOutput = nullptr;
    m_compositionAdvance = false;
  }

  bool EffectRegistry::compositionNodeVisible(wlr_scene_node* node, const Output* output) const {
    if (sourceNodeVisible(node, output)) {
      return true;
    }
    const auto box = output->layoutBox();
    if (!wlr_scene_node_visible_in_box(node, &box)) {
      return false;
    }
    const bool replaced = std::ranges::any_of(m_sourceOccurrences, [output](const auto& entry) {
      return entry.second.output == output && entry.second.replacesNativeViews;
    });
    if (!replaced) {
      return true;
    }
    for (const auto& view : m_server->views()) {
      if (view->sceneTree() == nullptr) {
        continue;
      }
      for (auto* ancestor = node; ancestor != nullptr;
           ancestor = ancestor->parent != nullptr ? &ancestor->parent->node : nullptr) {
        if (ancestor == &view->sceneTree()->node) {
          return false;
        }
      }
    }
    return true;
  }

  bool EffectRegistry::sourceNodeVisible(wlr_scene_node* node, const Output* output) const {
    for (const auto& [session, occurrence] : m_sourceOccurrences) {
      (void)session;
      if (occurrence.output != output) {
        continue;
      }
      // Source inventories retain map-lifetime identities, never View pointers.
      // Unmap/destruction cannot turn a stale occurrence into a different view.
      for (const auto& view : m_server->views()) {
        if (!view->mapped()
            || view->sceneTree() == nullptr
            || view->extForeignIdentifier() == nullptr
            || std::ranges::find(occurrence.views, view->extForeignIdentifier()) == occurrence.views.end()) {
          continue;
        }
        for (auto* ancestor = node; ancestor != nullptr;
             ancestor = ancestor->parent != nullptr ? &ancestor->parent->node : nullptr) {
          if (ancestor == &view->sceneTree()->node) {
            return true;
          }
        }
      }
    }
    return false;
  }

  std::vector<const void*> EffectRegistry::compositionOutputs(wlr_scene_node* node) const {
    std::vector<const void*> outputs;
    for (const auto& output : m_server->outputs()) {
      if (output->wlr()->enabled && compositionNodeVisible(node, output.get())) {
        outputs.push_back(output.get());
      }
    }
    return outputs;
  }

  void EffectRegistry::setSourceOccurrences(
      const void* session, Output* output, std::span<View* const> views, bool replacesNativeViews
  ) {
    if (session == nullptr || output == nullptr) {
      return;
    }
    SourceOccurrence occurrence{.output = output, .views = {}, .replacesNativeViews = replacesNativeViews};
    for (const auto* view : views) {
      if (view != nullptr && view->mapped() && view->extForeignIdentifier() != nullptr) {
        occurrence.views.emplace_back(view->extForeignIdentifier());
      }
    }
    m_sourceOccurrences[session] = std::move(occurrence);
    refreshTimeOccurrences();
  }

  void EffectRegistry::clearSourceOccurrences(const void* session) {
    if (m_sourceOccurrences.erase(session) != 0) {
      refreshTimeOccurrences();
    }
  }

  void EffectRegistry::bindSourceTime(const void* session) {
    const auto found = m_sourceOccurrences.find(session);
    if (found == m_sourceOccurrences.end()) {
      return;
    }
    const auto* previousOutput = m_compositionOutput;
    const bool previousAdvance = m_compositionAdvance;
    m_compositionOutput = found->second.output;
    // Source acquisition can precede the first output frame. It uses the held
    // output input, never advances or acknowledges it independently.
    m_compositionAdvance = false;
    for (const auto& view : m_server->views()) {
      if (view->mapped()
          && view->extForeignIdentifier() != nullptr
          && std::ranges::find(found->second.views, view->extForeignIdentifier()) != found->second.views.end()) {
        view->syncAnimationEffects();
      }
    }
    m_compositionOutput = previousOutput;
    m_compositionAdvance = previousAdvance;
  }

  void EffectRegistry::retainRequirements(const fx_effect_requirements& requirements) {
    m_retainedPersistent += requirements.persistent ? 1U : 0U;
    m_retainedInPlace += requirements.in_place ? 1U : 0U;
    m_retainedLight += requirements.light ? 1U : 0U;
    if (requirements.light) {
      syncLightLayer();
    }
  }

  void EffectRegistry::releaseRequirements(const fx_effect_requirements& requirements) {
    m_retainedPersistent -= requirements.persistent ? 1U : 0U;
    m_retainedInPlace -= requirements.in_place ? 1U : 0U;
    m_retainedLight -= requirements.light ? 1U : 0U;
    if (requirements.light) {
      syncLightLayer();
    }
  }

  void EffectRegistry::syncLightLayer() {
    const bool lit = std::ranges::any_of(config().effects.presets, [this](const EffectPreset& entry) {
      return entry.light && preset(entry.name, EffectKind::Border) != nullptr;
    });
    m_server->setEffectLightLayer(lit || m_retainedLight > 0);
  }

  fx_effect_shader* EffectRegistry::preset(std::string_view name, EffectKind kind) const {
    const auto entry = m_programs.find(name);
    return entry != m_programs.end() && entry->second.kind == kind ? entry->second.shader.get() : nullptr;
  }

  std::string_view EffectRegistry::programState(std::string_view name) const {
    const EffectPreset* configured = presetConfig(name);
    if (configured == nullptr || configured->inert()) {
      return "inert";
    }
    if (configured->scene) {
      switch (sceneProgramState(name)) {
      case scene_experiment::ProgramState::Unreferenced:
        return "unreferenced";
      case scene_experiment::ProgramState::Invalid:
        return "inert";
      case scene_experiment::ProgramState::Unsupported:
        return "unsupported";
      case scene_experiment::ProgramState::CompileFailed:
        return "failed";
      case scene_experiment::ProgramState::Ready:
        return "compiled";
      }
    }
    const auto entry = m_programs.find(name);
    if (entry == m_programs.end()
        || entry->second.kind != configured->kind
        || entry->second.code != configured->shader.code) {
      return "unreferenced";
    }
    return entry->second.shader != nullptr ? "compiled" : "failed";
  }

  scene_experiment::ProgramState EffectRegistry::sceneProgramState(std::string_view name) const {
    const auto* configured = presetConfig(name);
    if (configured == nullptr || !configured->scene) {
      return scene_experiment::ProgramState::Unreferenced;
    }
    if (configured->inert()) {
      return scene_experiment::ProgramState::Invalid;
    }
    return m_scenePrograms.state(name);
  }

  std::shared_ptr<const scene_experiment::ProgramBundle>
  EffectRegistry::scenePreset(std::string_view name, scene_experiment::Scope scope) const {
    const auto* configured = presetConfig(name);
    if (configured == nullptr
        || !configured->scene
        || configured->kind != EffectKind::Animation
        || configured->scene->sources.scope != scope) {
      return {};
    }
    auto bundle = m_scenePrograms.find(name);
    if (!bundle
        || bundle->definition.sources != configured->scene->sources
        || bundle->definition.parameters != configured->scene->parameters
        || bundle->definition.palette != configured->palette) {
      return {};
    }
    return bundle;
  }

  std::shared_ptr<const scene_experiment::ProgramBundle>
  EffectRegistry::sceneAnimationEffect(AnimationEvent event) const {
    const auto& settings = config().animation;
    const auto binding = settings.eventEffect(event);
    if (!settings.enabled || !binding.enabled || binding.effect == nullptr || binding.effect->empty()) {
      return {};
    }
    if (event == AnimationEvent::Workspaces) {
      return scenePreset(*binding.effect, scene_experiment::Scope::WorkspacePair);
    }

    return {};
  }

  const EffectPreset* EffectRegistry::presetConfig(std::string_view name) const {
    return findEffectPreset(config().effects, name);
  }

  fx_effect_shader* EffectRegistry::animationEffect(AnimationEvent event) const {
    const Config::Animation& settings = config().animation;
    const auto binding = settings.eventEffect(event);
    if (binding.effect == nullptr || !settings.enabled || !binding.enabled || binding.effect->empty()) {
      return nullptr;
    }
    return preset(*binding.effect, EffectKind::Animation);
  }

  fx_effect_shader* EffectRegistry::lifecycleEffect(AnimationEvent event) const {
    if (fx_effect_shader* custom = animationEffect(event)) {
      return custom;
    }
    return builtinFadeApplies(config().animation, event) ? m_builtinFade.get() : nullptr;
  }

  void EffectRegistry::dropDeformation() {
    m_deformation.reset();
    m_deformationCompiled = false;
  }

  fx_effect_shader* EffectRegistry::deformationShader() const {
    const Config::Animation& settings = config().animation;
    return settings.enabled && settings.windowsDrag.physics ? m_deformation.get() : nullptr;
  }

  const EffectPreset* EffectRegistry::animationPreset(AnimationEvent event) const {
    const auto binding = config().animation.eventEffect(event);
    return binding.effect != nullptr && !binding.effect->empty() ? findEffectPreset(config().effects, *binding.effect)
                                                                 : nullptr;
  }

  float EffectRegistry::clockSeconds() const {
    const uint64_t now = m_server->animationClockMsec();
    if (!m_clockEpochSet) {
      // The server isn't constructed far enough for animationClockMsec() to be safe from this
      // registry's own constructor, so the epoch is taken lazily on first use instead.
      m_clockEpochMsec = now;
      m_clockEpochSet = true;
    }
    return effectClockSeconds(now, m_clockEpochMsec);
  }

  float EffectRegistry::compositionSeconds(wlr_scene_node* node, float fallback) const {
    if (node != nullptr && m_compositionOutput != nullptr) {
      for (const auto& output : m_server->outputs()) {
        if (output.get() == m_compositionOutput && compositionNodeVisible(node, output.get())) {
          return output->effectSeconds();
        }
      }
    }
    return fallback;
  }

  void EffectRegistry::fillTimeUniforms(
      fx_animation_parameters& parameters, float seconds, const EffectPreset& preset, const fx_effect_shader* shader
  ) const {
    if (fx_effect_shader_reads(shader, "umbriel_time")) {
      if (fx_uniform* time = fx_parameters_add_uniform(&parameters, "umbriel_time", FX_UNIFORM_FLOAT, 1)) {
        time->floats[0] = seconds;
      }
    }

    if (!preset.palette) {
      return;
    }
    const auto palette = effectPalette(config().colors);
    if (fx_uniform* colors = fx_parameters_add_uniform(&parameters, "umbriel_palette", FX_UNIFORM_VEC4, 4)) {
      for (size_t i = 0; i < palette.size(); ++i) {
        std::ranges::copy(palette[i], &colors->floats[i * 4]);
      }
    }
    if (fx_uniform* count = fx_parameters_add_uniform(&parameters, "umbriel_palette_count", FX_UNIFORM_INT, 1)) {
      count->ints[0] = static_cast<int32_t>(palette.size());
    }
  }

  void EffectRegistry::setAnimationParameters(
      wlr_scene_node* node, unsigned slot, fx_effect_shader* shader, const fx_animation_parameters& parameters,
      const EffectPreset* preset
  ) const {
    if (m_compositionOutput != nullptr && preset != nullptr && fx_effect_shader_reads(shader, "umbriel_time")) {
      const bool damage = m_compositionAdvance;
      for (const auto& output : m_server->outputs()) {
        if (output.get() == m_compositionOutput
            && wlr_scene_node_set_animation_uniforms_for_output(
                node, slot, shader, &parameters, output->sceneOutput(),
                damage && compositionNodeVisible(node, output.get())
            )) {
          return;
        }
      }
    }
    wlr_scene_node_set_animation(node, slot, shader, &parameters);
  }

  void bindAnimationEffect(wlr_scene_node* node, AnimationEvent event, const AnimatedValue& value, float direction) {
    const double distance = value.target() - value.from();
    const auto progress = static_cast<float>(
        distance != 0.0 ? (value.current() - value.from()) / distance : evaluateCurve(value.curve(), value.progress())
    );
    bind(node, event, value, progress, direction != 0.0F ? direction : (distance < 0.0 ? -1.0F : 1.0F));
  }

  void bindAnimationEffect(wlr_scene_node* node, AnimationEvent event, const AnimatedColor& value, float direction) {
    bind(node, event, value, static_cast<float>(evaluateCurve(value.curve(), value.progress())), direction);
  }

} // namespace umbriel
