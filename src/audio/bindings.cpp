#include "audio/bindings.h"

#include "core/process.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <wayland-server-core.h>

namespace umbriel::audio {

  Bindings::~Bindings() {
    if (m_retirementIdle != nullptr) {
      wl_event_source_remove(m_retirementIdle);
    }
  }

  void Bindings::scheduleRetirement() {
    if (m_retirementIdle == nullptr) {
      m_retirementIdle = wl_event_loop_add_idle(m_loop, completeRetirement, this);
    }
  }

  void Bindings::completeRetirement(void* data) {
    auto& self = *static_cast<Bindings*>(data);
    self.m_retirementIdle = nullptr;
    if (!self.m_retiring || !self.m_retiring->quiescent()) {
      return;
    }
    self.m_retiring.reset();
    self.acquireConsumers();
  }

  void Bindings::endUpdate() {
    assert(m_updateDepth != 0);
    if (--m_updateDepth == 0) {
      acquireConsumers();
    }
  }

  void Bindings::acquireConsumers() {
    if (m_updateDepth != 0) {
      return;
    }
    for (auto& [owner, consumer] : m_consumers) {
      consumer.demanded = !m_retiring
          && m_service
          && consumer.eligible
          && m_service->acquire(consumer.source, reinterpret_cast<uintptr_t>(owner));
      if (consumer.demanded) {
        for (const void* output : consumer.outputs) {
          m_frames[output].inputs.try_emplace(consumer.source);
        }
      }
    }
  }

  void Bindings::configure(const std::vector<AudioSource>& definitions) {
    if (definitions == m_definitions) {
      return;
    }
    if (m_service) {
      m_service->retire();
      if (!m_service->quiescent()) {
        // New declarations stay dormant until the one previous service exits.
        // Repeated reloads therefore coalesce without accumulating children.
        m_retiring = std::move(m_service);
        m_retiring->stopped = [this] { scheduleRetirement(); };
      } else {
        m_service.reset();
      }
    }
    m_frames.clear();
    m_injections.clear();
    m_definitions = definitions;
    m_missingHelpers.clear();
    if (!definitions.empty()) {
      m_service = std::make_unique<Service>(m_loop);
      m_service->setSessionActive(m_active);
      m_service->changed = [this](std::string_view source) { sourceChanged(source); };
      for (const auto& definition : definitions) {
        SourceDefinition source;
        source.configuration.source =
            definition.mode == AudioMode::Playback ? SourceType::Playback : SourceType::Microphone;
        source.configuration.selector = definition.followDefault ? Selector::FollowDefault : Selector::Fixed;
        source.configuration.target = definition.target;
        source.executable =
            definition.provider == AudioProvider::External ? definition.executable : resolveExecutable("umbriel-audio");
        source.arguments = definition.args;
        // Missing optional helper is unavailable; no acquisition fallback.
        if (!source.executable.empty()) {
          (void)m_service->define(definition.name, std::move(source));
        } else {
          m_missingHelpers.insert(definition.name);
        }
      }
    }
    acquireConsumers();
  }

  void Bindings::update(const void* owner, const void* output, std::string_view source, bool eligible) {
    updateOccurrences(owner, std::span(&output, output != nullptr ? 1 : 0), source, eligible);
  }

  void Bindings::updateOccurrences(
      const void* owner, std::span<const void* const> outputs, std::string_view source, bool eligible
  ) {
    if (source.empty() || owner == nullptr || outputs.empty()) {
      remove(owner);
      return;
    }
    auto& consumer = m_consumers[owner];
    const auto previousOutputs = consumer.outputs;
    const auto previousSource = consumer.source;
    if (consumer.demanded && (consumer.source != source || !eligible)) {
      m_service->release(consumer.source, reinterpret_cast<uintptr_t>(owner));
      consumer.demanded = false;
    }
    consumer.outputs = std::set(outputs.begin(), outputs.end());
    consumer.source = source;
    consumer.eligible = eligible;
    if (m_updateDepth == 0 && !m_retiring && m_service && eligible && !consumer.demanded) {
      consumer.demanded = m_service->acquire(source, reinterpret_cast<uintptr_t>(owner));
    }
    if (consumer.demanded) {
      for (const void* output : consumer.outputs) {
        m_frames[output].inputs.try_emplace(consumer.source);
      }
    }
    for (const void* output : previousOutputs) {
      prune(output, previousSource);
    }
  }

  void Bindings::remove(const void* owner) {
    const auto found = m_consumers.find(owner);
    if (found == m_consumers.end()) {
      return;
    }
    auto consumer = std::move(found->second);
    m_consumers.erase(found);
    if (consumer.demanded && m_service) {
      m_service->release(consumer.source, reinterpret_cast<uintptr_t>(owner));
    }
    for (const void* output : consumer.outputs) {
      prune(output, consumer.source);
    }
  }

  void Bindings::prune(const void* output, std::string_view source) {
    const bool retained = std::ranges::any_of(m_consumers, [&](const auto& entry) {
      return entry.second.outputs.contains(output) && entry.second.source == source && entry.second.demanded;
    });
    if (retained) {
      return;
    }
    const auto frame = m_frames.find(output);
    if (frame != m_frames.end()) {
      frame->second.inputs.erase(std::string(source));
      if (frame->second.inputs.empty()) {
        m_frames.erase(frame);
      }
    }
  }

  void Bindings::removeOutput(const void* output) {
    for (auto it = m_consumers.begin(); it != m_consumers.end();) {
      const auto current = it++;
      current->second.outputs.erase(output);
      if (current->second.outputs.empty()) {
        remove(current->first);
      }
    }
    m_frames.erase(output);
  }

  void Bindings::setSessionActive(bool active) {
    m_active = active;
    if (m_service) {
      m_service->setSessionActive(active);
    }
    if (!active) {
      m_injections.clear();
      for (auto& [output, frame] : m_frames) {
        for (auto& [source, latch] : frame.inputs) {
          latch.cancel();
        }
      }
    }
  }

  bool Bindings::active(const void* output) const {
    return m_active && std::ranges::any_of(m_consumers, [output](const auto& entry) {
             return entry.second.outputs.contains(output) && entry.second.demanded && entry.second.eligible;
           });
  }

  const Receiver* Bindings::receiver(std::string_view source) const {
    const auto injected = m_injections.find(source);
    return injected != m_injections.end() ? &injected->second : inspect(source);
  }

  bool Bindings::dirty(const void* output) const {
    const auto frame = m_frames.find(output);
    if (!m_active || frame == m_frames.end()) {
      return false;
    }
    if (frame->second.injected) {
      return true;
    }
    for (const auto& [source, latch] : frame->second.inputs) {
      const auto* value = receiver(source);
      if (value != nullptr && latch.dirty(*value)) {
        return true;
      }
    }
    return false;
  }

  bool Bindings::injected(const void* output) const {
    const auto frame = m_frames.find(output);
    if (frame == m_frames.end()) {
      return false;
    }
    if (frame->second.injected) {
      return true;
    }
    for (const auto& [source, latch] : frame->second.inputs) {
      const auto value = m_injections.find(source);
      if (value != m_injections.end() && latch.dirty(value->second)) {
        return true;
      }
    }
    return false;
  }

  void Bindings::begin(const void* output, bool advance, bool frozen) {
    const auto frame = m_frames.find(output);
    if (frame == m_frames.end()) {
      return;
    }
    for (auto& [source, latch] : frame->second.inputs) {
      if (const auto* value = receiver(source)) {
        const bool explicitInjection = frame->second.injected || (m_injections.contains(source) && latch.dirty(*value));
        (void)latch.latch(*value, frozen || !advance, explicitInjection);
      }
    }
    frame->second.injected = false;
    std::map<std::string, Input, std::less<>> inputs;
    for (const auto& [source, latch] : frame->second.inputs) {
      inputs.emplace(source, latch.input());
    }
    if (inputs != frame->second.observedInputs) {
      frame->second.observedInputs = std::move(inputs);
      frame->second.revision = ++m_nextRevision;
    }
  }

  uint64_t Bindings::inputRevision(const void* output) const {
    const auto frame = m_frames.find(output);
    return frame == m_frames.end() ? 0 : frame->second.revision;
  }

  void Bindings::finish(const void* output, bool success) {
    const auto frame = m_frames.find(output);
    if (frame != m_frames.end()) {
      for (auto& [source, latch] : frame->second.inputs) {
        latch.submitted(success);
      }
    }
  }

  Input Bindings::input(const void* output, std::string_view source) const {
    if (!m_active) {
      return {};
    }
    const auto frame = m_frames.find(output);
    if (frame == m_frames.end()) {
      return {};
    }
    const auto latch = frame->second.inputs.find(source);
    return latch == frame->second.inputs.end() ? Input{} : latch->second.input();
  }

  const InputLatch* Bindings::inspectLatch(const void* output, std::string_view source) const {
    const auto frame = m_frames.find(output);
    if (frame == m_frames.end()) {
      return nullptr;
    }
    const auto latch = frame->second.inputs.find(source);
    return latch == frame->second.inputs.end() ? nullptr : &latch->second;
  }

  std::string_view Bindings::state(std::string_view source) const {
    if (m_retiring) {
      return "retiring";
    }
    if (m_missingHelpers.contains(source)) {
      return "helper_missing";
    }
    const bool refused = std::ranges::any_of(m_consumers, [source](const auto& entry) {
      return entry.second.source == source && entry.second.eligible && !entry.second.demanded;
    });
    if (refused && m_service && m_service->demandedSources() >= kMaxDemandedSources) {
      return "source_limit";
    }
    return m_service ? m_service->state(source) : "unused";
  }

  const Receiver* Bindings::inspect(std::string_view source) const {
    return m_service ? m_service->inspect(source) : nullptr;
  }

  void Bindings::sourceChanged(std::string_view source) {
    if (!changed) {
      return;
    }
    for (const auto& [output, frame] : m_frames) {
      if (frame.inputs.contains(source)) {
        changed(output);
      }
    }
  }

  bool Bindings::inject(std::string_view source, const Features& features) {
    if (!validFeatures(features)
        || !std::ranges::any_of(m_definitions, [&](const auto& definition) { return definition.name == source; })) {
      return false;
    }
    auto [entry, inserted] = m_injections.try_emplace(std::string(source));
    auto& value = entry->second;
    const uint64_t now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (inserted) {
      value.begin(1, SourceType::Synthetic, now);
      (void)value.receive(encode(Ready{.epoch = 1}), now);
    }
    if (value.receive(
            encode(
                Snapshot{
                    .epoch = 1,
                    .generation = 1,
                    .sequence = value.sequence() + 1,
                    .observationNs = std::max(now, value.observationNs() + 1),
                    .features = features
                }
            ),
            now
        )
        != ReceiveResult::Accepted) {
      return false;
    }
    for (auto& [output, frame] : m_frames) {
      if (frame.inputs.contains(source)) {
        frame.injected = true;
      }
    }
    sourceChanged(source);
    return true;
  }

  void Bindings::clearInjections() {
    if (m_injections.empty()) {
      return;
    }
    m_injections.clear();
    for (auto& [output, frame] : m_frames) {
      for (auto& [source, latch] : frame.inputs) {
        latch.discard();
      }
      if (changed) {
        changed(output);
      }
    }
  }

} // namespace umbriel::audio
