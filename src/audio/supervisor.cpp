#include "audio/supervisor.h"

#include "audio/transport.h"
#include "core/fdlimit.h"
#include "core/process.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace umbriel::audio {

  namespace {
    uint64_t nextEpoch() {
      // The compositor is single-threaded. Keep the issuer outside individual
      // supervisors so reset/config replacement cannot reuse a host epoch.
      static uint64_t epoch = 0;
      return ++epoch;
    }

    uint64_t nowNs() {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    }

  } // namespace

  Supervisor::Supervisor(
      wl_event_loop* loop, Configuration configuration, std::string executable, std::vector<std::string> arguments
  )
      : m_loop(loop), m_configuration(std::move(configuration)), m_executable(std::move(executable)),
        m_arguments(std::move(arguments)) {}

  Supervisor::~Supervisor() {
    closeChannel();
    signalChild(m_pidfd, SIGKILL);
    closeWatch();
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
  }

  std::string_view Supervisor::state() const {
    if (!m_demand) {
      return "unused";
    }
    if (!m_active) {
      return "suspended";
    }
    if (m_retry.exhausted()) {
      return "retry_exhausted";
    }
    if (m_retry.deadlineNs() && !running()) {
      return "retrying";
    }
    if (!running()) {
      return "launch_failed";
    }
    if (m_stopPhase != StopPhase::None) {
      return "stopping";
    }
    return !m_receiver.ready() ? "starting" : m_receiver.input().available ? "available" : "unavailable";
  }

  void Supervisor::publishChange(uint64_t before) {
    if (m_receiver.revision() != before && changed) {
      changed();
    }
  }

  void Supervisor::setDemand(bool demand) {
    if (demand == m_demand) {
      return;
    }
    m_demand = demand;
    m_retry.setDemand(m_demand && m_active);
    if (!m_demand) {
      const auto before = m_receiver.revision();
      m_receiver.clear();
      publishChange(before);
      stop(false);
    } else if (m_active && !running()) {
      launch();
    }
    arm();
  }

  void Supervisor::setSessionActive(bool active) {
    if (active == m_active) {
      return;
    }
    m_active = active;
    m_retry.setDemand(m_demand && m_active);
    if (!active) {
      const auto before = m_receiver.revision();
      m_receiver.clear();
      publishChange(before);
      stop(false);
    } else if (m_demand && !running()) {
      launch();
    }
    arm();
  }

  void Supervisor::reset() {
    m_retry.reset();
    if (running()) {
      stop(false);
    } else if (m_demand && m_active) {
      launch();
    }
    arm();
  }

  void Supervisor::launch() {
    if (!m_demand || !m_active || running() || m_retry.exhausted()) {
      return;
    }
    m_failed = false;
    m_stopPhase = StopPhase::None;
    m_epoch = nextEpoch();
    m_configuration.epoch = m_epoch;
    const auto configuration = encode(m_configuration);
    // Allocate the only timer before spawning so failure cannot strand a child.
    if (m_timer == nullptr) {
      m_timer = wl_event_loop_add_timer(m_loop, onTimer, this);
    }
    if (m_timer == nullptr) {
      m_retry.reset();
      return;
    }
    if (configuration.empty() || m_executable.empty() || m_executable.front() != '/') {
      m_retry.failed(nowNs());
      arm();
      return;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0) {
      m_retry.failed(nowNs());
      arm();
      return;
    }
    std::vector<char*> argv;
    argv.push_back(m_executable.data());
    for (auto& argument : m_arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
      // Reuse compositor child hygiene. stdin is the explicit inherited audio
      // channel; all other nonstandard fds are closed before direct exec.
      resetChildSignalState();
      if (dup2(sockets[1], STDIN_FILENO) < 0 || fcntl(STDIN_FILENO, F_SETFD, 0) < 0 || !closeChildFileDescriptors()) {
        _exit(126);
      }
      // Gate exec until the parent owns an exit watch. If pidfd/watch setup
      // fails, channel EOF makes this known child code exit without acquiring
      // anything, even when no race-free signal handle could be obtained.
      pollfd gate{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
      unsigned char proceed = 0;
      if (poll(&gate, 1, 1000) <= 0 || recv(STDIN_FILENO, &proceed, 1, 0) != 1 || proceed != 1) {
        _exit(126);
      }
      restoreFileDescriptorLimit();
      execv(m_executable.c_str(), argv.data());
      _exit(127);
    }
    close(sockets[1]);
    if (pid < 0) {
      close(sockets[0]);
      m_retry.failed(nowNs());
      arm();
      return;
    }
    m_channel = sockets[0];
    if (!watchChildExit(m_loop, pid, onExit, this, m_pidfd, m_exitSource)) {
      // Gated child exits on EOF if pidfd allocation failed. If only watcher
      // allocation failed, the retained pidfd also permits race-free KILL.
      signalChild(m_pidfd, SIGKILL);
      closeChannel();
      closeWatch();
      m_retry.failed(nowNs());
      arm();
      return;
    }
    const std::array<uint8_t, 1> proceed{1};
    if (sendPacket(m_channel, proceed) != IoResult::Complete) {
      stop(true);
      arm();
      return;
    }
    m_channelSource = wl_event_loop_add_fd(m_loop, m_channel, WL_EVENT_READABLE, onChannel, this);
    if (m_channelSource == nullptr || sendPacket(m_channel, configuration) != IoResult::Complete) {
      stop(true);
      arm();
      return;
    }
    const auto before = m_receiver.revision();
    m_receiver.begin(m_epoch, m_configuration.source, nowNs());
    publishChange(before);
    arm();
  }

  void Supervisor::closeChannel() {
    if (m_channelSource != nullptr) {
      wl_event_source_remove(m_channelSource);
      m_channelSource = nullptr;
    }
    if (m_channel >= 0) {
      close(m_channel);
      m_channel = -1;
    }
  }

  void Supervisor::closeWatch() { closeChildExitWatch(m_pidfd, m_exitSource); }

  void Supervisor::stop(bool failure) {
    closeChannel();
    m_failed = m_failed || failure;
    if (failure) {
      const auto before = m_receiver.revision();
      m_receiver.disconnected(nowNs());
      publishChange(before);
    }
    if (running() && m_stopPhase == StopPhase::None) {
      m_stopPhase = StopPhase::Eof;
      m_stopNs = nowNs();
    }
  }

  void Supervisor::exited() {
    const bool unexpected = m_stopPhase == StopPhase::None;
    closeChannel();
    closeWatch();
    const auto before = m_receiver.revision();
    m_receiver.disconnected(nowNs());
    publishChange(before);
    if (m_failed || unexpected) {
      m_retry.failed(nowNs());
    } else if (m_demand && m_active) {
      launch();
    }
    arm();
    if (!running() && stopped) {
      stopped();
    }
  }

  int Supervisor::onChannel(int /*fd*/, uint32_t mask, void* data) {
    auto& self = *static_cast<Supervisor*>(data);
    const auto before = self.m_receiver.revision();
    // A service has at most four sources, so this per-callback cap also bounds
    // a dispatch to 4 * 16 packets. No drain-until-empty loop.
    for (size_t i = 0; i < kSourceReceiveBudget; ++i) {
      std::vector<uint8_t> packet;
      const auto result = receivePacket(self.m_channel, packet);
      if (result == IoResult::Again) {
        break;
      }
      if (result != IoResult::Complete || self.m_receiver.receive(packet, nowNs()) == ReceiveResult::Flood) {
        self.stop(true);
        break;
      }
    }
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
      self.stop(true);
    }
    self.publishChange(before);
    self.arm();
    return 0;
  }

  int Supervisor::onExit(int /*fd*/, uint32_t /*mask*/, void* data) {
    static_cast<Supervisor*>(data)->exited();
    return 0;
  }

  int Supervisor::onTimer(void* data) {
    auto& self = *static_cast<Supervisor*>(data);
    self.m_timerDeadlineNs = 0;
    self.tick();
    return 0;
  }

  void Supervisor::tick() {
    const auto now = nowNs();
    const auto before = m_receiver.revision();
    m_receiver.advance(now);
    if (m_stopPhase == StopPhase::None
        && running()
        && (m_receiver.readyExpired(now) || m_receiver.transportExpired(now))) {
      stop(true);
    }
    if (running() && m_stopPhase == StopPhase::Eof && now - m_stopNs >= kEofGraceNs) {
      signalChild(m_pidfd, SIGTERM);
      m_stopPhase = StopPhase::Term;
      m_stopNs = now;
    } else if (running() && m_stopPhase == StopPhase::Term && now - m_stopNs >= kTermGraceNs) {
      signalChild(m_pidfd, SIGKILL);
      m_stopPhase = StopPhase::Kill;
    }
    if (!running() && m_retry.deadlineNs() && now >= *m_retry.deadlineNs()) {
      launch();
    }
    publishChange(before);
    arm();
  }

  void Supervisor::arm() {
    const auto now = nowNs();
    uint64_t deadline = 0;
    const auto consider = [&](uint64_t candidate) {
      if (candidate != 0 && (deadline == 0 || candidate < deadline)) {
        deadline = candidate;
      }
    };
    if (running() && m_stopPhase == StopPhase::None) {
      consider(m_receiver.deadlineNs(now));
    } else if (running() && m_stopPhase == StopPhase::Eof) {
      consider(m_stopNs + kEofGraceNs);
    } else if (running() && m_stopPhase == StopPhase::Term) {
      consider(m_stopNs + kTermGraceNs);
    }
    if (m_retry.deadlineNs() && !running()) {
      consider(*m_retry.deadlineNs());
    }
    if (m_receiver.decaying()) {
      consider(now + 16'000'000);
    }
    if (deadline != 0 && m_timer == nullptr) {
      m_timer = wl_event_loop_add_timer(m_loop, onTimer, this);
      if (m_timer == nullptr) {
        // No event source means no scheduled retry: stay unavailable until an
        // explicit reset or new demand.
        m_retry.reset();
        return;
      }
    }
    if (deadline != 0 && m_timer != nullptr) {
      // Never postpone an armed watchdog in response to incoming packets.
      // In particular, heartbeats cannot extend observation/READY deadlines.
      if (m_timerDeadlineNs == 0 || deadline < m_timerDeadlineNs) {
        const uint64_t delayMs = deadline > now ? (deadline - now + 999'999) / 1'000'000 : 1;
        wl_event_source_timer_update(m_timer, static_cast<int>(std::min<uint64_t>(delayMs, INT_MAX)));
        m_timerDeadlineNs = deadline;
      }
    } else if (deadline == 0 && m_timer != nullptr) {
      wl_event_source_remove(m_timer);
      m_timer = nullptr;
      m_timerDeadlineNs = 0;
    }
  }

} // namespace umbriel::audio
