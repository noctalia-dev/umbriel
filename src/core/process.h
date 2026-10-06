#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

struct wl_event_loop;
struct wl_event_source;

namespace umbriel {

  // Undo everything the compositor did to the process that a child must not inherit. `wl_event_loop_add_signal` blocks
  // SIGINT/SIGTERM process-wide via sigprocmask, and a blocked mask survives fork and exec. Restore the child defaults
  // before exec, alongside `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Reap exited children from a SIGCHLD handler. Unlike SIG_IGN, a handler is not inherited across exec, so programs
  // the compositor's libraries launch start with the default disposition: Xwayland waits for its own xkbcomp child and
  // cannot start when SIGCHLD is ignored. wlroots tolerates its Xwayland launcher being reaped here.
  void installChildReaper();

  // Close every non-standard descriptor before a managed application child
  // hands control to systemd-run. Returns false when a complete close cannot
  // be guaranteed.
  [[nodiscard]] bool closeChildFileDescriptors();

  // Shared Linux exit observation. SIGCHLD may be SIG_IGN: no waitable exit
  // status is required. On watch allocation failure, pidfd remains owned by the
  // caller so it can signal safely before closeChildExitWatch().
  [[nodiscard]] bool watchChildExit(
      wl_event_loop* loop, pid_t pid, int (*callback)(int, uint32_t, void*), void* data, int& pidfd,
      wl_event_source*& source
  );
  void closeChildExitWatch(int& pidfd, wl_event_source*& source);
  void signalChild(int pidfd, int signal);

  // Resolve a bare executable against PATH, or validate an explicit path. An
  // empty result means no executable was found.
  [[nodiscard]] std::string resolveExecutable(const char* name);

} // namespace umbriel
