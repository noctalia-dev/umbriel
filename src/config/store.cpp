#include "config/store.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace umbriel {

  void ConfigStore::beginLoad(const std::vector<std::filesystem::path>& watchPaths) {
    m_diagnostics.clear();
    m_missingIncludes = false;
    m_watchPaths.clear();
    for (const std::filesystem::path& path : watchPaths) {
      addWatchPath(path);
    }
  }

  void ConfigStore::addDiagnostic(ConfigDiagnostic diagnostic) { m_diagnostics.push_back(std::move(diagnostic)); }

  void ConfigStore::addWatchPath(std::filesystem::path path) {
    if (std::ranges::find(m_watchPaths, path) == m_watchPaths.end()) {
      m_watchPaths.push_back(std::move(path));
    }
  }

  void ConfigStore::sortDiagnostics() {
    // Stable so two diagnostics on the same key keep the order they were found
    // in. File-less entries (whole-config errors) sort first.
    std::ranges::stable_sort(m_diagnostics, [](const ConfigDiagnostic& a, const ConfigDiagnostic& b) {
      return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
  }

  ConfigReloadResult ConfigStore::commit(Config&& config, std::filesystem::path rootPath, bool fileMissing) {
    // Computed before the move, and only after the first load: everything is new
    // the first time through.
    ConfigReloadResult result{
        .success = true,
        .change = m_generation == 0 ? ConfigChange::everything() : ConfigChange::between(m_config, config),
        .effects = m_generation == 0 ? ConfigEffects::everything() : ConfigEffects::between(m_config, config),
    };
    m_config = std::move(config);
    m_rootPath = std::move(rootPath);
    m_fileMissing = fileMissing;
    ++m_generation;
    return result;
  }

  void ConfigStore::setRootPath(std::filesystem::path path, bool explicitPath) {
    m_rootPath = std::move(path);
    m_explicitPath = explicitPath;
  }

} // namespace umbriel
