#include "server/ipc_commands.h"

#include "check.h"
#include "config/keybind_parse.h"
#include "server/action_request.h"
#include "server/color_description.h"

#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

  std::string captureHumanOutput(const umbriel::IpcCommandSpec& spec, const nlohmann::json& ok) {
    FILE* output = std::tmpfile();
    CHECK(output != nullptr);
    if (output == nullptr) {
      return {};
    }

    std::fflush(stdout);
    const int savedStdout = dup(STDOUT_FILENO);
    CHECK(savedStdout >= 0);
    CHECK(dup2(fileno(output), STDOUT_FILENO) >= 0);

    spec.printHuman(ok);
    std::fflush(stdout);

    CHECK(dup2(savedStdout, STDOUT_FILENO) >= 0);
    close(savedStdout);

    CHECK(std::fseek(output, 0, SEEK_END) == 0);
    const long size = std::ftell(output);
    CHECK(size >= 0);
    CHECK(std::fseek(output, 0, SEEK_SET) == 0);

    std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    CHECK(std::fread(text.data(), 1, text.size(), output) == text.size());
    std::fclose(output);
    return text;
  }

  // Builds a minimal color IPC payload for a single output.
  nlohmann::json colorPayload(const nlohmann::json& outputOverrides) {
    nlohmann::json output = {
        {"name", "HEADLESS-1"},
        {"enabled", true},
        {"hdr_mode", "off"},
        {"hdr_requested", false},
        {"hdr_active", false},
        {"fallback_reason", ""},
        {"render_format", "XR24"},
        {"transfer_function", "none"},
        {"primaries", "none"},
        {"sdr_white", 203.0},
        {"sdr_transfer", "gamma22"},
        {"color_profile", "none"},
        {"sdr_gamut", "native"},
        {"sdr_gamut_staged", false},
        {"panel_primaries", nullptr},
        {"panel_transfer", nullptr},
        {"bit_depth", 8},
        {"bit_depth_active", false},
        {"bit_depth_fallback_reason", ""},
        {"supported_transfer_functions", nlohmann::json::array()},
        {"supported_primaries", nlohmann::json::array()},
        {"mastering_display_primaries", nullptr},
        {"mastering_luminance", nullptr},
        {"max_cll", 0.0},
        {"max_fall", 0.0},
    };
    output.merge_patch(outputOverrides);
    return {
        {"color_manager", false},
        {"renderer",
         {
             {"input_color_transform", false},
             {"output_color_transform", false},
             {"timeline", false},
         }},
        {"outputs", nlohmann::json::array({output})},
        {"surfaces", nlohmann::json::array()},
    };
  }

} // namespace

UMBRIEL_TEST(keyboardLayoutsHumanOutputListsAndMarksCurrentLayout) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("keyboard-layouts");
  CHECK(spec != nullptr);
  if (spec == nullptr) {
    return;
  }

  CHECK(spec->printHuman != nullptr);
  if (spec->printHuman == nullptr) {
    return;
  }

  const nlohmann::json layouts = {
      {"names", {"English (US)", "German"}},
      {"current_index", 1},
  };
  CHECK_EQ(captureHumanOutput(*spec, layouts), "  English (US)\n* German\n");
}

UMBRIEL_TEST(colorHumanDisabledOutputHasNoActiveDepth) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out =
      captureHumanOutput(*spec, colorPayload({{"enabled", false}, {"bit_depth", 10}, {"render_format", "XR30"}}));
  CHECK(!out.contains("10-bit SDR"));
  CHECK(out.contains("bit depth: none (configured 10)"));
}

UMBRIEL_TEST(colorHumanSdr10LineUsesCommittedFormatForDepth) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"bit_depth", 10},
          {"bit_depth_active", true},
          {"render_format", "XR30"},
      })
  );
  CHECK(out.contains("10-bit SDR: active"));
  CHECK(out.contains("bit depth: 10 (configured 10)"));
}

UMBRIEL_TEST(colorHumanSdr10FallbackShowsReason) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"bit_depth", 10},
          {"bit_depth_active", false},
          {"bit_depth_fallback_reason", "backend rejected all 10-bit SDR render formats"},
      })
  );
  CHECK(out.contains("10-bit SDR unavailable: backend rejected all 10-bit SDR render formats"));
  CHECK(out.contains("bit depth: 8 (configured 10)"));
}

UMBRIEL_TEST(colorHumanDepthIgnoresActivityFlags) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec, colorPayload({{"hdr_active", true}, {"bit_depth_active", true}, {"render_format", "XR24"}})
  );
  CHECK(out.contains("bit depth: 8 (configured 8)"));
}

UMBRIEL_TEST(colorHumanShowsCommittedHdrOutputMetadata) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"hdr_active", true},
          {"mastering_luminance", {{"min", 0.005}, {"max", 400.0}}},
          {"max_cll", 400.0},
          {"max_fall", 200.0},
      })
  );
  CHECK(out.contains("output metadata: 0.005 to 400 cd/m2; MaxCLL: 400; MaxFALL: 200"));
}

UMBRIEL_TEST(colorHumanReportsCharacterizationFields) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string inert = captureHumanOutput(*spec, colorPayload(nlohmann::json::object()));
  CHECK(inert.contains("color profile: none, SDR gamut: native"));
  CHECK(inert.contains("panel primaries: none; panel transfer: none"));

  const std::string characterized = captureHumanOutput(
      *spec,
      colorPayload({
          {"color_profile", "edid"},
          {"sdr_gamut", "srgb"},
          {"panel_primaries",
           {
               {"red", {{"x", 0.68}, {"y", 0.32}}},
               {"green", {{"x", 0.265}, {"y", 0.69}}},
               {"blue", {{"x", 0.15}, {"y", 0.06}}},
               {"white", {{"x", 0.3127}, {"y", 0.329}}},
           }},
          {"panel_transfer", "gamma22"},
      })
  );
  CHECK(characterized.contains("color profile: edid, SDR gamut: srgb"));
  CHECK(characterized.contains(
      "panel primaries: r 0.6800,0.3200 g 0.2650,0.6900 b 0.1500,0.0600 w 0.3127,0.3290; panel transfer: gamma22"
  ));
}

UMBRIEL_TEST(colorHumanLabelsFittedPowerTransferForSurfaces) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  // A client-ICC description with a power-fit TRC has no named transfer
  // function; the report shows the fitted exponent instead of "none".
  nlohmann::json payload = colorPayload(nlohmann::json::object());
  payload["surfaces"] = nlohmann::json::array({
      {
          {"id", ""},
          {"app_id", "color-feedback-client"},
          {"title", "color-feedback-client"},
          {"transfer_function", "power 1.8"},
          {"primaries", "custom"},
          {"max_cll", 0},
          {"max_fall", 0},
          {"mastering_display_primaries", nullptr},
          {"mastering_luminance", nullptr},
      },
  });
  const std::string out = captureHumanOutput(*spec, payload);
  CHECK(out.contains("power 1.8"));
  CHECK(!out.contains("unspecified"));
}

UMBRIEL_TEST(surfaceTransferLabelSeparatesNamedPowerAndBare) {
  CHECK(umbriel::surfaceTransferLabel(nullptr) == "none");
  umbriel::ColorDescription named{};
  named.data.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
  CHECK(umbriel::surfaceTransferLabel(&named) == "gamma 2.2");
  umbriel::ColorDescription powered{};
  powered.tfPower = 1.8F;
  CHECK(umbriel::surfaceTransferLabel(&powered) == "power 1.8");
  umbriel::ColorDescription bare{};
  CHECK(umbriel::surfaceTransferLabel(&bare) == "unspecified");
}

namespace {

  bool parseOutputCreate(
      std::vector<std::string> args, std::string& name, std::optional<umbriel::OutputMode>& mode, std::string& error
  ) {
    return umbriel::parseOutputCreateArgs(args, name, mode, error);
  }

} // namespace

UMBRIEL_TEST(parsesOutputCreateNameOnly) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(parseOutputCreate({"stream"}, name, mode, error));
  CHECK_EQ(name, "stream");
  CHECK(!mode.has_value());
  CHECK(error.empty());
}

UMBRIEL_TEST(parsesOutputCreateMode) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(parseOutputCreate({"stream", "2560x1440@120"}, name, mode, error));
  CHECK_EQ(name, "stream");
  CHECK(mode.has_value());
  if (mode.has_value()) {
    CHECK_EQ(mode->width, 2560);
    CHECK_EQ(mode->height, 1440);
    CHECK_EQ(mode->refreshMHz, 120000);
  }
  CHECK(error.empty());
}

UMBRIEL_TEST(parsesOutputCreateModeWithoutRefresh) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(parseOutputCreate({"stream", "400x300"}, name, mode, error));
  CHECK(mode.has_value());
  if (mode.has_value()) {
    CHECK_EQ(mode->width, 400);
    CHECK_EQ(mode->height, 300);
    CHECK_EQ(mode->refreshMHz, 0);
  }
}

UMBRIEL_TEST(rejectsOutputCreateInvalidMode) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(!parseOutputCreate({"stream", "1920x1080p"}, name, mode, error));
  CHECK(!error.empty());
  CHECK(!mode.has_value());
}

UMBRIEL_TEST(rejectsOutputCreateExtraArguments) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(!parseOutputCreate({"stream", "1920x1080", "extra"}, name, mode, error));
  CHECK(!error.empty());

  error.clear();
  CHECK(!parseOutputCreate({}, name, mode, error));
  CHECK(!error.empty());

  error.clear();
  CHECK(!parseOutputCreate({""}, name, mode, error));
  CHECK(!error.empty());
}

UMBRIEL_TEST(effectsCommandReportsStatesSuppressionAndDeclarationOrder) {
  const auto* spec = umbriel::findIpcCommand("effects");
  CHECK(spec != nullptr && spec->printHuman != nullptr && spec->handle == &umbriel::IpcCommands::effects);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }
  CHECK(!spec->takesArgs);
  const nlohmann::json slot = {
      {"name", "zebra"},
      {"pool", "zpool"},
      {"source", "runtime"},
      {"suppressed", true},
  };
  const nlohmann::json payload = {
      {"presets",
       {{{"name", "zebra"}, {"kind", "border"}, {"state", "compiled"}, {"overlay", "glow"}},
        {{"name", "alpha"}, {"kind", "window"}, {"state", "failed"}},
        {{"name", "blank"}, {"kind", "cursor"}, {"state", "inert"}},
        {{"name", "spare"}, {"kind", "screen"}, {"state", "unreferenced"}}}},
      {"pools",
       {{{"name", "zpool"},
         {"kind", "border"},
         {"policy", "unused_first"},
         {"members", {{{"name", "zebra"}, {"held", 2}}, {{"name", "alpha"}, {"held", 0}}}}},
        {{"name", "apool"}, {"kind", "window"}, {"policy", "random"}, {"members", nlohmann::json::array()}}}},
      {"cursor", slot},
      {"owners",
       {{{"type", "window"}, {"id", "window-1"}, {"app_id", "terminal"}, {"slots", {{"border", slot}}}},
        {{"type", "output"}, {"name", "vendor/panel"}, {"slots", {{"screen", slot}}}}}},
  };
  const std::string output = captureHumanOutput(*spec, payload);
  CHECK(output.find("zebra\tborder\tcompiled\tglow") < output.find("alpha\twindow\tfailed"));
  CHECK(output.contains("blank\tcursor\tinert"));
  CHECK(output.contains("spare\tscreen\tunreferenced"));
  CHECK(output.find("zpool\tborder\tunused_first") < output.find("apool\twindow\trandom"));
  CHECK(output.contains("zebra (2), alpha (0)"));
  CHECK(output.contains("cursor\tcursor\tzebra\tzpool\truntime\tyes"));
  CHECK(output.contains("window window-1 (terminal)\tborder"));
  CHECK(output.contains("output vendor/panel\tscreen"));
  CHECK_EQ(captureHumanOutput(*spec, payload), output);
}

UMBRIEL_TEST(audioRequestsAcceptOnlyTheCompleteBoundedSpectrumContract) {
  using umbriel::IpcCommands;
  auto bands = nlohmann::json::array();
  for (int i = 0; i < FX_AUDIO_BANDS; ++i) {
    bands.push_back(static_cast<double>(i) / (FX_AUDIO_BANDS - 1));
  }
  nlohmann::json request = {{"cmd", "effect-audio"}, {"version", 2}, {"bands", bands}};
  const auto audio = IpcCommands::parseAudioSpectrum(request);
  CHECK(audio.has_value());
  for (int i = 0; i < FX_AUDIO_BANDS; ++i) {
    CHECK_EQ((*audio)[i], static_cast<float>(i) / (FX_AUDIO_BANDS - 1));
  }
  CHECK_EQ((*audio)[FX_AUDIO_BANDS], 1.0F);
  for (const auto& value :
       {nlohmann::json(-0.01), nlohmann::json(1.01), nlohmann::json(true), nlohmann::json("0.5"),
        nlohmann::json(nullptr), nlohmann::json(std::numeric_limits<double>::infinity()),
        nlohmann::json(std::numeric_limits<double>::quiet_NaN())}) {
    request["bands"][63] = value;
    CHECK(!IpcCommands::parseAudioSpectrum(request));
  }
  request["bands"] = bands;
  for (const auto& version : {nlohmann::json(0), nlohmann::json(1), nlohmann::json(2.0), nlohmann::json(true)}) {
    request["version"] = version;
    CHECK(!IpcCommands::parseAudioSpectrum(request));
  }
  request["version"] = 2;
  request["level"] = 0.5;
  CHECK(!IpcCommands::parseAudioSpectrum(request));
  request.erase("level");
  request["bands"].erase(63);
  CHECK(!IpcCommands::parseAudioSpectrum(request));
  request["bands"] = bands;
  request["bands"].push_back(0);
  CHECK(!IpcCommands::parseAudioSpectrum(request));
  request["bands"] = 0;
  CHECK(!IpcCommands::parseAudioSpectrum(request));
  request.erase("bands");
  CHECK(!IpcCommands::parseAudioSpectrum(request));
  CHECK(!IpcCommands::parseAudioSpectrum(nlohmann::json::array()));
  CHECK(!IpcCommands::parseAudioSpectrum(nullptr));
}

namespace {

  // Word-grammar samples for every argument kind, with optional fields both set and left out.
  std::vector<std::string> actionSamples(umbriel::ActionArgKind kind) {
    using umbriel::ActionArgKind;
    switch (kind) {
    case ActionArgKind::None:
      return {""};
    case ActionArgKind::Command:
      return {" grim -g \"$(slurp)\" out.png"};
    case ActionArgKind::Submap:
      return {" 'my map'"};
    case ActionArgKind::Fraction:
      return {" 0.5"};
    case ActionArgKind::FractionDelta:
      return {" -0.25"};
    case ActionArgKind::WorkspaceMoveToOutput:
      return {
          " DP-2", " current 3 --output DP-1", " --output DP-1 current --name 2", " 'Vendor Model Serial' 'My web'"
      };
    case ActionArgKind::Workspace:
      return {" 3", " --name 2 --output DP-1", " web"};
    case ActionArgKind::WindowEffectSet:
      return {" off", " glow --window w1"};
    case ActionArgKind::WindowEffectCycle:
      return {"", " --window w1", " pool --window w1"};
    case ActionArgKind::ScreenEffectSet:
      return {" cinema --output vendor/panel"};
    case ActionArgKind::ScreenEffectCycle:
      return {"", " screens --output DP-1"};
    case ActionArgKind::CursorEffectSet:
      return {" halo"};
    case ActionArgKind::CursorEffectCycle:
      return {"", " cursors"};
    case ActionArgKind::Output:
      return {" DP-1"};
    case ActionArgKind::OptionalOutput:
      return {"", " DP-1"};
    case ActionArgKind::OptionalScratchpad:
      return {"", " term"};
    case ActionArgKind::WindowId:
      return {" w1"};
    case ActionArgKind::OptionalWindowId:
      return {"", " w1"};
    case ActionArgKind::WindowRegion:
      return {" bottom_left"};
    case ActionArgKind::ColumnDisplay:
      return {" tabbed"};
    case ActionArgKind::TabIndex:
      return {" -1"};
    case ActionArgKind::SkipConfirmation:
      return {"", " skip-confirmation"};
    case ActionArgKind::SdrWhite:
      return {" 200", " 300 --output DP-1"};
    case ActionArgKind::LayoutMode:
      return {" master", " toggle", " toggle scrolling master"};
    case ActionArgKind::WindowMoveTo:
      return {" top_left -10 20", " center 1 1 --window w1"};
    case ActionArgKind::WindowMoveBy:
      return {" 5 -5 --window w1"};
    }
    return {};
  }

  std::optional<umbriel::Keybind> decode(std::string_view text, std::string& error) {
    return umbriel::actionFromRequest(nlohmann::json::parse(text), error);
  }

} // namespace

// The CLI parses words and sends the structured request, so every payload the word grammar produces must survive the
// trip through the request unchanged.
UMBRIEL_TEST(everyWordFormRoundTripsThroughTheActionRequest) {
  for (const auto& spec : umbriel::actionSpecs()) {
    for (const auto& sample : actionSamples(spec.argKind)) {
      umbriel::Keybind words;
      CHECK(umbriel::parseAction(std::string(spec.name) + sample, words));
      std::string error;
      const auto decoded = umbriel::actionFromRequest(umbriel::actionRequest(words), error);
      CHECK(decoded.has_value());
      CHECK(error.empty());
      if (decoded) {
        CHECK(decoded->action == words.action);
        CHECK(decoded->payload == words.payload);
      }
    }
  }
}

UMBRIEL_TEST(actionRequestWorkspaceTypeSelectsPositionOrName) {
  std::string error;
  const auto position = decode(R"({"cmd":"action","action":"workspace-switch","workspace":2})", error);
  const auto* index = position
      ? std::get_if<umbriel::WorkspaceIndex>(&std::get<umbriel::WorkspaceArg>(position->payload).reference)
      : nullptr;
  CHECK(index != nullptr && index->value == 2);
  const auto named = decode(R"({"cmd":"action","action":"workspace-switch","workspace":"2","output":"DP-1"})", error);
  CHECK(named.has_value());
  if (named) {
    const auto& workspace = std::get<umbriel::WorkspaceArg>(named->payload);
    const auto* name = std::get_if<umbriel::WorkspaceName>(&workspace.reference);
    CHECK(name != nullptr && name->value == "2");
    CHECK_EQ(workspace.output, std::string{"DP-1"});
  }
}

UMBRIEL_TEST(actionRequestsRejectMalformedFieldsWithTheReason) {
  const std::pair<std::string_view, std::string_view> cases[] = {
      {R"({"cmd":"action"})", R"(missing "action")"},
      {R"({"cmd":"action","action":"nope"})", "unknown action: nope"},
      {R"({"cmd":"action","action":"window-close","id":"w1"})", R"(unknown field "id")"},
      {R"({"cmd":"action","action":"window-focus"})", R"(missing "window")"},
      {R"({"cmd":"action","action":"window-focus","window":""})", R"("window" must be a non-empty string)"},
      {R"({"cmd":"action","action":"window-focus","window":7})", R"("window" must be a non-empty string)"},
      {R"({"cmd":"action","action":"workspace-switch","workspace":0})", "position must be 1 to 64"},
      {R"({"cmd":"action","action":"workspace-switch","workspace":2.5})", "position number or a non-empty name"},
      {R"({"cmd":"action","action":"window-set-primary-extent","fraction":9})", R"("fraction" must be)"},
      {R"({"cmd":"action","action":"column-focus-tab","index":0})", R"("index" must not be 0)"},
      // Unsigned JSON integers past the signed range must not wrap into an accepted value.
      {R"({"cmd":"action","action":"column-focus-tab","index":18446744073709551615})", R"("index" is out of range)"},
      {R"({"cmd":"action","action":"window-move-by","dx":18446744073709551615,"dy":0})", R"("dx" is out of range)"},
      {R"({"cmd":"action","action":"workspace-switch","workspace":18446744073709551615})", "position must be 1 to 64"},
      {R"({"cmd":"action","action":"window-move-by","dx":1.5,"dy":0})", R"("dx" must be an integer)"},
      {R"({"cmd":"action","action":"workspace-set-layout","layout":"master","cycle":["master","dwindle"]})",
       R"("cycle" needs "layout": "toggle")"},
      {R"({"cmd":"action","action":"workspace-set-layout","layout":"toggle","cycle":["master","master"]})",
       "two or more distinct layouts"},
      {R"({"cmd":"action","action":"effect-window-cycle","pool":"off"})", R"("pool" cannot be off)"},
      {R"({"cmd":"action","action":"effect-cursor-set","effect":"halo","window":"w1"})", R"(unknown field "window")"},
      {R"({"cmd":"action","action":"effect-screen-set","effect":"x","window":"w1"})", R"(unknown field "window")"},
      {R"({"cmd":"action","action":"session-quit","skip_confirmation":"yes"})", "must be a boolean"},
  };
  for (const auto& [request, reason] : cases) {
    std::string error;
    CHECK(!decode(request, error).has_value());
    if (!error.contains(reason)) {
      std::println(stderr, "{} -> {}", request, error);
    }
    CHECK(error.contains(reason));
  }
}

UMBRIEL_TEST(workspaceOutputRequestRejectsMalformedFields) {
  std::string error;
  for (const auto& invalid :
       {nlohmann::json::object(), nlohmann::json{{"destination", 2}}, nlohmann::json{{"destination", ""}},
        nlohmann::json{{"destination", "current"}, {"output", "DP-1"}},
        nlohmann::json{{"destination", "current"}, {"workspace", 0}},
        nlohmann::json{{"destination", "current"}, {"workspace", true}},
        nlohmann::json{{"destination", "current"}, {"workspace", ""}},
        nlohmann::json{{"destination", "current"}, {"window", "id"}}}) {
    auto bad = invalid;
    bad["cmd"] = "action";
    bad["action"] = "workspace-move-to-output";
    CHECK(!umbriel::actionFromRequest(bad, error));
  }
}

int main() { return RUN_TESTS(); }
