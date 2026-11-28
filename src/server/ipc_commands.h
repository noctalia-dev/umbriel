#pragma once

#include "config/value_parse.h"
#include "scene/effect_audio.h"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace umbriel {

  class Server;
  struct ColorDescription;

  // Surface transfer label for reports: a named protocol value keeps its name,
  // a description without a curve reports "unspecified", and no description at
  // all reports "none".
  std::string surfaceTransferLabel(const ColorDescription* description);

  // A request's `args` array, one string per CLI word.
  using IpcArgs = std::span<const std::string>;

  struct IpcCommands {
    static nlohmann::json keyboardLayouts(Server& server, IpcArgs args);
    static nlohmann::json inputCapturePermissions(Server& server, IpcArgs args);
    static nlohmann::json inputCaptureRevoke(Server& server, IpcArgs args);
    static nlohmann::json globalShortcuts(Server& server, IpcArgs args);
    static nlohmann::json globalShortcutsRevoke(Server& server, IpcArgs args);
    static std::optional<EffectAudio> parseAudioSpectrum(const nlohmann::json& request);
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
    static nlohmann::json configSet(Server& server, IpcArgs args);
    static nlohmann::json configGet(Server& server, IpcArgs args);
    static nlohmann::json configUnset(Server& server, IpcArgs args);
#ifdef UMBRIEL_HARNESS
    // The reply to a settle request. The IPC server holds the request until the compositor is settled.
    static nlohmann::json settle(Server& server, IpcArgs args);
    static nlohmann::json clockFreeze(Server& server, IpcArgs args);
    // The reply to clock-advance. The IPC server advances the clock and holds the reply until every output has drawn.
    static nlohmann::json clockAdvance(Server& server, IpcArgs args);
    static nlohmann::json clockResume(Server& server, IpcArgs args);
    static nlohmann::json rendererRecover(Server& server, IpcArgs args);
    static nlohmann::json swipeForTest(Server& server, IpcArgs args);
    static nlohmann::json animationCaptureFail(Server& server, IpcArgs args);
    static nlohmann::json blurCompositeSplit(Server& server, IpcArgs args);
    static nlohmann::json eventPacingHold(Server& server, IpcArgs args);
    static nlohmann::json planeCursor(Server& server, IpcArgs args);
    static nlohmann::json effectFrames(Server& server, IpcArgs args);
    static nlohmann::json cursorState(Server& server, IpcArgs args);
#endif
  };

  // The `umbriel --help` section that lists a command.
  enum class IpcCommandGroup {
    Control,
    // Spelled `umbriel config <verb>` on the command line; the request names it `config-<verb>`.
    Config,
    Inspect,
    Harness,
  };

  // What shell completion offers for a command's argument words.
  struct IpcArgCompletion {
    enum class Kind : uint8_t {
      None,
      Path,   // the shell completes a filesystem path
      Output, // an output name, live or configured
      Choice, // one of `choices`
    };

    constexpr IpcArgCompletion() = default;
    constexpr explicit IpcArgCompletion(
        Kind kind, std::span<const std::string_view> choices = {}, std::string_view description = {}
    )
        : m_kind(kind), m_choices(choices), m_description(description) {}

    Kind kind() const { return m_kind; }
    std::span<const std::string_view> choices() const { return m_choices; }
    std::string_view description() const { return m_description; }

  private:
    Kind m_kind = Kind::None;
    std::span<const std::string_view> m_choices;
    std::string_view m_description;
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
    // What shell completion offers for the command's argument words; `None` offers only the JSON flags.
    IpcArgCompletion completion{IpcArgCompletion::Kind::None};
  };

  std::span<const IpcCommandSpec> ipcCommands();
  const IpcCommandSpec* findIpcCommand(std::string_view name);
  // The Config-group command a `config <sub>` word names, or nullptr for anything else.
  const IpcCommandSpec* findConfigCommand(std::string_view sub);
  // The Config group's request names carry this prefix and the CLI spells them as two words; `cliName` splits on it.
  inline constexpr std::string_view kConfigCommandPrefix = "config-";
  // `config unset`'s argument that drops every override.
  inline constexpr std::string_view kUnsetAllWord = "--all";
  // How the command line spells a command: `config set` for `config-set`, the request name otherwise.
  std::string cliName(const IpcCommandSpec& spec);

  // Reads output-create's arguments: the output name and its optional mode, in the config `mode` format
  // ("WIDTHxHEIGHT[@HZ]"). Returns false with `error` set on anything else.
  bool parseOutputCreateArgs(IpcArgs args, std::string& name, std::optional<OutputMode>& mode, std::string& error);

} // namespace umbriel
