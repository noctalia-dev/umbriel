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
        {"dpms_off", false},
        {"hdr_mode", "off"},
        {"hdr_requested", false},
        {"hdr_active", false},
        {"fallback_reason", ""},
        {"render_format", "XR24"},
        {"transfer_function", "none"},
        {"primaries", "none"},
        {"sdr_white", 203.0},
        {"bit_depth", 8},
        {"sdr10_active", false},
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

UMBRIEL_TEST(colorHumanSdr8OutputOmits10BitLine) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(*spec, colorPayload({{"bit_depth", 8}}));
  CHECK(!out.contains("10-bit SDR"));
}

UMBRIEL_TEST(colorHumanSdr10XR30ShowsActive) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"bit_depth", 10},
          {"sdr10_active", true},
          {"render_format", "XR30"},
      })
  );
  CHECK(out.contains("10-bit SDR: active"));
}

UMBRIEL_TEST(colorHumanSdr10XB30ShowsActive) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"bit_depth", 10},
          {"sdr10_active", true},
          {"render_format", "XB30"},
      })
  );
  CHECK(out.contains("10-bit SDR: active"));
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
          {"sdr10_active", false},
          {"bit_depth_fallback_reason", "backend rejected all 10-bit SDR render formats"},
      })
  );
  CHECK(out.contains("10-bit SDR unavailable: backend rejected all 10-bit SDR render formats"));
}

UMBRIEL_TEST(colorHumanHdrActiveOutput) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"hdr_mode", "on"},
          {"hdr_requested", true},
          {"hdr_active", true},
          {"render_format", "XR30"},
          {"transfer_function", "st2084_pq"},
          {"primaries", "bt2020"},
      })
  );
  // Human line: "output ...: HDR mode on, requested yes, active yes, ..."
  CHECK(out.contains("active yes"));
  CHECK(!out.contains("10-bit SDR"));
}

UMBRIEL_TEST(colorHumanHdrFallbackWithSdr10Active) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("color");
  CHECK(spec != nullptr && spec->printHuman != nullptr);
  if (spec == nullptr || spec->printHuman == nullptr) {
    return;
  }

  // HDR was requested, failed (headless: no PQ), but bit_depth=10 independently committed XR30.
  const std::string out = captureHumanOutput(
      *spec,
      colorPayload({
          {"hdr_mode", "on"},
          {"hdr_requested", true},
          {"hdr_active", false},
          {"fallback_reason", "display does not advertise PQ"},
          {"render_format", "XR30"},
          {"bit_depth", 10},
          {"sdr10_active", true},
      })
  );
  CHECK(out.contains("fallback: display does not advertise PQ"));
  CHECK(out.contains("10-bit SDR: active"));
}

UMBRIEL_TEST(colorJsonEnabledFieldPresent) {
  const nlohmann::json payload = colorPayload(nlohmann::json::object());
  CHECK(payload["outputs"][0].contains("enabled"));
  CHECK(payload["outputs"][0]["enabled"] == true);
}

UMBRIEL_TEST(colorJsonDisabledOutput) {
  const nlohmann::json payload = colorPayload({{"enabled", false}});
  CHECK(payload["outputs"][0]["enabled"] == false);
}

UMBRIEL_TEST(colorJsonDpmsOffFieldPresent) {
  const nlohmann::json payload = colorPayload({{"dpms_off", true}});
  CHECK(payload["outputs"][0].contains("dpms_off"));
  CHECK(payload["outputs"][0]["dpms_off"] == true);
}

UMBRIEL_TEST(colorJsonSdr10XR30Fields) {
  const nlohmann::json payload = colorPayload({
      {"bit_depth", 10},
      {"sdr10_active", true},
      {"render_format", "XR30"},
      {"bit_depth_fallback_reason", ""},
  });
  CHECK(payload["outputs"][0]["sdr10_active"] == true);
  CHECK(payload["outputs"][0]["render_format"] == "XR30");
  CHECK(payload["outputs"][0]["bit_depth_fallback_reason"] == "");
}

UMBRIEL_TEST(colorJsonSdr10XB30Fields) {
  const nlohmann::json payload = colorPayload({
      {"bit_depth", 10},
      {"sdr10_active", true},
      {"render_format", "XB30"},
      {"bit_depth_fallback_reason", ""},
  });
  CHECK(payload["outputs"][0]["sdr10_active"] == true);
  CHECK(payload["outputs"][0]["render_format"] == "XB30");
}

UMBRIEL_TEST(colorJsonXr24FallbackFields) {
  const nlohmann::json payload = colorPayload({
      {"bit_depth", 10},
      {"sdr10_active", false},
      {"render_format", "XR24"},
      {"bit_depth_fallback_reason", "backend rejected all 10-bit SDR render formats"},
  });
  CHECK(payload["outputs"][0]["sdr10_active"] == false);
  CHECK(payload["outputs"][0]["render_format"] == "XR24");
  CHECK(payload["outputs"][0]["bit_depth_fallback_reason"] == "backend rejected all 10-bit SDR render formats");
}

UMBRIEL_TEST(colorJsonHdrActiveFields) {
  const nlohmann::json payload = colorPayload({
      {"hdr_active", true},
      {"hdr_requested", true},
      {"render_format", "XR30"},
      {"transfer_function", "st2084_pq"},
      {"primaries", "bt2020"},
  });
  CHECK(payload["outputs"][0]["hdr_active"] == true);
  CHECK(payload["outputs"][0]["transfer_function"] == "st2084_pq");
  CHECK(payload["outputs"][0]["primaries"] == "bt2020");
}

int main() { return RUN_TESTS(); }
