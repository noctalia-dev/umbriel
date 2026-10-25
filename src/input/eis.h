#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <sys/types.h>

struct wlr_input_device;
struct wlr_keyboard;
struct wlr_keyboard_modifiers;

namespace umbriel {

  class Server;

  class EisInput {
  public:
    using PeerAuthentication = std::function<bool(pid_t)>;
    using AuthorizationRequest = std::function<void(uint64_t, std::string_view)>;
    using AuthorizationCancelled = std::function<void(uint64_t)>;

    explicit EisInput(Server& server);
    ~EisInput();
    EisInput(const EisInput&) = delete;
    EisInput& operator=(const EisInput&) = delete;

    [[nodiscard]] int socketFd() const;
    [[nodiscard]] bool ownsDevice(const wlr_input_device* device) const;
    void setAuthenticate(PeerAuthentication authenticate);
    void setAuthorize(AuthorizationRequest authorize);
    void setAuthorizationCancelled(AuthorizationCancelled cancelled);
    void authorizeClient(uint64_t id, bool allowed);
    void applyConfig();
    void updateOutputs();
    void updateModifiers(const wlr_keyboard_modifiers& modifiers);
    void setSessionLocked(bool locked);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void addKeyboard(wlr_input_device* device);
    void addPointer(wlr_input_device* device);
    [[nodiscard]] wlr_keyboard* sourceKeyboard() const;
  };

} // namespace umbriel
