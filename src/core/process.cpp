#include "core/process.h"

#include "core/fdlimit.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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

  bool runShellCommand(const std::string& command) {
    int completion[2];
    if (pipe2(completion, O_CLOEXEC) != 0) {
      return false;
    }
    // Only the success marker reaches the pipe. A SIGCHLD handler may reap
    // the shell before we read it, so waitpid cannot reliably report success.
    const std::string script = "( " + command + " ) >&2 && printf x";
    const pid_t pid = fork();
    if (pid == 0) {
      resetChildSignalState();
      restoreFileDescriptorLimit();
      unsetenv("LIBEI_SOCKET");
      close(completion[0]);
      if (dup2(completion[1], STDOUT_FILENO) < 0 || !closeChildFileDescriptors()) {
        _exit(1);
      }
      execl("/bin/sh", "/bin/sh", "-c", script.c_str(), nullptr);
      _exit(1);
    }
    const int forkError = errno;
    close(completion[1]);
    if (pid < 0) {
      close(completion[0]);
      errno = forkError;
      return false;
    }
    char result = 0;
    ssize_t size;
    do {
      size = read(completion[0], &result, sizeof(result));
    } while (size < 0 && errno == EINTR);
    const int readError = errno;
    close(completion[0]);
    if (size != 1 || result != 'x') {
      errno = size < 0 ? readError : EIO;
      return false;
    }
    return true;
  }

  bool notifyServiceReady(std::string_view socketPath) {
    if (socketPath.empty()) {
      return true;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path) || (socketPath.front() != '/' && socketPath.front() != '@')) {
      errno = EINVAL;
      return false;
    }
    std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
    const bool abstract = socketPath.front() == '@';
    if (abstract) {
      address.sun_path[0] = '\0';
    }
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socketPath.size() + !abstract);
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return false;
    }
    constexpr std::string_view ready = "READY=1";
    ssize_t sent;
    do {
      sent = sendto(fd, ready.data(), ready.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&address), length);
    } while (sent < 0 && errno == EINTR);
    const int sendError = errno;
    close(fd);
    errno = sendError;
    return sent == static_cast<ssize_t>(ready.size());
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
