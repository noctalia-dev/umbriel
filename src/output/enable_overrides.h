#pragma once

#include "output/identity.h"

#include <optional>
#include <string>
#include <vector>

namespace umbriel {

  // Session-scoped logical enablement selected through output management.
  // Identified monitors can carry their choice to another connector. Outputs
  // without display identity remain bound to their connector name.
  class OutputEnableOverrides {
  public:
    void remember(const OutputIdentity& identity, bool enabled);
    std::optional<bool> resolve(const OutputIdentity& identity);
    void clear() { m_entries.clear(); }

  private:
    struct Entry {
      std::string connector;
      std::string descriptor;
      bool enabled = true;
      bool descriptorAmbiguous = false;
    };

    std::vector<Entry> m_entries;
  };

} // namespace umbriel
