#pragma once

#include <memory>

struct wl_display;
struct wlr_backend;
struct wlr_output;

namespace umbriel {

  class DrmLeaseManager {
  public:
    DrmLeaseManager(wl_display* display, wlr_backend* backend);
    ~DrmLeaseManager();

    DrmLeaseManager(const DrmLeaseManager&) = delete;
    DrmLeaseManager& operator=(const DrmLeaseManager&) = delete;

    [[nodiscard]] bool offerOutput(wlr_output* output);
    // Deactivation rejects new requests and revokes every outstanding lease.
    void setActive(bool active);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
