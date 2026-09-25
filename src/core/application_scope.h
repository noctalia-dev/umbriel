#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace umbriel {

  // True only for the process systemd started directly. Descendants inherit
  // SYSTEMD_EXEC_PID, so comparing it with the current pid prevents nested or
  // manually launched compositors from attaching applications to a host
  // Umbriel session.
  [[nodiscard]] bool isDirectSystemdService(pid_t currentPid, const char* systemdExecPid);

  // Return the deepest service or scope unit from cgroup file contents.
  // Empty means the process is not owned by a recognizable systemd unit.
  [[nodiscard]] std::string systemdUnitFromCgroup(std::string_view cgroup);
  [[nodiscard]] std::string currentSystemdUnit();

  inline constexpr std::array<const char*, 3> kSystemdControlEnvironmentVariables{
      "XDG_RUNTIME_DIR",
      "DBUS_SESSION_BUS_ADDRESS",
      "SYSTEMD_BUS_ADDRESS",
  };
  using SystemdControlEnvironment = std::array<std::optional<std::string>, kSystemdControlEnvironmentVariables.size()>;
  using ApplicationScopeEnvironmentArguments = std::array<std::string, kSystemdControlEnvironmentVariables.size()>;

  // Keep the user-manager connection independent from configured application
  // values. systemd-run connects with the captured values, then applies the
  // explicit arguments to the command after its scope has started.
  [[nodiscard]] SystemdControlEnvironment captureSystemdControlEnvironment();
  [[nodiscard]] bool restoreSystemdControlEnvironment(const SystemdControlEnvironment& environment);
  [[nodiscard]] ApplicationScopeEnvironmentArguments
  applicationScopeEnvironmentArguments(std::span<const std::pair<std::string, std::string>> configuredEnvironment);

  // Replace the current child with systemd-run. The caller must pass an
  // absolute executable, preformatted lifecycle properties, and any explicit
  // command environment arguments. Environment expansion is disabled so the
  // shell receives the configured command byte-for-byte.
  // Failure is closed: this function exits without executing the application.
  [[noreturn]] void execApplicationInScope(
      const char* systemdRun, const char* unitArgument, const char* partOfProperty, const char* bindsToProperty,
      const ApplicationScopeEnvironmentArguments& environmentArguments, const char* command
  );

} // namespace umbriel
