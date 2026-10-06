#include "audio/transport.h"
#include "check.h"

#include <sys/socket.h>
#include <unistd.h>

using namespace umbriel::audio;

namespace {
  struct Channel {
    int fd[2]{-1, -1};
    Channel() { CHECK_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd), 0); }
    ~Channel() {
      close(fd[0]);
      close(fd[1]);
    }
  };
} // namespace

UMBRIEL_TEST(seqpacketPreservesBoundariesAndHasNonblockingEof) {
  Channel channel;
  std::vector<uint8_t> received;
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Again);
  const auto first = encode(Ready{.epoch = 1});
  const auto second = encode(Status{.epoch = 1, .generation = 2});
  CHECK_EQ(sendPacket(channel.fd[1], first), IoResult::Complete);
  CHECK_EQ(sendPacket(channel.fd[1], second), IoResult::Complete);
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Complete);
  CHECK_EQ(received, first);
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Complete);
  CHECK_EQ(received, second);
  close(channel.fd[1]);
  channel.fd[1] = -1;
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Closed);
  CHECK_EQ(sendPacket(channel.fd[0], first), IoResult::Closed);
}

UMBRIEL_TEST(oversizedDatagramCannotBeAcceptedAsValidPrefix) {
  Channel channel;
  auto oversized = encode(Snapshot{.epoch = 1});
  oversized.resize(kMaxPacketBytes + 1);
  CHECK_EQ(sendPacket(channel.fd[1], oversized), IoResult::Complete);
  std::vector<uint8_t> received;
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Malformed);
  CHECK(received.empty());
  const auto config = encode(
      Configuration{
          .epoch = 1, .source = SourceType::Playback, .selector = Selector::Fixed, .target = std::string(1024, 'a')
      }
  );
  CHECK_EQ(sendPacket(channel.fd[1], config), IoResult::Complete);
  CHECK_EQ(receivePacket(channel.fd[0], received, true), IoResult::Complete);
  CHECK_EQ(received, config);
}

UMBRIEL_TEST(backpressureKeepsOnlyNewestPendingMeasurement) {
  Channel channel;
  int bytes = 1024;
  CHECK_EQ(setsockopt(channel.fd[1], SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)), 0);
  const auto heartbeat = encode(Status{.epoch = 1});
  size_t filled = 0;
  while (sendPacket(channel.fd[1], heartbeat) == IoResult::Complete && filled < 10000) {
    ++filled;
  }
  CHECK(filled > 0 && filled < 10000);
  CoalescedWriter writer;
  writer.replace(Snapshot{.epoch = 1, .generation = 1, .sequence = 1});
  CHECK_EQ(writer.flush(channel.fd[1]), IoResult::Again);
  CHECK(writer.pending());
  writer.replace(Snapshot{.epoch = 1, .generation = 1, .sequence = 99});
  std::vector<uint8_t> received;
  for (size_t i = 0; i < filled; ++i) {
    CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Complete);
  }
  CHECK_EQ(writer.flush(channel.fd[1]), IoResult::Complete);
  CHECK(!writer.pending());
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Complete);
  CHECK_EQ(std::get<Snapshot>(*decode(received)).sequence, 99U);
  CHECK_EQ(receivePacket(channel.fd[0], received), IoResult::Again);
}

int main() { return RUN_TESTS(); }
