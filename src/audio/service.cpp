#include "audio/service.h"

namespace umbriel::audio {

  bool Service::define(std::string name, SourceDefinition definition) {
    if (name.empty() || definition.executable.empty() || definition.executable.front() != '/') {
      return false;
    }
    // Epoch belongs to demand acquisition, not user configuration.
    definition.configuration.epoch = 1;
    if (encode(definition.configuration).empty()) {
      return false;
    }
    auto found = m_sources.find(name);
    if (found != m_sources.end() && !found->second.consumers.empty()) {
      return false;
    }
    m_sources.insert_or_assign(
        std::move(name), Entry{.definition = std::move(definition), .consumers = {}, .supervisor = {}}
    );
    return true;
  }

  bool Service::acquire(std::string_view source, uint64_t consumer) {
    auto found = m_sources.find(source);
    if (found == m_sources.end() || consumer == 0) {
      return false;
    }
    auto& entry = found->second;
    if (entry.consumers.contains(consumer)) {
      return true;
    }
    if (entry.consumers.empty() && demandedSources() >= kMaxDemandedSources) {
      return false;
    }
    if (!entry.supervisor) {
      entry.supervisor = std::make_unique<Supervisor>(
          m_loop, entry.definition.configuration, entry.definition.executable, entry.definition.arguments
      );
      entry.supervisor->setSessionActive(m_active);
      entry.supervisor->changed = [this, name = found->first] {
        if (changed) {
          changed(name);
        }
      };
      entry.supervisor->stopped = [this] {
        if (quiescent() && stopped) {
          stopped();
        }
      };
    }
    entry.consumers.insert(consumer);
    entry.supervisor->setDemand(true);
    return true;
  }

  void Service::release(std::string_view source, uint64_t consumer) {
    const auto found = m_sources.find(source);
    if (found == m_sources.end()) {
      return;
    }
    auto& entry = found->second;
    entry.consumers.erase(consumer);
    if (entry.consumers.empty() && entry.supervisor) {
      entry.supervisor->setDemand(false);
    }
  }

  void Service::setSessionActive(bool active) {
    m_active = active;
    for (auto& [name, entry] : m_sources) {
      if (entry.supervisor) {
        entry.supervisor->setSessionActive(active);
      }
    }
  }

  void Service::retire() {
    changed = {};
    for (auto& [name, entry] : m_sources) {
      entry.consumers.clear();
      if (entry.supervisor) {
        entry.supervisor->setDemand(false);
      }
    }
  }

  bool Service::quiescent() const {
    for (const auto& [name, entry] : m_sources) {
      if (entry.supervisor && entry.supervisor->running()) {
        return false;
      }
    }
    return true;
  }

  void Service::reset(std::string_view source) {
    const auto found = m_sources.find(source);
    if (found != m_sources.end() && found->second.supervisor) {
      found->second.supervisor->reset();
    }
  }

  size_t Service::demandedSources() const {
    size_t count = 0;
    for (const auto& [name, entry] : m_sources) {
      count += !entry.consumers.empty();
    }
    return count;
  }

  size_t Service::consumers(std::string_view source) const {
    const auto found = m_sources.find(source);
    return found == m_sources.end() ? 0 : found->second.consumers.size();
  }

  std::string_view Service::state(std::string_view source) const {
    const auto found = m_sources.find(source);
    if (found == m_sources.end()) {
      return "unavailable";
    }
    return found->second.supervisor ? found->second.supervisor->state() : "unused";
  }

  const Receiver* Service::inspect(std::string_view source) const {
    const auto found = m_sources.find(source);
    return found == m_sources.end() || !found->second.supervisor ? nullptr : &found->second.supervisor->receiver();
  }

} // namespace umbriel::audio
