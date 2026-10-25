#include "input/eis_socket.h"

#include "check.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace {

  class Fd {
  public:
    explicit Fd(int fd = -1) : m_fd(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
      reset(std::exchange(other.m_fd, -1));
      return *this;
    }
    [[nodiscard]] int get() const { return m_fd; }
    void reset(int fd = -1) {
      if (m_fd >= 0) {
        close(m_fd);
      }
      m_fd = fd;
    }

  private:
    int m_fd;
  };

  struct Fixture {
    std::string directory;
    std::optional<std::string> previousRuntime;
    wl_event_loop* loop = nullptr;
    bool allow = true;
    bool open = true;
    int opened = 0;
    int relayFd = -1;
    std::vector<pid_t> authenticated;
    Fd backend;
    std::unique_ptr<umbriel::EisSocket> socket;

    Fixture() {
      if (const char* previous = std::getenv("XDG_RUNTIME_DIR")) {
        previousRuntime = previous;
      }
      std::array<char, 64> pattern{};
      std::strcpy(pattern.data(), "/tmp/umbriel-eis-socket-test.XXXXXX");
      if (mkdtemp(pattern.data()) == nullptr) {
        return;
      }
      directory = pattern.data();
      setenv("XDG_RUNTIME_DIR", directory.c_str(), 1);
      loop = wl_event_loop_create();
      if (loop == nullptr) {
        return;
      }
      socket = std::make_unique<umbriel::EisSocket>(
          loop,
          [this](pid_t pid) {
            authenticated.push_back(pid);
            return allow;
          },
          [this] {
            ++opened;
            if (!open) {
              return -1;
            }
            std::array<int, 2> pair{};
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0) {
              return -1;
            }
            backend.reset(pair[0]);
            relayFd = pair[1];
            const int size = 4096;
            setsockopt(pair[0], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
            setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
            setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
            return pair[1];
          }
      );
    }

    ~Fixture() {
      socket.reset();
      backend.reset();
      if (loop != nullptr) {
        wl_event_loop_destroy(loop);
      }
      if (previousRuntime) {
        setenv("XDG_RUNTIME_DIR", previousRuntime->c_str(), 1);
      } else {
        unsetenv("XDG_RUNTIME_DIR");
      }
      if (!directory.empty()) {
        std::filesystem::remove_all(directory);
      }
    }

    [[nodiscard]] bool valid() const { return socket != nullptr && socket->fd() >= 0; }

    [[nodiscard]] Fd connect() const {
      Fd peer(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
      if (peer.get() < 0 || !valid()) {
        return Fd();
      }
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const std::string path = "/proc/self/fd/" + std::to_string(socket->fd());
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      if (::connect(peer.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return Fd();
      }
      const int size = 4096;
      setsockopt(peer.get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
      setsockopt(peer.get(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
      return peer;
    }

    template <typename Predicate> bool until(Predicate predicate) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
          return true;
        }
        if (wl_event_loop_dispatch(loop, 2) < 0) {
          return false;
        }
      }
      return predicate();
    }
  };

  bool disconnected(int fd) {
    char byte = 0;
    const ssize_t result = recv(fd, &byte, 1, MSG_DONTWAIT);
    return result == 0 || (result < 0 && (errno == ECONNRESET || errno == ENOTCONN));
  }

  size_t descriptorCount() {
    return static_cast<size_t>(
        std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator())
    );
  }

  std::string payload(size_t size, unsigned seed) {
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<char>((i * 31U + (i >> 8U) + seed) % 251U);
    }
    return bytes;
  }

  bool sendBytes(int fd, const std::string& bytes, size_t& sent) {
    if (sent == bytes.size()) {
      return true;
    }
    const ssize_t count = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<size_t>(count);
      return true;
    }
    return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }

  bool receiveBytes(int fd, std::string& bytes) {
    std::array<char, 4096> buffer{};
    const ssize_t count = recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count > 0) {
      bytes.append(buffer.data(), static_cast<size_t>(count));
      return true;
    }
    return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
  }

  bool sendRights(int fd, const std::vector<int>& descriptors) {
    std::vector<char> control(CMSG_SPACE(descriptors.size() * sizeof(int)));
    char byte = 'k';
    iovec iov{.iov_base = &byte, .iov_len = 1};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(descriptors.size() * sizeof(int));
    std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size() * sizeof(int));
    return sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL) == 1;
  }

  bool receiveRights(int fd, Fd& received) {
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    char byte = 0;
    iovec iov{.iov_base = &byte, .iov_len = 1};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    if (recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) != 1) {
      return false;
    }
    std::vector<Fd> descriptors;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS && header->cmsg_len >= CMSG_LEN(0)) {
        const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
          int descriptor = -1;
          std::memcpy(&descriptor, CMSG_DATA(header) + i * sizeof(int), sizeof(descriptor));
          descriptors.emplace_back(descriptor);
        }
      }
    }
    if (byte != 'k' || (message.msg_flags & MSG_CTRUNC) != 0 || descriptors.size() != 1) {
      return false;
    }
    received = std::move(descriptors.front());
    return true;
  }

} // namespace

UMBRIEL_TEST(authenticateTheRealSocketPeerBeforeOpeningBackend) {
  Fixture fixture;
  CHECK(fixture.valid());
  if (!fixture.valid()) {
    return;
  }
  fixture.allow = false;
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child < 0) {
    return;
  }
  if (child == 0) {
    Fd peer = fixture.connect();
    if (peer.get() < 0) {
      _exit(2);
    }
    pollfd descriptor{.fd = peer.get(), .events = POLLIN, .revents = 0};
    _exit(poll(&descriptor, 1, 3000) > 0 && disconnected(peer.get()) ? 0 : 3);
  }
  CHECK(fixture.until([&] { return !fixture.authenticated.empty(); }));
  CHECK_EQ(fixture.authenticated.size(), 1U);
  if (!fixture.authenticated.empty()) {
    CHECK_EQ(fixture.authenticated.front(), child);
  }
  CHECK_EQ(fixture.opened, 0);
  int status = 0;
  CHECK_EQ(waitpid(child, &status, 0), child);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);
}

UMBRIEL_TEST(relayBothDirectionsUnderBackpressureWithoutLosingOrder) {
  Fixture fixture;
  CHECK(fixture.valid());
  if (!fixture.valid()) {
    return;
  }
  Fd peer = fixture.connect();
  CHECK(peer.get() >= 0);
  CHECK(fixture.until([&] { return fixture.backend.get() >= 0; }));
  if (peer.get() < 0 || fixture.backend.get() < 0) {
    return;
  }
  CHECK((fcntl(fixture.socket->fd(), F_GETFD) & FD_CLOEXEC) != 0);
  CHECK((fcntl(fixture.relayFd, F_GETFD) & FD_CLOEXEC) != 0);
  const std::string forward = payload(512 * 1024 + 37, 17);
  const std::string reverse = payload(384 * 1024 + 91, 43);
  size_t forwardSent = 0;
  size_t reverseSent = 0;
  bool forwardBlocked = false;
  bool reverseBlocked = false;
  // Neither destination reads until both paths have actually filled their socket buffers and the relay queues.
  for (int iteration = 0; iteration < 100; ++iteration) {
    const size_t beforeForward = forwardSent;
    const size_t beforeReverse = reverseSent;
    CHECK(sendBytes(peer.get(), forward, forwardSent));
    CHECK(sendBytes(fixture.backend.get(), reverse, reverseSent));
    CHECK_EQ(wl_event_loop_dispatch(fixture.loop, 0), 0);
    forwardBlocked = forwardBlocked || (forwardSent == beforeForward && forwardSent < forward.size());
    reverseBlocked = reverseBlocked || (reverseSent == beforeReverse && reverseSent < reverse.size());
  }
  CHECK(forwardBlocked);
  CHECK(reverseBlocked);
  std::string atBackend;
  std::string atPeer;
  bool healthy = true;
  CHECK(fixture.until([&] {
    healthy = healthy
        && sendBytes(peer.get(), forward, forwardSent)
        && sendBytes(fixture.backend.get(), reverse, reverseSent)
        && receiveBytes(fixture.backend.get(), atBackend)
        && receiveBytes(peer.get(), atPeer);
    return !healthy || (atBackend.size() == forward.size() && atPeer.size() == reverse.size());
  }));
  CHECK(healthy);
  CHECK(atBackend == forward);
  CHECK(atPeer == reverse);
}

UMBRIEL_TEST(relayKeymapDescriptorsInBothDirections) {
  Fixture fixture;
  CHECK(fixture.valid());
  if (!fixture.valid()) {
    return;
  }
  Fd peer = fixture.connect();
  CHECK(peer.get() >= 0);
  CHECK(fixture.until([&] { return fixture.backend.get() >= 0; }));
  if (peer.get() < 0 || fixture.backend.get() < 0) {
    return;
  }
  std::string path = fixture.directory + "/keymap.XXXXXX";
  Fd keymap(mkstemp(path.data()));
  CHECK(keymap.get() >= 0);
  if (keymap.get() < 0) {
    return;
  }
  unlink(path.c_str());
  const std::string contents = "xkb_keymap { test marker; };";
  CHECK_EQ(write(keymap.get(), contents.data(), contents.size()), static_cast<ssize_t>(contents.size()));
  for (const bool towardBackend : {true, false}) {
    const int sender = towardBackend ? peer.get() : fixture.backend.get();
    const int receiver = towardBackend ? fixture.backend.get() : peer.get();
    CHECK(sendRights(sender, {keymap.get()}));
    Fd received;
    CHECK(fixture.until([&] { return receiveRights(receiver, received); }));
    if (received.get() < 0) {
      continue;
    }
    CHECK((fcntl(received.get(), F_GETFD) & FD_CLOEXEC) != 0);
    std::array<char, 128> bytes{};
    const ssize_t count = pread(received.get(), bytes.data(), bytes.size(), 0);
    CHECK_EQ(count, static_cast<ssize_t>(contents.size()));
    if (count >= 0) {
      CHECK_EQ(std::string(bytes.data(), static_cast<size_t>(count)), contents);
    }
  }
}

UMBRIEL_TEST(truncatedRightsDisconnectAndReleaseEveryReceivedDescriptor) {
  Fixture fixture;
  CHECK(fixture.valid());
  if (!fixture.valid()) {
    return;
  }
  const size_t baseline = descriptorCount();
  Fd peer = fixture.connect();
  CHECK(peer.get() >= 0);
  CHECK(fixture.until([&] { return fixture.backend.get() >= 0; }));
  if (peer.get() < 0 || fixture.backend.get() < 0) {
    return;
  }
  Fd marker(open("/dev/null", O_RDONLY | O_CLOEXEC));
  CHECK(marker.get() >= 0);
  if (marker.get() < 0) {
    return;
  }
  CHECK(sendRights(peer.get(), std::vector<int>(80, marker.get())));
  CHECK(fixture.until([&] { return disconnected(peer.get()) && disconnected(fixture.backend.get()); }));
  // The two application endpoints and original marker remain; every relay-owned descriptor is gone.
  CHECK_EQ(descriptorCount(), baseline + 3);
}

UMBRIEL_TEST(halfCloseDrainsPayloadAndPreservesTheReverseDirection) {
  Fixture fixture;
  CHECK(fixture.valid());
  if (!fixture.valid()) {
    return;
  }
  const size_t baseline = descriptorCount();
  Fd peer = fixture.connect();
  CHECK(peer.get() >= 0);
  CHECK(fixture.until([&] { return fixture.backend.get() >= 0; }));
  if (peer.get() < 0 || fixture.backend.get() < 0) {
    return;
  }
  const std::string request = "final request before half close";
  CHECK_EQ(send(peer.get(), request.data(), request.size(), MSG_NOSIGNAL), static_cast<ssize_t>(request.size()));
  CHECK_EQ(shutdown(peer.get(), SHUT_WR), 0);
  std::string atBackend;
  CHECK(fixture.until([&] {
    receiveBytes(fixture.backend.get(), atBackend);
    return atBackend.size() == request.size();
  }));
  CHECK_EQ(atBackend, request);
  CHECK(fixture.until([&] { return disconnected(fixture.backend.get()); }));
  const std::string reply = "reply after peer stopped sending";
  CHECK_EQ(send(fixture.backend.get(), reply.data(), reply.size(), MSG_NOSIGNAL), static_cast<ssize_t>(reply.size()));
  CHECK_EQ(shutdown(fixture.backend.get(), SHUT_WR), 0);
  std::string atPeer;
  CHECK(fixture.until([&] {
    receiveBytes(peer.get(), atPeer);
    return atPeer.size() == reply.size();
  }));
  CHECK_EQ(atPeer, reply);
  CHECK(fixture.until([&] { return disconnected(peer.get()); }));
  CHECK_EQ(descriptorCount(), baseline + 2);
}

int main() { return RUN_TESTS(); }
