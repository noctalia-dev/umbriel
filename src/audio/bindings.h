#pragma once

#include "audio/service.h"
#include "config/audio.h"

namespace umbriel::audio {

  // Maps renderer occurrences to source demand without touching selection/RNG.
  // Output keys are also used for independent isolated-capture compositions.
  class Bindings {
  public:
    explicit Bindings(wl_event_loop* loop) : m_loop(loop) {}
    ~Bindings();
    void configure(const std::vector<AudioSource>& definitions);
    // Defer new acquisitions until all obsolete occurrence leases are released.
    void beginUpdate() { ++m_updateDepth; }
    void endUpdate();
    void update(const void* owner, const void* output, std::string_view source, bool eligible);
    void
    updateOccurrences(const void* owner, std::span<const void* const> outputs, std::string_view source, bool eligible);
    void remove(const void* owner);
    void removeOutput(const void* output);
    void setSessionActive(bool active);
    [[nodiscard]] bool active(const void* output) const;
    [[nodiscard]] bool dirty(const void* output) const;
    [[nodiscard]] bool injected(const void* output) const;
    void begin(const void* output, bool advance, bool frozen);
    void finish(const void* output, bool success);
    [[nodiscard]] uint64_t inputRevision(const void* output) const;
    [[nodiscard]] Input input(const void* output, std::string_view source) const;
    [[nodiscard]] const InputLatch* inspectLatch(const void* output, std::string_view source) const;
    [[nodiscard]] std::string_view state(std::string_view source) const;
    [[nodiscard]] const Receiver* inspect(std::string_view source) const;
    [[nodiscard]] size_t demandedSources() const { return m_service ? m_service->demandedSources() : 0; }
    // Fixture injection is explicitly latched while frozen. Real snapshots
    // continue to arrive but cannot replace the frozen fixture input.
    [[nodiscard]] bool inject(std::string_view source, const Features& features);
    void clearInjections();
    std::function<void(const void*)> changed;

  private:
    struct Consumer {
      std::set<const void*> outputs;
      std::string source;
      bool eligible = false;
      bool demanded = false;
    };
    struct Frame {
      std::map<std::string, InputLatch, std::less<>> inputs;
      bool injected = false;
      std::map<std::string, Input, std::less<>> observedInputs;
      uint64_t revision = 0;
    };
    [[nodiscard]] const Receiver* receiver(std::string_view source) const;
    void sourceChanged(std::string_view source);
    void prune(const void* output, std::string_view source);
    void acquireConsumers();
    void scheduleRetirement();
    static void completeRetirement(void* data);
    wl_event_loop* m_loop;
    bool m_active = true;
    unsigned m_updateDepth = 0;
    uint64_t m_nextRevision = 0;
    std::vector<AudioSource> m_definitions;
    std::set<std::string, std::less<>> m_missingHelpers;
    std::unique_ptr<Service> m_service;
    std::unique_ptr<Service> m_retiring;
    wl_event_source* m_retirementIdle = nullptr;
    std::map<const void*, Consumer> m_consumers;
    std::map<const void*, Frame> m_frames;
    std::map<std::string, Receiver, std::less<>> m_injections;
  };

} // namespace umbriel::audio
