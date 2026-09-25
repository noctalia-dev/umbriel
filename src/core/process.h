#pragma once

#include <string>

namespace umbriel {

  // Undo everything the compositor did to the process that a child must not inherit. `wl_event_loop_add_signal` blocks
  // SIGINT/SIGTERM process-wide via sigprocmask, and a blocked mask survives
  // fork and exec. Restore the child defaults before exec, alongside
  // `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Close every non-standard descriptor before a managed application child
  // hands control to systemd-run. Returns false when a complete close cannot
  // be guaranteed.
  [[nodiscard]] bool closeChildFileDescriptors();

  // Resolve a bare executable against PATH, or validate an explicit path. An
  // empty result means no executable was found.
  [[nodiscard]] std::string resolveExecutable(const char* name);

} // namespace umbriel
