#include "core/application_scope.h"

#include "check.h"
#include "core/process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using umbriel::ApplicationScopeEnvironmentArguments;
using umbriel::applicationScopeEnvironmentArguments;
using umbriel::captureSystemdControlEnvironment;
using umbriel::closeChildFileDescriptors;
using umbriel::currentSystemdUnit;
using umbriel::execApplicationInScope;
using umbriel::isDirectSystemdService;
using umbriel::resolveExecutable;
using umbriel::restoreSystemdControlEnvironment;
using umbriel::systemdUnitFromCgroup;

namespace {
  bool gForceCloseRangeFailure = false;
}

extern "C" int __real_close_range(unsigned int first, unsigned int last, int flags);

extern "C" int __wrap_close_range(unsigned int first, unsigned int last, int flags) {
  if (gForceCloseRangeFailure) {
    errno = ENOSYS;
    return -1;
  }
  return __real_close_range(first, last, flags);
}

namespace {
  void closePair(int (&descriptors)[2]) {
    for (int& descriptor : descriptors) {
      if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
      }
    }
  }

  bool waitForExit(pid_t pid, int* status, int timeoutMs = 5000) {
    for (int elapsed = 0; elapsed <= timeoutMs; elapsed += 10) {
      const pid_t result = waitpid(pid, status, WNOHANG);
      if (result == pid) {
        return true;
      }
      if (result < 0 && errno != EINTR) {
        return false;
      }
      poll(nullptr, 0, 10);
    }
    return false;
  }

  std::string readLinesWithTimeout(int descriptor, size_t minimumLines, int timeoutMs = 8000) {
    std::string output;
    for (int elapsed = 0; elapsed <= timeoutMs; elapsed += 20) {
      pollfd pollDescriptor{.fd = descriptor, .events = POLLIN, .revents = 0};
      int result = -1;
      do {
        result = poll(&pollDescriptor, 1, 20);
      } while (result < 0 && errno == EINTR);
      if (result < 0) {
        break;
      }
      if (result > 0 && (pollDescriptor.revents & (POLLIN | POLLHUP)) != 0) {
        char buffer[4096];
        ssize_t size = -1;
        do {
          size = read(descriptor, buffer, sizeof(buffer));
        } while (size < 0 && errno == EINTR);
        if (size > 0) {
          output.append(buffer, static_cast<size_t>(size));
        } else if (size == 0) {
          break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          break;
        }
      }
      if (static_cast<size_t>(std::count(output.begin(), output.end(), '\n')) >= minimumLines) {
        break;
      }
    }
    return output;
  }

  std::string systemctlProperty(const std::string& systemctl, const std::string& unit, const std::string& property) {
    int outputPipe[2]{-1, -1};
    if (pipe2(outputPipe, O_CLOEXEC | O_NONBLOCK) < 0) {
      return {};
    }
    const std::string propertyArgument = "--property=" + property;
    const pid_t child = fork();
    if (child == 0) {
      close(outputPipe[0]);
      if (dup2(outputPipe[1], STDOUT_FILENO) < 0) {
        _exit(1);
      }
      close(outputPipe[1]);
      execl(
          systemctl.c_str(), "systemctl", "--user", "show", unit.c_str(), propertyArgument.c_str(), "--value", nullptr
      );
      _exit(1);
    }
    close(outputPipe[1]);
    outputPipe[1] = -1;
    if (child < 0) {
      closePair(outputPipe);
      return {};
    }

    std::string output;
    int status = 0;
    bool exited = false;
    for (int elapsed = 0; elapsed <= 3000; elapsed += 10) {
      char buffer[1024];
      while (true) {
        const ssize_t size = read(outputPipe[0], buffer, sizeof(buffer));
        if (size > 0) {
          output.append(buffer, static_cast<size_t>(size));
          continue;
        }
        if (size < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      const pid_t result = waitpid(child, &status, WNOHANG);
      if (result == child) {
        exited = true;
        break;
      }
      if (result < 0 && errno != EINTR) {
        break;
      }
      poll(nullptr, 0, 10);
    }
    if (!exited) {
      kill(child, SIGKILL);
      exited = waitForExit(child, &status);
    }
    char buffer[1024];
    ssize_t size = -1;
    do {
      size = read(outputPipe[0], buffer, sizeof(buffer));
      if (size > 0) {
        output.append(buffer, static_cast<size_t>(size));
      }
    } while (size > 0 || (size < 0 && errno == EINTR));
    close(outputPipe[0]);

    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return {};
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
      output.pop_back();
    }
    return output;
  }

  bool containsWord(std::string_view words, std::string_view expected) {
    std::istringstream stream{std::string(words)};
    std::string word;
    while (stream >> word) {
      if (word == expected) {
        return true;
      }
    }
    return false;
  }

  int captureExecArguments(int argc, char** argv) {
    for (int index = 0; index < argc; ++index) {
      const char* cursor = argv[index];
      size_t remaining = std::strlen(cursor);
      while (remaining > 0) {
        const ssize_t size = write(STDOUT_FILENO, cursor, remaining);
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size <= 0) {
          return 2;
        }
        cursor += size;
        remaining -= static_cast<size_t>(size);
      }
      if (write(STDOUT_FILENO, "\n", 1) != 1) {
        return 2;
      }
    }
    return 0;
  }
} // namespace

UMBRIEL_TEST(managedSessionDetectionRequiresTheDirectSystemdProcess) {
  CHECK(!isDirectSystemdService(42, nullptr));
  CHECK(!isDirectSystemdService(42, ""));
  CHECK(!isDirectSystemdService(42, "41"));
  CHECK(!isDirectSystemdService(42, "42suffix"));
  CHECK(!isDirectSystemdService(42, "-42"));
  CHECK(isDirectSystemdService(42, "42"));
}

UMBRIEL_TEST(cgroupParsingFindsTheInnermostOwningUnit) {
  CHECK_EQ(
      systemdUnitFromCgroup("0::/user.slice/user-1000.slice/user@1000.service/session.slice/umbriel.service\n"),
      "umbriel.service"
  );
  CHECK_EQ(
      systemdUnitFromCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-umbriel-test.scope\n"),
      "app-umbriel-test.scope"
  );
  CHECK_EQ(
      systemdUnitFromCgroup(
          "2:name=systemd:/user.slice/user-1000.slice/user@1000.service/session.slice/umbriel.service\n"
          "1:memory:/user.slice/user-1000.slice/user@1000.service\n"
      ),
      "umbriel.service"
  );
  CHECK_EQ(systemdUnitFromCgroup("12:devices:/user.slice\n11:memory:/\n"), "");
}

UMBRIEL_TEST(applicationScopeEnvironmentKeepsTheControlConnectionSeparate) {
  const std::vector<std::pair<std::string, std::string>> configured{
      {"UNRELATED", "ignored"},
      {"XDG_RUNTIME_DIR", "/tmp/application runtime"},
      {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/application-$bus"},
      {"SYSTEMD_BUS_ADDRESS", "unix:path=/tmp/application=systemd"},
  };
  const ApplicationScopeEnvironmentArguments arguments = applicationScopeEnvironmentArguments(configured);
  CHECK_EQ(arguments[0], "--setenv=XDG_RUNTIME_DIR=/tmp/application runtime");
  CHECK_EQ(arguments[1], "--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/application-$bus");
  CHECK_EQ(arguments[2], "--setenv=SYSTEMD_BUS_ADDRESS=unix:path=/tmp/application=systemd");

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    setenv("XDG_RUNTIME_DIR", "/tmp/control-runtime", 1);
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("SYSTEMD_BUS_ADDRESS", "unix:path=/tmp/control-systemd", 1);
    const auto control = captureSystemdControlEnvironment();

    setenv("XDG_RUNTIME_DIR", "/tmp/changed-runtime", 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/changed-dbus", 1);
    unsetenv("SYSTEMD_BUS_ADDRESS");
    if (!restoreSystemdControlEnvironment(control)) {
      _exit(2);
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* dbus = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const char* systemd = std::getenv("SYSTEMD_BUS_ADDRESS");
    const bool restored = runtime != nullptr
        && std::string_view(runtime) == "/tmp/control-runtime"
        && dbus == nullptr
        && systemd != nullptr
        && std::string_view(systemd) == "unix:path=/tmp/control-systemd";
    _exit(restored ? 0 : 1);
  }
  if (child < 0) {
    return;
  }
  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
  } else {
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
      CHECK_EQ(WEXITSTATUS(status), 0);
    }
  }
}

UMBRIEL_TEST(managedChildClosesInheritedDescriptors) {
  int inherited[2]{-1, -1};
  CHECK(pipe2(inherited, O_CLOEXEC) == 0);
  if (inherited[0] < 0) {
    return;
  }

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullFd < 0
        || dup2(nullFd, STDIN_FILENO) < 0
        || dup2(nullFd, STDOUT_FILENO) < 0
        || dup2(nullFd, STDERR_FILENO) < 0) {
      _exit(2);
    }
    if (nullFd > STDERR_FILENO) {
      close(nullFd);
    }
    const bool closed = closeChildFileDescriptors()
        && fcntl(STDIN_FILENO, F_GETFD) >= 0
        && fcntl(STDOUT_FILENO, F_GETFD) >= 0
        && fcntl(STDERR_FILENO, F_GETFD) >= 0
        && fcntl(inherited[0], F_GETFD) < 0
        && errno == EBADF
        && fcntl(inherited[1], F_GETFD) < 0
        && errno == EBADF;
    _exit(closed ? 0 : 1);
  }
  closePair(inherited);
  if (child < 0) {
    return;
  }

  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
  } else {
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
      CHECK_EQ(WEXITSTATUS(status), 0);
    }
  }
}

UMBRIEL_TEST(closeRangeFallbackUsesTheHardDescriptorLimit) {
  rlimit originalLimit{};
  CHECK(getrlimit(RLIMIT_NOFILE, &originalLimit) == 0);
  if (originalLimit.rlim_cur <= 128 || originalLimit.rlim_max <= 128) {
    return;
  }

  const int sourceFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  CHECK(sourceFd >= 0);
  if (sourceFd < 0) {
    return;
  }
  const int highFd = fcntl(sourceFd, F_DUPFD_CLOEXEC, 128);
  close(sourceFd);
  CHECK(highFd >= 128);
  if (highFd < 128 || static_cast<rlim_t>(highFd + 1) > originalLimit.rlim_max) {
    if (highFd >= 0) {
      close(highFd);
    }
    return;
  }

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    rlimit childLimit{.rlim_cur = 64, .rlim_max = static_cast<rlim_t>(highFd + 1)};
    if (setrlimit(RLIMIT_NOFILE, &childLimit) != 0) {
      _exit(2);
    }
    gForceCloseRangeFailure = true;
    const bool closed = closeChildFileDescriptors() && fcntl(highFd, F_GETFD) < 0 && errno == EBADF;
    _exit(closed ? 0 : 1);
  }
  close(highFd);
  if (child < 0) {
    return;
  }

  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
  } else {
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
      CHECK_EQ(WEXITSTATUS(status), 0);
    }
  }
}

UMBRIEL_TEST(systemdRunReceivesExactScopeArguments) {
  int outputPipe[2]{-1, -1};
  CHECK(pipe2(outputPipe, O_CLOEXEC | O_NONBLOCK) == 0);
  if (outputPipe[0] < 0) {
    return;
  }

  const ApplicationScopeEnvironmentArguments environmentArguments{
      "--setenv=XDG_RUNTIME_DIR=/tmp/application runtime",
      "--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/application-$bus",
      "--setenv=SYSTEMD_BUS_ADDRESS=unix:path=/tmp/application=systemd",
  };
  const char* command = "printf '%s\\n' '$HOME ${USER} $$'";
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(outputPipe[0]);
    if (dup2(outputPipe[1], STDOUT_FILENO) < 0) {
      _exit(2);
    }
    close(outputPipe[1]);
    execApplicationInScope(
        "/proc/self/exe", "--unit=app-umbriel-arguments.scope", "--property=PartOf=umbriel-session.target",
        "--property=BindsTo=umbriel.service", environmentArguments, command
    );
  }
  close(outputPipe[1]);
  outputPipe[1] = -1;
  if (child < 0) {
    closePair(outputPipe);
    return;
  }

  const std::string output = readLinesWithTimeout(outputPipe[0], 19);
  close(outputPipe[0]);
  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
    return;
  }
  CHECK(WIFEXITED(status));
  if (WIFEXITED(status)) {
    CHECK_EQ(WEXITSTATUS(status), 0);
  }
  CHECK_EQ(
      output,
      std::string("systemd-run\n")
          + "--user\n"
          + "--scope\n"
          + "--quiet\n"
          + "--no-ask-password\n"
          + "--collect\n"
          + "--expand-environment=no\n"
          + "--slice=app.slice\n"
          + "--description=Umbriel application\n"
          + "--unit=app-umbriel-arguments.scope\n"
          + "--property=PartOf=umbriel-session.target\n"
          + "--property=BindsTo=umbriel.service\n"
          + "--setenv=XDG_RUNTIME_DIR=/tmp/application runtime\n"
          + "--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/application-$bus\n"
          + "--setenv=SYSTEMD_BUS_ADDRESS=unix:path=/tmp/application=systemd\n"
          + "--\n"
          + "/bin/sh\n"
          + "-c\n"
          + command
          + "\n"
  );
}

UMBRIEL_TEST(missingSystemdRunFailsClosed) {
  int outputPipe[2]{-1, -1};
  CHECK(pipe2(outputPipe, O_CLOEXEC) == 0);
  if (outputPipe[0] < 0) {
    return;
  }

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(outputPipe[0]);
    if (dup2(outputPipe[1], STDOUT_FILENO) < 0) {
      _exit(2);
    }
    close(outputPipe[1]);
    const ApplicationScopeEnvironmentArguments environmentArguments{};
    execApplicationInScope(
        "/nonexistent/umbriel-systemd-run", "--unit=app-umbriel-missing.scope",
        "--property=PartOf=umbriel-session.target", "--property=BindsTo=umbriel.service", environmentArguments,
        "printf launched"
    );
  }
  close(outputPipe[1]);
  outputPipe[1] = -1;
  if (child < 0) {
    closePair(outputPipe);
    return;
  }

  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
  } else {
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
      CHECK(WEXITSTATUS(status) != 0);
    }
  }
  char output = 0;
  CHECK(read(outputPipe[0], &output, sizeof(output)) == 0);
  close(outputPipe[0]);
}

UMBRIEL_TEST(systemdRunMovesTheCommandIntoAnApplicationScope) {
  if (std::getenv("UMBRIEL_REQUIRE_SYSTEMD_SCOPE_TEST") == nullptr) {
    return;
  }
  const std::string systemdRun = resolveExecutable("systemd-run");
  const std::string systemctl = resolveExecutable("systemctl");
  const std::string ownerUnit = currentSystemdUnit();
  CHECK(!systemdRun.empty());
  CHECK(!systemctl.empty());
  CHECK(!ownerUnit.empty());
  if (systemdRun.empty() || systemctl.empty() || ownerUnit.empty()) {
    return;
  }

  const std::string unit = "app-umbriel-test-"
      + std::to_string(getpid())
      + "-"
      + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
      + ".scope";
  const std::string unitArgument = "--unit=" + unit;
  const std::string partOfProperty = "--property=PartOf=umbriel-session.target";
  const std::string bindsToProperty = "--property=BindsTo=" + ownerUnit;
  const ApplicationScopeEnvironmentArguments environmentArguments{
      "--setenv=XDG_RUNTIME_DIR=/tmp/umbriel application $runtime",
      "--setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/umbriel-application-dbus",
      "--setenv=SYSTEMD_BUS_ADDRESS=unix:path=/tmp/umbriel-application=systemd",
  };
  int inputPipe[2]{-1, -1};
  int outputPipe[2]{-1, -1};
  CHECK(pipe2(inputPipe, O_CLOEXEC) == 0);
  CHECK(pipe2(outputPipe, O_CLOEXEC) == 0);
  if (inputPipe[0] < 0 || outputPipe[0] < 0) {
    closePair(inputPipe);
    closePair(outputPipe);
    return;
  }

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(inputPipe[1]);
    close(outputPipe[0]);
    if (dup2(inputPipe[0], STDIN_FILENO) < 0 || dup2(outputPipe[1], STDOUT_FILENO) < 0) {
      _exit(2);
    }
    close(inputPipe[0]);
    close(outputPipe[1]);
    if (!closeChildFileDescriptors()) {
      _exit(2);
    }
    execApplicationInScope(
        systemdRun.c_str(), unitArgument.c_str(), partOfProperty.c_str(), bindsToProperty.c_str(), environmentArguments,
        "printf '%s\\n' '$HOME ${USER} $$' \"$XDG_RUNTIME_DIR\" \"$DBUS_SESSION_BUS_ADDRESS\" "
        "\"$SYSTEMD_BUS_ADDRESS\"; cat /proc/self/cgroup; read ignored || :"
    );
  }

  close(inputPipe[0]);
  close(outputPipe[1]);
  inputPipe[0] = -1;
  outputPipe[1] = -1;
  if (child < 0) {
    closePair(inputPipe);
    closePair(outputPipe);
    return;
  }

  const std::string output = readLinesWithTimeout(outputPipe[0], 5);
  std::istringstream outputStream(output);
  std::string literal;
  std::string runtime;
  std::string dbus;
  std::string systemd;
  std::getline(outputStream, literal);
  std::getline(outputStream, runtime);
  std::getline(outputStream, dbus);
  std::getline(outputStream, systemd);
  const std::string cgroup{std::istreambuf_iterator<char>(outputStream), std::istreambuf_iterator<char>()};
  CHECK_EQ(literal, "$HOME ${USER} $$");
  CHECK_EQ(runtime, "/tmp/umbriel application $runtime");
  CHECK_EQ(dbus, "unix:path=/tmp/umbriel-application-dbus");
  CHECK_EQ(systemd, "unix:path=/tmp/umbriel-application=systemd");
  CHECK(cgroup.contains("/app.slice/" + unit));

  CHECK_EQ(systemctlProperty(systemctl, unit, "Slice"), "app.slice");
  CHECK_EQ(systemctlProperty(systemctl, unit, "CollectMode"), "inactive-or-failed");
  CHECK_EQ(systemctlProperty(systemctl, unit, "Description"), "Umbriel application");
  const std::string partOf = systemctlProperty(systemctl, unit, "PartOf");
  CHECK(containsWord(partOf, "umbriel-session.target"));
  const std::string bindsTo = systemctlProperty(systemctl, unit, "BindsTo");
  CHECK(containsWord(bindsTo, ownerUnit));

  close(inputPipe[1]);
  inputPipe[1] = -1;
  close(outputPipe[0]);
  outputPipe[0] = -1;
  int status = 0;
  if (!waitForExit(child, &status)) {
    kill(child, SIGKILL);
    waitForExit(child, &status);
    CHECK(false);
  } else {
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
      CHECK_EQ(WEXITSTATUS(status), 0);
    }
  }
}

int main(int argc, char** argv) {
  if (argc > 0 && std::strcmp(argv[0], "systemd-run") == 0) {
    return captureExecArguments(argc, argv);
  }
  return RUN_TESTS();
}
