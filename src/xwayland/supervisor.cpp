#include "xwayland/supervisor.h"

#include "config/config.h"
#include "core/fdlimit.h"
#include "core/log.h"
#include "core/process.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("xwayland");

    // Displays past this are not worth scanning; a machine with 32 X servers
    // already running has other problems.
    constexpr int kMaxDisplay = 32;
    // A satellite that survives this long counts as healthy, so its next crash
    // starts a fresh budget rather than counting toward a burst from hours ago.
    constexpr auto kHealthyRuntime = std::chrono::seconds(60);
    constexpr int kMaxFailures = 5;
    constexpr int kRespawnDelayMs = 1000;
    // execlp could not find the binary. Respawning cannot help.
    constexpr int kExecFailedStatus = 127;
  } // namespace

  XwaylandSupervisor::XwaylandSupervisor(wl_event_loop* loop, std::string waylandSocket)
      : m_loop(loop), m_waylandSocket(std::move(waylandSocket)) {}

  XwaylandSupervisor::~XwaylandSupervisor() { stop(); }

  void XwaylandSupervisor::start() {
    m_executable = resolveExecutable("xwayland-satellite");
    if (m_executable.empty()) {
      kLog.info("xwayland-satellite not on PATH; skipping");
      return;
    }

    // Claim a display number by finding one nobody has taken. This is a check-then-use race by construction: another
    // compositor starting at the same instant can take the same slot between the test and satellite's bind. Left alone
    // deliberately: the socket is xwayland-satellite's to create, so it owns the decision, and losing the race costs a
    // respawn rather than corrupting anything.
    for (int n = 0; n < kMaxDisplay; ++n) {
      const std::string num = std::to_string(n);
      if (std::filesystem::exists("/tmp/.X11-unix/X" + num) || std::filesystem::exists("/tmp/.X" + num + "-lock")) {
        continue;
      }
      m_display = ":" + num;
      setenv("DISPLAY", m_display.c_str(), 1);
      m_environment = config().environment.variables;
      kLog.info("using DISPLAY={}", m_display);
      spawn();
      return;
    }
    kLog.error("no free X display in :0..:{}; disabling xwayland", kMaxDisplay - 1);
  }

  void XwaylandSupervisor::spawn() {
    m_spawnTime = std::chrono::steady_clock::now();

    pid_t pid = fork();
    if (pid < 0) {
      kLog.error("fork failed for xwayland-satellite");
      return;
    }
    if (pid == 0) {
      resetChildSignalState();
      restoreFileDescriptorLimit();
      setenv("WAYLAND_DISPLAY", m_waylandSocket.c_str(), 1);
      unsetenv("WAYLAND_SOCKET");
      // Satellite provides DISPLAY; it must not consume one.
      unsetenv("DISPLAY");
      for (const auto& [name, value] : m_environment) {
        setenv(name.c_str(), value.c_str(), 1);
      }
      execl(m_executable.c_str(), "xwayland-satellite", m_display.c_str(), nullptr);
      _exit(kExecFailedStatus);
    }

    m_pid = pid;
    m_pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (m_pidfd < 0) {
      kLog.error("pidfd_open failed for xwayland-satellite; crash respawn disabled");
    } else {
      m_exitSource = wl_event_loop_add_fd(m_loop, m_pidfd, WL_EVENT_READABLE, onPidfd, this);
    }
    kLog.info("xwayland-satellite spawned (pid {}) on DISPLAY={}", pid, m_display);
  }

  int XwaylandSupervisor::onPidfd(int /*fd*/, uint32_t /*mask*/, void* data) {
    static_cast<XwaylandSupervisor*>(data)->handleExit();
    return 0;
  }

  int XwaylandSupervisor::onRespawnTimer(void* data) {
    static_cast<XwaylandSupervisor*>(data)->spawn();
    return 0;
  }

  void XwaylandSupervisor::closeWatch() {
    if (m_exitSource != nullptr) {
      wl_event_source_remove(m_exitSource);
      m_exitSource = nullptr;
    }
    if (m_pidfd >= 0) {
      close(m_pidfd);
      m_pidfd = -1;
    }
  }

  void XwaylandSupervisor::handleExit() {
    int exitStatus = -1;
    if (m_pidfd >= 0) {
      siginfo_t info{};
      if (waitid(P_PIDFD, static_cast<id_t>(m_pidfd), &info, WEXITED | WNOHANG) == 0 && info.si_code == CLD_EXITED) {
        exitStatus = info.si_status;
      }
    }

    // No waitpid needed: SIGCHLD is SIG_IGN, so the kernel reaps for us.
    closeWatch();
    m_pid = -1;

    if (exitStatus == kExecFailedStatus) {
      kLog.error("xwayland-satellite not found or failed to exec; not respawning");
      m_display.clear();
      unsetenv("DISPLAY");
      return;
    }

    if (std::chrono::steady_clock::now() - m_spawnTime > kHealthyRuntime) {
      m_failures = 0;
    }
    ++m_failures;

    if (m_failures > kMaxFailures) {
      kLog.error(
          "xwayland-satellite keeps exiting; giving up "
          "(is it installed and is Xwayland >= 23.1 present?)"
      );
      m_display.clear();
      unsetenv("DISPLAY");
      return;
    }

    kLog.warn("xwayland-satellite exited; respawning in 1 s");
    if (m_respawnTimer == nullptr) {
      m_respawnTimer = wl_event_loop_add_timer(m_loop, onRespawnTimer, this);
    }
    wl_event_source_timer_update(m_respawnTimer, kRespawnDelayMs);
  }

  void XwaylandSupervisor::stop() {
    closeWatch();
    if (m_respawnTimer != nullptr) {
      wl_event_source_remove(m_respawnTimer);
      m_respawnTimer = nullptr;
    }
    if (m_pid > 0) {
      kill(m_pid, SIGTERM);
      m_pid = -1;
    }
  }

} // namespace umbriel
