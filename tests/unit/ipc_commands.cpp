#include "server/ipc_commands.h"

#include "check.h"

#include <cstdio>
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

int main() { return RUN_TESTS(); }
