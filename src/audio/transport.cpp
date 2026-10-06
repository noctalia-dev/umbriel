#include "audio/transport.h"

#include <array>
#include <cerrno>
#include <sys/socket.h>

namespace umbriel::audio {

  IoResult receivePacket(int fd, std::vector<uint8_t>& packet, bool configuration) {
    std::array<uint8_t, kMaxConfigurationBytes> buffer{};
    iovec iov{.iov_base = buffer.data(), .iov_len = configuration ? kMaxConfigurationBytes : kMaxPacketBytes};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    const auto count = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    packet.clear();
    if (count < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? IoResult::Again : IoResult::Closed;
    }
    if (count == 0) {
      return IoResult::Closed;
    }
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
      return IoResult::Malformed;
    }
    packet.assign(buffer.begin(), buffer.begin() + count);
    return IoResult::Complete;
  }

  IoResult sendPacket(int fd, std::span<const uint8_t> packet) {
    if (packet.empty() || packet.size() > kMaxConfigurationBytes) {
      return IoResult::Malformed;
    }
    const auto count = send(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? IoResult::Again : IoResult::Closed;
    }
    return static_cast<size_t>(count) == packet.size() ? IoResult::Complete : IoResult::Malformed;
  }

  IoResult CoalescedWriter::flush(int fd) {
    if (m_pending.empty()) {
      return IoResult::Complete;
    }
    const auto result = sendPacket(fd, m_pending);
    if (result == IoResult::Complete) {
      m_pending.clear();
    }
    return result;
  }

} // namespace umbriel::audio
