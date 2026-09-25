#pragma once
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string_view>

namespace umbriel {

  class Server;

  struct IpcCommands {
    static nlohmann::json keyboardLayouts(Server& server, std::string_view arg);
    static nlohmann::json windows(Server& server, std::string_view arg);
    static nlohmann::json workspaces(Server& server, std::string_view arg);
    static nlohmann::json submap(Server& server, std::string_view arg);
    static nlohmann::json layers(Server& server, std::string_view arg);
    static nlohmann::json color(Server& server, std::string_view arg);
    static nlohmann::json tearing(Server& server, std::string_view arg);
    static nlohmann::json msg(Server& server, std::string_view arg);
    static nlohmann::json outputCreate(Server& server, std::string_view arg);
    static nlohmann::json outputDestroy(Server& server, std::string_view arg);
    // The reply to a settle request. The IPC server holds the request until the compositor is settled.
    static nlohmann::json settle(Server& server, std::string_view arg);
    static nlohmann::json clockFreeze(Server& server, std::string_view arg);
    // The reply to clock-advance. The IPC server advances the clock and holds the reply until every output has drawn.
    static nlohmann::json clockAdvance(Server& server, std::string_view arg);
    static nlohmann::json clockResume(Server& server, std::string_view arg);
  };

  struct IpcCommandSpec {
    std::string_view name;
    std::string_view argSpec;
    std::string_view description;
    bool takesArg;
    nlohmann::json (*handle)(Server& server, std::string_view arg);
    void (*printHuman)(const nlohmann::json& ok);
    // How long the CLI waits for the reply.
    int replyTimeoutSec = 2;
  };

  std::span<const IpcCommandSpec> ipcCommands();
  const IpcCommandSpec* findIpcCommand(std::string_view name);

} // namespace umbriel
