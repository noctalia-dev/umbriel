#include "check.h"
#include "core/process.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {
  constexpr const char* kVariable = "UMBRIEL_TEST_READY_FD";

  void exportDescriptor(int fd) { setenv(kVariable, std::to_string(fd).c_str(), 1); }
} // namespace

UMBRIEL_TEST(absentReadinessPipeIsOptional) {
  unsetenv(kVariable);
  umbriel::ServiceReadyPipe ready(kVariable);
  CHECK(!ready.present());
  CHECK(ready.valid());
  CHECK(ready.notify());
}

UMBRIEL_TEST(readinessPipeReportsOnceAndClosesItsDescriptor) {
  std::array<int, 2> fds{};
  CHECK_EQ(pipe(fds.data()), 0);
  exportDescriptor(fds[1]);
  umbriel::ServiceReadyPipe ready(kVariable);
  CHECK(ready.present());
  CHECK(ready.valid());
  CHECK(std::getenv(kVariable) == nullptr);
  CHECK((fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0);
  CHECK(ready.notify());
  std::array<char, 32> message{};
  const auto size = read(fds[0], message.data(), message.size());
  CHECK_EQ(size, 8);
  CHECK_EQ(std::string(message.data(), 8), std::string("READY=1\n"));
  CHECK_EQ(read(fds[0], message.data(), message.size()), 0);
  CHECK_EQ(fcntl(fds[1], F_GETFD), -1);
  CHECK_EQ(errno, EBADF);
  CHECK(!ready.notify());
  close(fds[0]);
}

UMBRIEL_TEST(abandonedReadinessPipeClosesWithoutReporting) {
  std::array<int, 2> fds{};
  CHECK_EQ(pipe(fds.data()), 0);
  exportDescriptor(fds[1]);
  {
    const umbriel::ServiceReadyPipe ready(kVariable);
  }
  char message{};
  CHECK_EQ(read(fds[0], &message, 1), 0);
  close(fds[0]);
}

UMBRIEL_TEST(readinessRejectsInvalidAndNonWritableDescriptors) {
  for (const char* value : {"", "0", "2", "-1", "3bad", "99999999999999999999"}) {
    setenv(kVariable, value, 1);
    umbriel::ServiceReadyPipe ready(kVariable);
    CHECK(ready.present());
    CHECK(!ready.valid());
    CHECK(std::getenv(kVariable) == nullptr);
  }
  std::array<int, 2> fds{};
  CHECK_EQ(pipe(fds.data()), 0);
  exportDescriptor(fds[0]);
  const umbriel::ServiceReadyPipe readOnly(kVariable);
  CHECK(!readOnly.valid());
  close(fds[0]);
  close(fds[1]);
  const int file = open("/dev/null", O_WRONLY | O_CLOEXEC);
  CHECK(file >= 0);
  exportDescriptor(file);
  const umbriel::ServiceReadyPipe regularFile(kVariable);
  CHECK(!regularFile.valid());
  close(file);
}

UMBRIEL_TEST(closedReadinessReaderDoesNotRaiseSigpipe) {
  std::array<int, 2> fds{};
  CHECK_EQ(pipe(fds.data()), 0);
  exportDescriptor(fds[1]);
  umbriel::ServiceReadyPipe ready(kVariable);
  close(fds[0]);
  struct sigaction original{}, action{};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  CHECK_EQ(sigaction(SIGPIPE, &action, &original), 0);
  CHECK(!ready.notify());
  CHECK_EQ(errno, EPIPE);
  CHECK_EQ(sigaction(SIGPIPE, &original, nullptr), 0);
}

int main() { return RUN_TESTS(); }
