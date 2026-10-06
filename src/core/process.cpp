#include "core/process.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

extern "C" int umbrielCloseRange(unsigned int first, unsigned int last, int flags);

namespace umbriel {

  namespace {
    void reapChildren(int /*signal*/) {
      const int savedErrno = errno;
      while (waitpid(-1, nullptr, WNOHANG) > 0) {
      }
      errno = savedErrno;
    }
  } // namespace

  void installChildReaper() {
    struct sigaction action{};
    action.sa_handler = reapChildren;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &action, nullptr);
  }

  void resetChildSignalState() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, nullptr);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
  }

  bool closeChildFileDescriptors() {
    constexpr int kFirstDescriptor = STDERR_FILENO + 1;
    if (umbrielCloseRange(kFirstDescriptor, UINT_MAX, 0) == 0) {
      return true;
    }

    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      return false;
    }
    if (limit.rlim_cur == RLIM_INFINITY) {
      return false;
    }
    // The compositor raises its soft limit, then restores the inherited value
    // in children. Descriptors opened while the higher limit was active can
    // therefore sit above rlim_cur. The hard limit is the actual bound on all
    // descriptors that could have been inherited.
    if (limit.rlim_max == RLIM_INFINITY || limit.rlim_max > static_cast<rlim_t>(INT_MAX)) {
      return false;
    }
    const rlim_t upper = limit.rlim_max;
    for (rlim_t fd = kFirstDescriptor; fd < upper; ++fd) {
      close(static_cast<int>(fd));
    }
    return true;
  }

  bool watchChildExit(
      wl_event_loop* loop, pid_t pid, int (*callback)(int, uint32_t, void*), void* data, int& pidfd,
      wl_event_source*& source
  ) {
    pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (pidfd < 0) {
      return false;
    }
    source = wl_event_loop_add_fd(loop, pidfd, WL_EVENT_READABLE, callback, data);
    return source != nullptr;
  }

  void closeChildExitWatch(int& pidfd, wl_event_source*& source) {
    if (source != nullptr) {
      wl_event_source_remove(source);
      source = nullptr;
    }
    if (pidfd >= 0) {
      close(pidfd);
      pidfd = -1;
    }
  }

  void signalChild(int pidfd, int signal) {
    if (pidfd >= 0) {
      syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0);
    }
  }

  std::string resolveExecutable(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return {};
    }
    if (std::strchr(name, '/') != nullptr) {
      return access(name, X_OK) == 0 ? name : "";
    }
    const char* pathEnv = std::getenv("PATH");
    std::string path = pathEnv != nullptr ? pathEnv : "/bin:/usr/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::size_t count = (end == std::string::npos ? path.size() : end) - start;
      const std::string dir = count == 0 ? "." : path.substr(start, count);
      start = end == std::string::npos ? path.size() + 1 : end + 1;
      const std::string candidate = dir + "/" + name;
      if (access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    return {};
  }

} // namespace umbriel
