#pragma once

#include "audio/bindings.h"
#include "config/effects.h"
#include "core/animation.h"
#include "scene/effect_ledger.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-server-core.h>

struct fx_effect_shader;
struct fx_effect_requirements;
struct fx_animation_parameters;
struct wlr_output;
struct wlr_scene_node;
struct wlr_renderer;

namespace umbriel {

  class Output;
  class Server;
  class View;

  // One compiled program per referenced preset for the current renderer, plus
  // the built-in lifecycle fade. Prepares at startup, on reload with the
  // effects or animation flags, and after renderer recovery; never compiles in
  // a render callback. Failures are cached as null with a diagnostic.
  class EffectRegistry {
  public:
    explicit EffectRegistry(Server& server);
    ~EffectRegistry();
    EffectRegistry(const EffectRegistry&) = delete;
    EffectRegistry& operator=(const EffectRegistry&) = delete;

    void prepare(wlr_renderer* renderer);
    void clear();
    [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }

    // The program for `name`, or null when the preset is off, inert, of another kind, or failed to compile.
    [[nodiscard]] fx_effect_shader* preset(std::string_view name, EffectKind kind) const;
    [[nodiscard]] const EffectPreset* presetConfig(std::string_view name) const;
    [[nodiscard]] std::string_view programState(std::string_view name) const;
    // The preset bound to an animation event through `effect =`, or null.
    [[nodiscard]] fx_effect_shader* animationEffect(AnimationEvent event) const;
    // The program a lifecycle fade composes through: the event's preset, or for windows_in and windows_out without
    // one, a built-in fade that applies the lifecycle alpha to the whole window at once. Null when buffers fade
    // individually; prepare() compiles every program, never this lookup.
    [[nodiscard]] fx_effect_shader* lifecycleEffect(AnimationEvent event) const;
    // The drag slot's built-in deformation program, compiled by prepare() for the current renderer. Null while
    // animations or windows_drag.physics are off, or when it failed to compile.
    [[nodiscard]] fx_effect_shader* deformationShader() const;
    // The preset bound to an animation event through `effect =`, or null (also null for the built-in fade).
    [[nodiscard]] const EffectPreset* animationPreset(AnimationEvent event) const;
    // The animation clock in seconds, read only when a program needs it. Measured from an epoch
    // taken on the first call, so the millisecond count backing it can exceed a float's exact
    // integer range without the returned value losing precision.
    [[nodiscard]] float clockSeconds() const;
    // Adds `umbriel_time` when `shader` reads it, and the `[colors]` palette for palette presets.
    [[nodiscard]] float compositionSeconds(wlr_scene_node* node, float fallback) const;
    void fillTimeUniforms(
        fx_animation_parameters& parameters, float seconds, const EffectPreset& preset, const fx_effect_shader* shader,
        const void* output = nullptr, wlr_scene_node* node = nullptr
    ) const;
    // Uniform-only composition rebinding must not invalidate another output's
    // occurrence. Program/lifecycle/geometry changes use normal scene damage.
    void setAnimationParameters(
        wlr_scene_node* node, unsigned slot, fx_effect_shader* shader, const fx_animation_parameters& parameters,
        const EffectPreset* preset
    ) const;

    [[nodiscard]] bool active() const { return m_ledger.active() > 0; }
    // Prepared programs and retained snapshots both keep rendering requirements alive.
    [[nodiscard]] bool persistentReferenced() const { return m_persistentReferenced || m_retainedPersistent > 0; }
    [[nodiscard]] bool inPlaceReferenced() const { return m_inPlaceReferenced || m_retainedInPlace > 0; }
    // Pointer motion reaches outputs only while the selected cursor effect is active.
    [[nodiscard]] bool cursorEffectActive() const { return m_cursorActive; }
    [[nodiscard]] EffectLedger& ledger() { return m_ledger; }
    void setSuspended(bool suspended);
    // Records an instance; schedules its output's effect frame when that output gains its first eligible instance.
    void updateInstance(
        const void* owner, const EffectInstanceState& state, std::string_view audioSource = {},
        wlr_scene_node* node = nullptr
    );
    [[nodiscard]] static std::string_view audioSource(const EffectPreset& preset, const fx_effect_shader* shader);
    // Finite animation slots retain a demand occurrence only while the node/slot
    // exists and the authored program uses audio. Returns its visible output.
    [[nodiscard]] const void* updateAnimationAudio(
        wlr_scene_node* node, unsigned slot, const EffectPreset* preset, const fx_effect_shader* shader
    );
    [[nodiscard]] uint64_t audioInputRevision(const void* output) const;
    [[nodiscard]] bool audioActive(const void* output) const;
    [[nodiscard]] bool audioDirty(const void* output) const;
    void beginAudioFrame(const Output* output, bool advance);
    void finishAudioFrame(const Output* output, bool success);
    void registerAudioCapture(const void* capture, std::function<void()> schedule);
    void removeAudioCapture(const void* capture);
    void updateCaptureAudio(const void* owner, const void* capture, std::string_view source, bool eligible);
    void beginAudioCapture(const void* capture, bool advance);
    void finishAudioCapture(const void* capture, bool success);
    void resumeAudioClock();
    [[nodiscard]] bool injectAudio(std::string_view source, const audio::Features& features);
    [[nodiscard]] std::string_view audioState(std::string_view source) const;
    [[nodiscard]] const audio::Receiver* inspectAudio(std::string_view source) const;
    [[nodiscard]] size_t audioDemandedSources() const;
    [[nodiscard]] const audio::InputLatch* inspectAudioLatch(const void* output, std::string_view source) const;

    void removeInstance(const void* owner);
    void removeOutput(const Output* output);
    // Keep the light layer while prepared programs or snapshots require it.
    void syncLightLayer();
    void retainRequirements(const fx_effect_requirements& requirements);
    void releaseRequirements(const fx_effect_requirements& requirements);
    // Pushes the output-level effect settings to every output.
    void applyOutputEffects();
    // Forwards the pointer to every output's cursor slot; call only while cursorEffectActive().
    void pointerMoved(double lx, double ly, bool visible);
    // The output layout changed: the next pointerMoved() re-applies every output.
    void forgetPointerOutput() { m_pointerWlrOutput = nullptr; }

  private:
    struct Entry {
      EffectKind kind = EffectKind::Animation;
      std::string code;
      std::shared_ptr<fx_effect_shader> shader; // null once compilation failed
    };
    void compile(const EffectPreset& preset);
    void referencedNames(std::vector<std::string>& names) const;
    void updateCursorActive();
    [[nodiscard]] bool audioNodeVisible(wlr_scene_node* node, const Output* output) const;
    [[nodiscard]] std::vector<const void*> audioOutputs(wlr_scene_node* node) const;
    void refreshAudioOccurrences();
    void updateTimeOccurrence(const void* owner, const EffectInstanceState& state, wlr_scene_node* node);
    void refreshTimeOccurrences();
    // Forgets the deformation program so the next prepare() that needs it compiles afresh.
    void dropDeformation();

    Server* m_server = nullptr;
    wlr_renderer* m_renderer = nullptr;
    std::map<std::string, Entry, std::less<>> m_programs;
    std::shared_ptr<fx_effect_shader> m_builtinFade;
    std::shared_ptr<fx_effect_shader> m_deformation; // null once compilation failed
    bool m_deformationCompiled = false;
    bool m_persistentReferenced = false;
    bool m_inPlaceReferenced = false;
    bool m_cursorActive = false;
    unsigned m_retainedPersistent = 0;
    unsigned m_retainedInPlace = 0;
    unsigned m_retainedLight = 0;
    const wlr_output* m_pointerWlrOutput = nullptr; // under the pointer at the last forward
    bool m_pointerVisible = false;
    EffectLedger m_ledger;
    std::unique_ptr<audio::Bindings> m_audio;
    const void* m_audioOutput = nullptr;
    bool m_audioAdvance = false;
    std::map<const Output*, float> m_submittedEffectTimes;
    bool m_audioWasFrozen = false;
    struct TimeInstance {
      wlr_scene_node* node = nullptr;
      EffectInstanceState state;
    };
    std::map<const void*, TimeInstance> m_timeInstances;
    struct AudioInstance {
      wlr_scene_node* node = nullptr;
      std::string source;
    };
    std::map<const void*, AudioInstance> m_audioInstances;
    std::map<const void*, std::function<void()>> m_audioCaptures;
    wl_listener m_audioSessionActive{};
    struct AnimationAudio {
      EffectRegistry* registry = nullptr;
      wlr_scene_node* node = nullptr;
      unsigned slot = 0;
      std::string source;
      wl_listener destroy{};
    };
    std::map<std::pair<wlr_scene_node*, unsigned>, std::unique_ptr<AnimationAudio>> m_animationAudio;
    mutable uint64_t m_clockEpochMsec = 0;
    mutable bool m_clockEpochSet = false;
  };

  // The Server's registry. Set in Server's constructor before any view exists.
  [[nodiscard]] EffectRegistry& effectRegistry();

  // Binds `event`'s slot on `node` to its lifecycle effect while `value` animates, and clears it otherwise. For an
  // AnimatedValue, a zero `direction` follows the sign of the value's travel.
  void
  bindAnimationEffect(wlr_scene_node* node, AnimationEvent event, const AnimatedValue& value, float direction = 0.0F);
  void bindAnimationEffect(wlr_scene_node* node, AnimationEvent event, const AnimatedColor& value, float direction);

  // Seconds elapsed from `epochMsec` to `nowMsec`, computed in double precision so the result keeps
  // sub-millisecond resolution long after the raw millisecond count exceeds a float's exact range.
  [[nodiscard]] inline float effectClockSeconds(uint64_t nowMsec, uint64_t epochMsec) {
    return static_cast<float>(static_cast<double>(nowMsec - epochMsec) / 1000.0);
  }

} // namespace umbriel
