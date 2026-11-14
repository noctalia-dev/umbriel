#pragma once

#include "config/change.h"
#include "config/config.h"
#include "core/toml.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace umbriel {

  // One loaded configuration and everything learned while loading it: `Config`, diagnostics, the set of files to watch
  // (a config that stops `include`-ing a file must stop watching it), and whether a config file exists. A reload
  // replaces all of them at once. `generation()` counts successful reloads; consumers that cache derived state compare
  // it against the generation they cached at.
  class ConfigStore {
  public:
    // Resolve the user, system, or packaged config and load it. Any error,
    // including a missing explicit path, starts with defaults and leaves the
    // diagnostics for the error banner.
    void load(const char* explicitPath);
    // Replace the root config file. On failure the previous configuration is kept
    // and the generation does not move: a config with a syntax error must not take
    // the session down.
    ConfigReloadResult replace(const std::filesystem::path& explicitPath, std::string& errors);
    // Re-parse. On failure the previous configuration is kept and the generation
    // does not move: a config with a syntax error must not take the session down.
    ConfigReloadResult reload();
    // Re-reads the current config file with `overrides` in place of the active runtime overrides and commits the
    // result. A load that fails, or, when `rejectNewDiagnostics`, reports a diagnostic the active configuration did
    // not, changes nothing and lists why in `errors`, one diagnostic per line.
    ConfigReloadResult applyOverrides(toml::table overrides, bool rejectNewDiagnostics, std::string& errors);

    const Config& config() const { return m_config; }
    // Values set by `umbriel config set`. Every load lays them over the file and its includes; `load` starts without.
    const toml::table& overrides() const { return m_overrides; }
    // The document the active configuration was read from: file, includes, and overrides merged.
    const toml::table& effective() const { return m_effective; }
    const std::vector<ConfigDiagnostic>& diagnostics() const { return m_diagnostics; }
    // Every path whose change can affect the next reload: implicit lookup
    // candidates, the attempted root, and its includes.
    const std::vector<std::filesystem::path>& watchPaths() const { return m_watchPaths; }
    const std::filesystem::path& rootPath() const { return m_rootPath; }
    // True when the session is running on built-in defaults because no file was
    // found. Drives the "no config" notice on the cheatsheet.
    bool fileMissing() const { return m_fileMissing; }
    uint64_t generation() const { return m_generation; }
    // Generation of the last reload that reapplied workspace layouts.
    uint64_t workspaceLayoutGeneration() const { return m_workspaceLayoutGeneration; }
    bool missingIncludes() const { return m_missingIncludes; }

    // Loader-only writers follow. The read-only API remains above.
    // Keep the active config when a reload fails.
    void beginLoad(const std::vector<std::filesystem::path>& watchPaths);
    void addDiagnostic(ConfigDiagnostic diagnostic);
    // The sink the section readers append to directly.
    std::vector<ConfigDiagnostic>& mutableDiagnostics() { return m_diagnostics; }
    // Ignores duplicates: the same file can be included from several places.
    void addWatchPath(std::filesystem::path path);
    // Put diagnostics back into source order. Readers emit in whatever order they run, and unknown-key reports come
    // last (after a section has finished), so without this the list jumps around the file.
    void sortDiagnostics();
    // Adopt a successfully parsed config and the document it was read from, and bump the generation.
    ConfigReloadResult
    commit(Config&& config, toml::table&& effective, std::filesystem::path rootPath, bool fileMissing);
    void setMissingIncludes(bool missing) { m_missingIncludes = missing; }
    void setRootPath(std::filesystem::path path, bool explicitPath);

  private:
    Config m_config;
    toml::table m_overrides;
    toml::table m_effective;
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
    uint64_t m_workspaceLayoutGeneration = 0;
  };

  // The process-wide store, read by consumers below Server on render paths. The layout and parsing layers take their
  // settings as arguments and do not reach for this.
  ConfigStore& configStore();

} // namespace umbriel
