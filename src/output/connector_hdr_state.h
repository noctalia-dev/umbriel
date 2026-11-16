#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

struct wlr_backend;

namespace umbriel {

  class ConnectorHdrState {
  public:
    void recordCommit(
        const wlr_backend* backend, std::string_view connector, bool imageDescriptionCommitted, bool hdrActive
    ) {
      if (!imageDescriptionCommitted) {
        return;
      }
      const auto entry = find(backend, connector);
      if (hdrActive && entry == m_entries.end()) {
        m_entries.push_back({.backend = backend, .connector = std::string(connector)});
      } else if (!hdrActive && entry != m_entries.end()) {
        m_entries.erase(entry);
      }
    }

    void forgetBackend(const wlr_backend* backend) {
      std::erase_if(m_entries, [&](const Entry& entry) { return entry.backend == backend; });
    }

    bool mayHoldHdr(const wlr_backend* backend, std::string_view connector) const {
      return find(backend, connector) != m_entries.end();
    }

  private:
    struct Entry {
      const wlr_backend* backend = nullptr;
      std::string connector;
    };

    std::vector<Entry>::const_iterator find(const wlr_backend* backend, std::string_view connector) const {
      return std::ranges::find_if(m_entries, [&](const Entry& entry) {
        return entry.backend == backend && entry.connector == connector;
      });
    }

    std::vector<Entry> m_entries;
  };

} // namespace umbriel
