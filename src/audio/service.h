#pragma once

#include "audio/supervisor.h"

#include <map>
#include <memory>
#include <set>
#include <string_view>

namespace umbriel::audio {

  struct SourceDefinition {
    Configuration configuration;
    std::string executable;
    std::vector<std::string> arguments;
  };

  // Internal demand boundary. Callers must only acquire for usable visible or
  // actively captured occurrences, never for config parsing/compilation. Owner
  // IDs deduplicate mirrors; separate display/capture leases use distinct IDs.
  class Service {
  public:
    explicit Service(wl_event_loop* loop) : m_loop(loop) {}
    [[nodiscard]] bool define(std::string name, SourceDefinition definition);
    [[nodiscard]] bool acquire(std::string_view source, uint64_t consumer);
    void release(std::string_view source, uint64_t consumer);
    void setSessionActive(bool active);
    void reset(std::string_view source);
    void retire();
    [[nodiscard]] bool quiescent() const;
    [[nodiscard]] size_t demandedSources() const;
    [[nodiscard]] size_t consumers(std::string_view source) const;
    [[nodiscard]] std::string_view state(std::string_view source) const;
    [[nodiscard]] const Receiver* inspect(std::string_view source) const;
    std::function<void(std::string_view)> changed;
    std::function<void()> stopped;

  private:
    struct Entry {
      SourceDefinition definition;
      std::set<uint64_t> consumers;
      std::unique_ptr<Supervisor> supervisor;
    };
    wl_event_loop* m_loop;
    bool m_active = true;
    std::map<std::string, Entry, std::less<>> m_sources;
  };

} // namespace umbriel::audio
