#pragma once

#include <string>
#include <string_view>

namespace umbriel {

  // Undo everything the compositor did to the process that a child must not inherit. `wl_event_loop_add_signal` blocks
  // SIGINT/SIGTERM process-wide via sigprocmask, and a blocked mask survives fork and exec. Restore the child defaults
  // before exec, alongside `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Reap exited children from a SIGCHLD handler. Unlike SIG_IGN, a handler is not inherited across exec, so programs
  // the compositor's libraries launch start with the default disposition: Xwayland waits for its own xkbcomp child and
  // cannot start when SIGCHLD is ignored. wlroots tolerates its Xwayland launcher being reaped here.
  void installChildReaper();

  // Run a shell command to completion without racing the global child reaper.
  // The command's output goes to stderr; false means it did not complete successfully.
  [[nodiscard]] bool runShellCommand(const std::string& command);

  // Send READY=1 from the compositor process. An empty socket path is a no-op.
  [[nodiscard]] bool notifyServiceReady(std::string_view socketPath);

  // Close every non-standard descriptor before a managed application child
  // hands control to systemd-run. Returns false when a complete close cannot
  // be guaranteed.
  [[nodiscard]] bool closeChildFileDescriptors();

  // Resolve a bare executable against PATH, or validate an explicit path. An
  // empty result means no executable was found.
  std::string resolveExecutable(const char* name);

} // namespace umbriel
