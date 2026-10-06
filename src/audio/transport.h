#pragma once

#include "audio/protocol.h"

namespace umbriel::audio {

  enum class IoResult { Complete, Again, Closed, Malformed };

  // SOCK_SEQPACKET boundaries are mandatory. Truncation is an error, never a
  // prefix of a valid packet. These functions never wait or retry in a loop.
  [[nodiscard]] IoResult receivePacket(int fd, std::vector<uint8_t>& packet, bool configuration = false);
  [[nodiscard]] IoResult sendPacket(int fd, std::span<const uint8_t> packet);

  class CoalescedWriter {
  public:
    void replace(const Packet& packet) { m_pending = encode(packet); }
    [[nodiscard]] IoResult flush(int fd);
    [[nodiscard]] bool pending() const { return !m_pending.empty(); }

  private:
    std::vector<uint8_t> m_pending;
  };

} // namespace umbriel::audio
