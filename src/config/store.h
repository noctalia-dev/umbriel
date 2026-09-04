#pragma once

#include "config/change.h"
#include "config/config.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace umbriel {

  // One loaded configuration, with everything that was learned while loading it. The pieces belong together because a
  // reload replaces all of them at once: a new `Config`, a new diagnostic list, a new set of files to watch (a config
  // that stops `include`-ing a file must stop watching it), and a new answer to "is there even a config file". Keeping
  // them as separate globals made it possible to update one and forget another. `generation()` counts successful
  // reloads. Consumers that cache anything derived from the config compare it against the generation they cached at,
  // which is cheaper and less error-prone than every consumer being individually re-notified from Server::applyConfig.
  class ConfigStore {
  public:
    // Resolve the user, system, or packaged config and load it. Falls back to
    // built-in defaults when no file exists.
    void load(const char* explicitPath);
    // Re-parse. On failure the previous configuration is kept and the generation
    // does not move: a config with a syntax error must not take the session down.
    [[nodiscard]] ConfigReloadResult reload();

    [[nodiscard]] const Config& config() const { return m_config; }
    [[nodiscard]] const std::vector<ConfigDiagnostic>& diagnostics() const { return m_diagnostics; }
    // Every path whose change can affect the next reload: implicit lookup
    // candidates, the attempted root, and its includes.
    [[nodiscard]] const std::vector<std::filesystem::path>& watchPaths() const { return m_watchPaths; }
    [[nodiscard]] const std::filesystem::path& rootPath() const { return m_rootPath; }
    // True when the session is running on built-in defaults because no file was
    // found. Drives the "no config" notice on the cheatsheet.
    [[nodiscard]] bool fileMissing() const { return m_fileMissing; }
    [[nodiscard]] uint64_t generation() const { return m_generation; }
    [[nodiscard]] bool missingIncludes() const { return m_missingIncludes; }

    // Loader-only writers follow. The read-only API remains above.
    // Keep the active config when a reload fails.
    void beginLoad(const std::vector<std::filesystem::path>& watchPaths);
    void addDiagnostic(ConfigDiagnostic diagnostic);
    // The sink the section readers append to directly.
    [[nodiscard]] std::vector<ConfigDiagnostic>& mutableDiagnostics() { return m_diagnostics; }
    // Ignores duplicates: the same file can be included from several places.
    void addWatchPath(std::filesystem::path path);
    // Put diagnostics back into source order. Readers emit in whatever order they run, and unknown-key reports come
    // last (after a section has finished), so without this the list jumps around the file.
    void sortDiagnostics();
    // Adopt a successfully parsed config and bump the generation.
    [[nodiscard]] ConfigReloadResult commit(Config&& config, std::filesystem::path rootPath, bool fileMissing);
    void setMissingIncludes(bool missing) { m_missingIncludes = missing; }
    void setRootPath(std::filesystem::path path, bool explicitPath);

  private:
    Config m_config;
    std::vector<ConfigDiagnostic> m_diagnostics;
    std::vector<std::filesystem::path> m_watchPaths;
    // Captured before configured environment variables are applied. Reloads
    // recheck this fixed search order instead of letting the config redirect
    // its own source through HOME or the XDG variables.
    std::vector<std::filesystem::path> m_implicitCandidates;
    std::filesystem::path m_rootPath;
    bool m_explicitPath = false;
    bool m_fileMissing = false;
    bool m_missingIncludes = false;
    uint64_t m_generation = 0;
  };

  // The process-wide store. Still a global: every consumer below Server reads appearance settings on render paths, and
  // threading a reference through all of them buys nothing while there is exactly one configuration per process. The
  // layout and parsing layers, which are the ones worth unit-testing, already take their settings as arguments and do
  // not reach for this.
  [[nodiscard]] ConfigStore& configStore();

} // namespace umbriel
