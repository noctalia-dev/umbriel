#pragma once

#include "audio/state.h"

#include <functional>
#include <string>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace umbriel::audio {

  // One demanded source. The service must enforce kMaxDemandedSources before
  // constructing/acquiring supervisors. No descriptors or timers until demand.
  // Child exit observation never depends on waitpid or a waitable SIGCHLD.
  class Supervisor {
  public:
    Supervisor(
        wl_event_loop* loop, Configuration configuration, std::string executable, std::vector<std::string> arguments
    );
    ~Supervisor();
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    void setDemand(bool demand);
    void setSessionActive(bool active);
    void reset();
    [[nodiscard]] const Receiver& receiver() const { return m_receiver; }
    [[nodiscard]] bool running() const { return m_pidfd >= 0; }
    [[nodiscard]] std::string_view state() const;
    [[nodiscard]] bool demanded() const { return m_demand; }
    // Called only when published shader input changes; silence/heartbeats don't
    // cause effect-only frames. Consumer scheduling stays outside supervision.
    std::function<void()> changed;
    // Notification only: callers must defer destruction until the callback returns.
    std::function<void()> stopped;

  private:
    enum class StopPhase { None, Eof, Term, Kill };
    static int onChannel(int fd, uint32_t mask, void* data);
    static int onExit(int fd, uint32_t mask, void* data);
    static int onTimer(void* data);
    void launch();
    void stop(bool failure);
    void closeChannel();
    void closeWatch();
    void exited();
    void tick();
    void arm();
    void publishChange(uint64_t before);
    wl_event_loop* m_loop;
    Configuration m_configuration;
    std::string m_executable;
    std::vector<std::string> m_arguments;
    Receiver m_receiver;
    RetryPolicy m_retry;
    bool m_demand = false;
    bool m_active = true;
    bool m_failed = false;
    uint64_t m_epoch = 0;
    uint64_t m_stopNs = 0;
    uint64_t m_timerDeadlineNs = 0;
    StopPhase m_stopPhase = StopPhase::None;
    int m_channel = -1;
    int m_pidfd = -1;
    wl_event_source* m_channelSource = nullptr;
    wl_event_source* m_exitSource = nullptr;
    wl_event_source* m_timer = nullptr;
  };

} // namespace umbriel::audio
