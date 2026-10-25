#pragma once

#include <functional>
#include <memory>
#include <sys/types.h>

struct wl_event_loop;

namespace umbriel {

  // Authenticates socket peers before bridging them into libeis' FD backend.
  // The socketpair backend is available in libei 1.3 and keeps peer authentication
  // independent of libeis' socket backend and its newer credential API.
  class EisSocket {
  public:
    using Authenticate = std::function<bool(pid_t)>;
    // Returns an owned backend endpoint, or a negative value on failure.
    using OpenClient = std::function<int()>;

    EisSocket(wl_event_loop* loop, Authenticate authenticate, OpenClient openClient);
    ~EisSocket();
    EisSocket(const EisSocket&) = delete;
    EisSocket& operator=(const EisSocket&) = delete;

    [[nodiscard]] int fd() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
