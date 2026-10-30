#pragma once

#include "config/value_parse.h"
#include "scene/effect_audio.h"

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace umbriel {

  class Server;
  struct ColorDescription;

  // Surface transfer label for reports: named protocol values keep their name,
  // fitted powers report the exponent ("power <x>"), a description without a
  // curve reports "unspecified", and no description at all reports "none".
  std::string surfaceTransferLabel(const ColorDescription* description);

  // A request's `args` array, one string per CLI word.
  using IpcArgs = std::span<const std::string>;

  struct IpcCommands {
    static nlohmann::json keyboardLayouts(Server& server, IpcArgs args);
    static nlohmann::json inputCapturePermissions(Server& server, IpcArgs args);
    static nlohmann::json inputCaptureRevoke(Server& server, IpcArgs args);
    [[nodiscard]] static std::optional<EffectAudio> parseAudioSpectrum(const nlohmann::json& request);
    static nlohmann::json effects(Server& server, IpcArgs args);
    static nlohmann::json windows(Server& server, IpcArgs args);
    static nlohmann::json focusedWindow(Server& server, IpcArgs args);
    static nlohmann::json workspaces(Server& server, IpcArgs args);
    static nlohmann::json submap(Server& server, IpcArgs args);
    static nlohmann::json layers(Server& server, IpcArgs args);
    static nlohmann::json outputs(Server& server, IpcArgs args);
    static nlohmann::json color(Server& server, IpcArgs args);
    static nlohmann::json tearing(Server& server, IpcArgs args);
    // Runs a structured action request ({"cmd":"action", ...}); see server/action_request.h.
    static nlohmann::json action(Server& server, const nlohmann::json& request);
    static nlohmann::json outputCreate(Server& server, IpcArgs args);
    static nlohmann::json outputDestroy(Server& server, IpcArgs args);
    static nlohmann::json configReplace(Server& server, IpcArgs args);
    // The reply to a settle request. The IPC server holds the request until the compositor is settled.
    static nlohmann::json settle(Server& server, IpcArgs args);
    static nlohmann::json clockFreeze(Server& server, IpcArgs args);
    // The reply to clock-advance. The IPC server advances the clock and holds the reply until every output has drawn.
    static nlohmann::json clockAdvance(Server& server, IpcArgs args);
    static nlohmann::json clockResume(Server& server, IpcArgs args);
#ifdef UMBRIEL_HARNESS
    static nlohmann::json rendererRecover(Server& server, IpcArgs args);
    static nlohmann::json swipeForTest(Server& server, IpcArgs args);
    static nlohmann::json animationCaptureFail(Server& server, IpcArgs args);
    static nlohmann::json blurCompositeSplit(Server& server, IpcArgs args);
    static nlohmann::json planeCursor(Server& server, IpcArgs args);
    static nlohmann::json effectFrames(Server& server, IpcArgs args);
    static nlohmann::json cursorState(Server& server, IpcArgs args);
#endif
  };

  // The `umbriel --help` section that lists a command.
  enum class IpcCommandGroup {
    Control,
    Inspect,
    Harness,
  };

  struct IpcCommandSpec {
    std::string_view name;
    std::string_view argSpec;
    std::string_view description;
    IpcCommandGroup group;
    bool takesArgs;
    nlohmann::json (*handle)(Server& server, IpcArgs args);
    void (*printHuman)(const nlohmann::json& ok);
    // How long the CLI waits for the reply.
    int replyTimeoutSec = 2;
  };

  std::span<const IpcCommandSpec> ipcCommands();
  const IpcCommandSpec* findIpcCommand(std::string_view name);

  // Reads output-create's arguments: the output name and its optional mode, in the config `mode` format
  // ("WIDTHxHEIGHT[@HZ]"). Returns false with `error` set on anything else.
  bool parseOutputCreateArgs(IpcArgs args, std::string& name, std::optional<OutputMode>& mode, std::string& error);

} // namespace umbriel
