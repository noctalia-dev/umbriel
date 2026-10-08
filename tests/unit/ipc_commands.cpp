#include "server/ipc_commands.h"

#include "check.h"

#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

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
  nlohmann::json colorPayload(nlohmann::json outputOverrides) {
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

UMBRIEL_TEST(parsesOutputCreateNameOnly) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(umbriel::parseOutputCreateArg("stream", name, mode, error));
  CHECK_EQ(name, "stream");
  CHECK(!mode.has_value());
  CHECK(error.empty());
}

UMBRIEL_TEST(parsesOutputCreateMode) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(umbriel::parseOutputCreateArg("  stream   2560x1440@120  ", name, mode, error));
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
  CHECK(umbriel::parseOutputCreateArg("stream 400x300", name, mode, error));
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
  CHECK(!umbriel::parseOutputCreateArg("stream 1920x1080p", name, mode, error));
  CHECK(!error.empty());
  CHECK(!mode.has_value());
}

UMBRIEL_TEST(rejectsOutputCreateExtraArguments) {
  std::string name;
  std::optional<umbriel::OutputMode> mode;
  std::string error;
  CHECK(!umbriel::parseOutputCreateArg("stream 1920x1080 extra", name, mode, error));
  CHECK(!error.empty());

  error.clear();
  CHECK(!umbriel::parseOutputCreateArg("   ", name, mode, error));
  CHECK(!error.empty());
}

UMBRIEL_TEST(effectsCommandReportsStatesSuppressionAndDeclarationOrder) {
  const auto* spec = umbriel::findIpcCommand("effects");
  CHECK(spec != nullptr && spec->printHuman != nullptr && spec->handle == &umbriel::IpcCommands::effects);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }
  CHECK(!spec->takesArg);
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

UMBRIEL_TEST(audioRequestsAcceptOnlyTheCompleteBoundedLevelContract) {
  using umbriel::IpcCommands;
  nlohmann::json request = {{"cmd", "effect-audio"}, {"version", 1}, {"level", 0}};
  CHECK_EQ(IpcCommands::parseAudioLevel(request), std::optional<float>(0.0F));
  request["level"] = 1;
  CHECK_EQ(IpcCommands::parseAudioLevel(request), std::optional<float>(1.0F));
  request["level"] = 0.25;
  CHECK_EQ(IpcCommands::parseAudioLevel(request), std::optional<float>(0.25F));
  for (const auto& level :
       {nlohmann::json(-0.01), nlohmann::json(1.01), nlohmann::json(true), nlohmann::json("0.5"),
        nlohmann::json(nullptr), nlohmann::json(std::numeric_limits<double>::infinity()),
        nlohmann::json(std::numeric_limits<double>::quiet_NaN())}) {
    request["level"] = level;
    CHECK(!IpcCommands::parseAudioLevel(request));
  }
  request["level"] = 0;
  for (const auto& version : {nlohmann::json(0), nlohmann::json(2), nlohmann::json(1.0), nlohmann::json(true)}) {
    request["version"] = version;
    CHECK(!IpcCommands::parseAudioLevel(request));
  }
  request["version"] = 1;
  request["source"] = "extra";
  CHECK(!IpcCommands::parseAudioLevel(request));
  request.erase("source");
  request.erase("level");
  CHECK(!IpcCommands::parseAudioLevel(request));
  CHECK(!IpcCommands::parseAudioLevel(nlohmann::json::array()));
  CHECK(!IpcCommands::parseAudioLevel(nullptr));
}

int main() { return RUN_TESTS(); }
