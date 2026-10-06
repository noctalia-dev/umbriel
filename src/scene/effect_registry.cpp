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
      const void* audioOutput = registry.updateAnimationAudio(node, static_cast<unsigned>(event), preset, shader);
      // Built-in fades have no preset and therefore no audio demand.
      if (shader != nullptr && preset != nullptr) {
        registry.fillTimeUniforms(parameters, registry.clockSeconds(), *preset, shader, audioOutput, node);
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
    for (auto& [key, instance] : m_animationAudio) {
      wl_list_remove(&instance->destroy.link);
    }
    m_animationAudio.clear();
    m_audioInstances.clear();
    m_timeInstances.clear();
    if (m_audioSessionActive.link.next != nullptr) {
      wl_list_remove(&m_audioSessionActive.link);
      m_audioSessionActive.link.next = nullptr;
    }
    m_audio.reset();
    m_audioOutput = nullptr;
    m_audioAdvance = false;
    m_submittedEffectTimes.clear();
    m_programs.clear();
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
    if (m_audio) {
      m_audio->setSessionActive(!suspended && (m_server->session() == nullptr || m_server->session()->active));
    }
    updateCursorActive();
  }

  void EffectRegistry::removeOutput(const Output* output) {
    m_submittedEffectTimes.erase(output);
    m_ledger.removeOutput(output);
    if (m_audio) {
      m_audio->removeOutput(output);
    }
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
      m_builtinFade.reset();
      dropDeformation();
      m_renderer = renderer;
    }
    const Config& settings = config();
    if (!settings.effects.audioSources.empty() && !m_audio) {
      m_audio = std::make_unique<audio::Bindings>(wl_display_get_event_loop(m_server->display()));
      m_audio->setSessionActive(
          !m_ledger.suspended() && (m_server->session() == nullptr || m_server->session()->active)
      );
      if (m_server->session() != nullptr && m_audioSessionActive.link.next == nullptr) {
        m_audioSessionActive.notify = [](wl_listener* listener, void*) {
          EffectRegistry* self;
          self = wl_container_of(listener, self, m_audioSessionActive);
          if (self->m_audio) {
            self->m_audio->setSessionActive(!self->m_ledger.suspended() && self->m_server->session()->active);
          }
        };
        wl_signal_add(&m_server->session()->events.active, &m_audioSessionActive);
      }
      m_audio->changed = [this](const void* target) {
        if (!audioDirty(target)) {
          return;
        }
        for (const auto& output : m_server->outputs()) {
          if (output.get() == target) {
            output->scheduleAudioFrame();
          }
        }
        if (const auto capture = m_audioCaptures.find(target); capture != m_audioCaptures.end()) {
          capture->second();
        }
      };
    }
    if (m_audio) {
      m_audio->configure(settings.effects.audioSources);
    }
    std::vector<std::string> names;
    referencedNames(names);
    std::erase_if(m_programs, [&](const auto& item) {
      const auto* preset = findEffectPreset(settings.effects, item.first);
      return std::ranges::find(names, item.first) == names.end() || preset == nullptr;
    });
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
    // History that starts here needs its instance re-bound so it asks for fade frames; while it lasts, those frames
    // re-bind it and drop the request once it expires.
    bool motionStarted = false;
    for (const auto& output : m_server->outputs()) {
      wlr_scene_output* sceneOutput = output->sceneOutput();
      const bool motionBefore = wlr_scene_output_cursor_motion_active(sceneOutput);
      wlr_scene_output_set_effect_time(sceneOutput, m_server->animationClockMsec());
      wlr_scene_output_set_effect_pointer(sceneOutput, lx, ly, visible);
      motionStarted = motionStarted || (!motionBefore && wlr_scene_output_cursor_motion_active(sceneOutput));
    }
    // The cursor instance's visibility follows the output under the pointer.
    const wlr_output* under = wlr_output_layout_output_at(m_server->outputLayout(), lx, ly);
    if (under != m_pointerWlrOutput || visible != m_pointerVisible || motionStarted) {
      m_pointerWlrOutput = under;
      m_pointerVisible = visible;
      applyOutputEffects();
    }
  }

  void EffectRegistry::updateInstance(
      const void* owner, const EffectInstanceState& state, std::string_view source, wlr_scene_node* node
  ) {
    if (m_audio) {
      if (node != nullptr && !source.empty()) {
        m_audioInstances[owner] = {.node = node, .source = std::string(source)};
        m_audio->updateOccurrences(owner, audioOutputs(node), source, !m_ledger.suspended());
      } else {
        m_audioInstances.erase(owner);
        m_audio->update(owner, state.output, source, state.visible && !m_ledger.suspended());
      }
    }
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
      m_ledger.updateOccurrences(owner, state, audioOutputs(node));
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
    m_audioInstances.erase(owner);
    m_ledger.remove(owner);
    if (m_audio) {
      m_audio->remove(owner);
    }
  }

  std::string_view EffectRegistry::audioSource(const EffectPreset& preset, const fx_effect_shader* shader) {
    return !preset.audio.empty()
            && shader != nullptr
            && (fx_effect_shader_reads(shader, "umbriel_audio_levels")
                || fx_effect_shader_reads(shader, "umbriel_audio_bands"))
        ? preset.audio
        : std::string_view{};
  }

  const void* EffectRegistry::updateAnimationAudio(
      wlr_scene_node* node, unsigned slot, const EffectPreset* preset, const fx_effect_shader* shader
  ) {
    const auto key = std::pair(node, slot);
    const std::string_view source = preset != nullptr ? audioSource(*preset, shader) : std::string_view{};
    auto found = m_animationAudio.find(key);
    if (!m_audio || source.empty()) {
      if (found != m_animationAudio.end()) {
        if (m_audio) {
          m_audio->remove(found->second.get());
        }
        wl_list_remove(&found->second->destroy.link);
        m_animationAudio.erase(found);
      }
      return nullptr;
    }
    if (found == m_animationAudio.end()) {
      auto instance = std::make_unique<AnimationAudio>();
      instance->registry = this;
      instance->node = node;
      instance->slot = slot;
      instance->destroy.notify = [](wl_listener* listener, void*) {
        AnimationAudio* self;
        self = wl_container_of(listener, self, destroy);
        (void)self->registry->updateAnimationAudio(self->node, self->slot, nullptr, nullptr);
      };
      wl_signal_add(&node->events.destroy, &instance->destroy);
      found = m_animationAudio.emplace(key, std::move(instance)).first;
    }
    found->second->source = source;
    const auto visible = audioOutputs(node);
    m_audio->updateOccurrences(found->second.get(), visible, source, !m_ledger.suspended());
    return visible.empty() ? nullptr : visible.front();
  }

  uint64_t EffectRegistry::audioInputRevision(const void* output) const {
    return m_audio ? m_audio->inputRevision(output) : 0;
  }

  bool EffectRegistry::audioActive(const void* output) const { return m_audio && m_audio->active(output); }

  bool EffectRegistry::audioDirty(const void* output) const {
    if (!m_audio || m_ledger.suspended()) {
      return false;
    }
#ifdef UMBRIEL_TEST_IPC
    if (m_server->animationClockFrozen() && !m_audio->injected(output)) {
      return false;
    }
#endif
    return m_audio->dirty(output);
  }

  void EffectRegistry::beginAudioFrame(const Output* output, bool advance) {
    bool frozen = false;
#ifdef UMBRIEL_TEST_IPC
    frozen = m_server->animationClockFrozen();
#endif
    if (m_audio && m_audioWasFrozen && !frozen) {
      m_audio->clearInjections();
    }
    m_audioWasFrozen = frozen;
    m_audioOutput = output;
    // Explicit frozen-clock steps also change TIME, even though the periodic
    // effect clock is stopped. Compare against this output's last submitted
    // instant, preserving damage across failed submissions and quiet rebinds
    // of a shared node for another output.
    const auto submitted = m_submittedEffectTimes.find(output);
    m_audioAdvance =
        advance || submitted == m_submittedEffectTimes.end() || submitted->second != output->effectSeconds();
    // A view may unmap while its role object survives. Node destruction alone
    // is not a sufficient visibility boundary for a finite-slot occurrence.
    for (const auto& [key, instance] : m_animationAudio) {
      if (!audioNodeVisible(instance->node, output)) {
        bool elsewhere = false;
        for (const auto& other : m_server->outputs()) {
          elsewhere = elsewhere || (other->wlr()->enabled && audioNodeVisible(instance->node, other.get()));
        }
        if (!elsewhere) {
          m_audio->remove(instance.get());
        }
      }
    }
    if (m_audio) {
      m_audio->begin(output, advance, frozen);
    }
    // The animation clock may be frozen or already ticked by another output.
    // Rebind audio consumers explicitly from the immutable output latch.
    for (const auto& view : m_server->views()) {
      if (view->mapped()) {
        view->syncAnimationEffects();
      }
    }
  }

  void EffectRegistry::finishAudioFrame(const Output* output, bool success) {
    if (success) {
      m_submittedEffectTimes[output] = output->effectSeconds();
    }
    if (m_audio) {
      m_audio->finish(output, success);
    }
    m_audioOutput = nullptr;
    m_audioAdvance = false;
  }

  bool EffectRegistry::audioNodeVisible(wlr_scene_node* node, const Output* output) const {
    const auto box = output->layoutBox();
    return wlr_scene_node_visible_in_box(node, &box);
  }

  std::vector<const void*> EffectRegistry::audioOutputs(wlr_scene_node* node) const {
    std::vector<const void*> outputs;
    for (const auto& output : m_server->outputs()) {
      if (output->wlr()->enabled && audioNodeVisible(node, output.get())) {
        outputs.push_back(output.get());
      }
    }
    return outputs;
  }

  void EffectRegistry::refreshAudioOccurrences() {
    if (!m_audio) {
      return;
    }
    m_audio->beginUpdate();
    for (const auto& [owner, instance] : m_audioInstances) {
      m_audio->updateOccurrences(owner, audioOutputs(instance.node), instance.source, !m_ledger.suspended());
    }
    for (const auto& [key, instance] : m_animationAudio) {
      (void)key;
      m_audio->updateOccurrences(instance.get(), audioOutputs(instance->node), instance->source, !m_ledger.suspended());
    }
    m_audio->endUpdate();
  }

  void EffectRegistry::registerAudioCapture(const void* capture, std::function<void()> schedule) {
    m_audioCaptures[capture] = std::move(schedule);
  }

  void EffectRegistry::removeAudioCapture(const void* capture) {
    m_audioCaptures.erase(capture);
    if (m_audio) {
      m_audio->removeOutput(capture);
    }
  }

  void
  EffectRegistry::updateCaptureAudio(const void* owner, const void* capture, std::string_view source, bool eligible) {
    if (m_audio) {
      m_audio->update(owner, capture, source, eligible && !m_ledger.suspended());
    }
  }

  void EffectRegistry::beginAudioCapture(const void* capture, bool advance) {
    if (!m_audio) {
      return;
    }
    bool frozen = false;
#ifdef UMBRIEL_TEST_IPC
    frozen = m_server->animationClockFrozen();
#endif
    if (m_audio && m_audioWasFrozen && !frozen) {
      m_audio->clearInjections();
    }
    m_audioWasFrozen = frozen;
    m_audio->begin(capture, advance, frozen);
  }

  void EffectRegistry::finishAudioCapture(const void* capture, bool success) {
    if (m_audio) {
      m_audio->finish(capture, success);
    }
  }

  void EffectRegistry::resumeAudioClock() {
    m_audioWasFrozen = false;
    if (m_audio) {
      m_audio->clearInjections();
    }
  }

  bool EffectRegistry::injectAudio(std::string_view source, const audio::Features& features) {
    return m_audio && m_audio->inject(source, features);
  }

  std::string_view EffectRegistry::audioState(std::string_view source) const {
    return m_audio ? m_audio->state(source) : "unused";
  }

  const audio::Receiver* EffectRegistry::inspectAudio(std::string_view source) const {
    return m_audio ? m_audio->inspect(source) : nullptr;
  }

  size_t EffectRegistry::audioDemandedSources() const { return m_audio ? m_audio->demandedSources() : 0; }

  const audio::InputLatch* EffectRegistry::inspectAudioLatch(const void* output, std::string_view source) const {
    return m_audio ? m_audio->inspectLatch(output, source) : nullptr;
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
    const auto entry = m_programs.find(name);
    if (entry == m_programs.end()
        || entry->second.kind != configured->kind
        || entry->second.code != configured->shader.code) {
      return "unreferenced";
    }
    return entry->second.shader != nullptr ? "compiled" : "failed";
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
    if (node != nullptr && m_audioOutput != nullptr) {
      for (const auto& output : m_server->outputs()) {
        if (output.get() == m_audioOutput && audioNodeVisible(node, output.get())) {
          return output->effectSeconds();
        }
      }
    }
    return fallback;
  }

  void EffectRegistry::fillTimeUniforms(
      fx_animation_parameters& parameters, float seconds, const EffectPreset& preset, const fx_effect_shader* shader,
      const void* output, wlr_scene_node* node
  ) const {
    if (fx_effect_shader_reads(shader, "umbriel_time")) {
      if (fx_uniform* time = fx_parameters_add_uniform(&parameters, "umbriel_time", FX_UNIFORM_FLOAT, 1)) {
        time->floats[0] = seconds;
      }
    }
    if (!audioSource(preset, shader).empty()) {
      // Node slots are shared by their output occurrences. Immediately before
      // each composition, bind that composition's immutable latch, including
      // when a straddling node's authoritative output is another output.
      if (node != nullptr && m_audioOutput != nullptr) {
        for (const auto& candidate : m_server->outputs()) {
          if (candidate.get() == m_audioOutput && audioNodeVisible(node, candidate.get())) {
            output = m_audioOutput;
            break;
          }
        }
      }
      const audio::Input input =
          m_audio ? m_audio->input(output != nullptr ? output : m_audioOutput, preset.audio) : audio::Input{};
      if (fx_uniform* levels = fx_parameters_add_uniform(&parameters, "umbriel_audio_levels", FX_UNIFORM_VEC4, 1)) {
        std::ranges::copy(input.levels(), levels->floats);
      }
      if (fx_uniform* bands = fx_parameters_add_uniform(&parameters, "umbriel_audio_bands", FX_UNIFORM_VEC4, 4)) {
        std::ranges::copy(input.features.bands, bands->floats);
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
    if (m_audioOutput != nullptr
        && preset != nullptr
        && (!audioSource(*preset, shader).empty() || fx_effect_shader_reads(shader, "umbriel_time"))) {
      const auto* latch = m_audio ? m_audio->inspectLatch(m_audioOutput, preset->audio) : nullptr;
      const bool damage =
          (latch != nullptr && latch->pending()) || (m_audioAdvance && fx_effect_shader_reads(shader, "umbriel_time"));
      for (const auto& output : m_server->outputs()) {
        if (output.get() == m_audioOutput
            && wlr_scene_node_set_animation_uniforms_for_output(
                node, slot, shader, &parameters, output->sceneOutput(), damage && audioNodeVisible(node, output.get())
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
