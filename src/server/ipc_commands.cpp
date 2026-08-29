#include "server/ipc_commands.h"

#include "config/config.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <cctype>
#include <drm_fourcc.h>
#include <nlohmann/json.hpp>
#include <print>
#include <string>

namespace umbriel {

  namespace {
    const char* layerName(uint32_t layer) {
      switch (layer) {
      case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return "background";
      case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return "bottom";
      case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return "top";
      case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return "overlay";
      default:
        return "unknown";
      }
    }

    const char* layoutModeName(LayoutMode mode) {
      switch (mode) {
      case LayoutMode::Scrolling:
        return "scrolling";
      case LayoutMode::Dwindle:
        return "dwindle";
      case LayoutMode::Master:
        return "master";
      }
      return "unknown";
    }

    void printWindows(const nlohmann::json& ok) {
      for (const auto& entry : ok) {
        const std::string appId = entry.value("app_id", "");
        const std::string title = entry.value("title", "");
        const std::string xdgTag = entry.value("xdg_tag", "");
        const std::string xdgTagSuffix = xdgTag.empty() ? "" : " [xdg_tag=" + xdgTag + "]";
        const std::string contentType = entry.value("content_type", "none");
        const std::string contentTypeSuffix = contentType == "none" ? "" : " [content_type=" + contentType + "]";
        std::println(
            "{}{}{}\t{}\t[{} {}x{}{:+}{:+}]{}{}",
            entry.value("focused", false) ? "*" : (entry.value("urgent", false) ? "!" : " "),
            entry.value("xwayland", false) ? "[Xwayland] " : "", appId.empty() ? "-" : appId,
            title.empty() ? "-" : title, entry.value("floating", false) ? "float" : "tile", entry.value("w", 0),
            entry.value("h", 0), entry.value("x", 0), entry.value("y", 0), xdgTagSuffix, contentTypeSuffix
        );
      }
    }

    void printWorkspaces(const nlohmann::json& ok) {
      for (const auto& entry : ok) {
        const std::string output = entry.value("output", "");
        const std::string name = entry.value("name", "");
        const std::string layout = entry.value("layout", "unknown");
        std::println(
            "{} {}: {} [{}]{}", entry.value("active", false) ? "*" : " ", output.empty() ? "-" : output,
            name.empty() ? "-" : name, layout, entry.value("focused", false) ? " (focused)" : ""
        );
      }
    }

    void printSubmap(const nlohmann::json& ok) {
      if (!ok.is_string()) {
        return;
      }
      const std::string name = ok.get<std::string>();
      std::println("{}", name.empty() ? "unnamed" : name);
    }

    void printLayers(const nlohmann::json& ok) {
      for (const auto& entry : ok) {
        const std::string layer = entry.value("layer", "");
        const std::string ns = entry.value("namespace", "");
        const std::string output = entry.value("output", "");
        bool mapped = entry.value("mapped", false);
        std::println(
            "{}\t{}\t{}\t{}", layer, ns.empty() ? "-" : ns, output.empty() ? "-" : output, mapped ? "yes" : "no"
        );
      }
    }

    void printOutputName(const nlohmann::json& ok) { std::println("{}", ok.get<std::string>()); }

    std::string fourccName(uint32_t format) {
      if (format == DRM_FORMAT_INVALID) {
        return "invalid";
      }
      std::string name(4, '?');
      for (size_t i = 0; i < name.size(); ++i) {
        const auto value = static_cast<unsigned char>((format >> (i * 8)) & 0xFFU);
        name[i] = std::isprint(value) != 0 ? static_cast<char>(value) : '?';
      }
      return name;
    }

    const char* transferFunctionName(wlr_color_transfer_function value) {
      switch (value) {
      case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
        return "sRGB";
      case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
        return "PQ";
      case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
        return "extended linear";
      case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
        return "gamma 2.2";
      case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
        return "BT.1886";
      }
      return "unknown";
    }

    const char* primariesName(wlr_color_named_primaries value) {
      switch (value) {
      case WLR_COLOR_NAMED_PRIMARIES_SRGB:
        return "sRGB";
      case WLR_COLOR_NAMED_PRIMARIES_BT2020:
        return "BT.2020";
      }
      return "unknown";
    }

    const char* protocolTransferFunctionName(uint32_t value) {
      switch (value) {
      case 0:
        return "none";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
        return "BT.1886";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
        return "gamma 2.2";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
        return "gamma 2.8";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST240:
        return "ST 240";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
        return "extended linear";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_100:
        return "log 100";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_LOG_316:
        return "log 316";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_XVYCC:
        return "xvYCC";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
        return "sRGB";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_SRGB:
        return "extended sRGB";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
        return "PQ";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST428:
        return "ST 428";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG:
        return "HLG";
      case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
        return "compound power 2.4";
      default:
        return "unknown";
      }
    }

    const char* protocolPrimariesName(uint32_t value) {
      switch (value) {
      case 0:
        return "none";
      case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
        return "sRGB";
      case WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M:
        return "PAL-M";
      case WP_COLOR_MANAGER_V1_PRIMARIES_PAL:
        return "PAL";
      case WP_COLOR_MANAGER_V1_PRIMARIES_NTSC:
        return "NTSC";
      case WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM:
        return "generic film";
      case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
        return "BT.2020";
      case WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ:
        return "CIE 1931 XYZ";
      case WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3:
        return "DCI-P3";
      case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3:
        return "Display P3";
      case WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB:
        return "Adobe RGB";
      default:
        return "unknown";
      }
    }

    nlohmann::json supportedTransferFunctions(uint32_t supported) {
      nlohmann::json names = nlohmann::json::array();
      constexpr wlr_color_transfer_function values[] = {
          WLR_COLOR_TRANSFER_FUNCTION_SRGB,       WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
          WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, WLR_COLOR_TRANSFER_FUNCTION_GAMMA22,
          WLR_COLOR_TRANSFER_FUNCTION_BT1886,
      };
      for (const auto value : values) {
        if ((supported & value) != 0) {
          names.push_back(transferFunctionName(value));
        }
      }
      return names;
    }

    nlohmann::json supportedPrimaries(uint32_t supported) {
      nlohmann::json names = nlohmann::json::array();
      constexpr wlr_color_named_primaries values[] = {
          WLR_COLOR_NAMED_PRIMARIES_SRGB,
          WLR_COLOR_NAMED_PRIMARIES_BT2020,
      };
      for (const auto value : values) {
        if ((supported & value) != 0) {
          names.push_back(primariesName(value));
        }
      }
      return names;
    }

    nlohmann::json xy(const wlr_color_cie1931_xy& value) { return {{"x", value.x}, {"y", value.y}}; }

    std::string joinNames(const nlohmann::json& names) {
      std::string joined;
      for (const auto& name : names) {
        if (!joined.empty()) {
          joined += ", ";
        }
        joined += name.get<std::string>();
      }
      return joined.empty() ? "none" : joined;
    }

    void printColor(const nlohmann::json& ok) {
      const auto& renderer = ok.at("renderer");
      std::println(
          "color manager: {}, renderer input transform: {}, output transform: {}, timeline: {}",
          ok.value("color_manager", false) ? "yes" : "no",
          renderer.value("input_color_transform", false) ? "yes" : "no",
          renderer.value("output_color_transform", false) ? "yes" : "no",
          renderer.value("timeline", false) ? "yes" : "no"
      );
      for (const auto& output : ok.at("outputs")) {
        const std::string fallback = output.value("fallback_reason", "");
        std::println(
            "output {}: HDR mode {}, requested {}, active {}, format {}, {}, {}, SDR white {} cd/m2",
            output.value("name", ""), output.value("hdr_mode", "off"),
            output.value("hdr_requested", false) ? "yes" : "no", output.value("hdr_active", false) ? "yes" : "no",
            output.value("render_format", "invalid"), output.value("transfer_function", "none"),
            output.value("primaries", "none"), output.value("sdr_white", 0.0)
        );
        if (!fallback.empty()) {
          std::println("  fallback: {}", fallback);
        }
        std::println(
            "  supported transfer functions: {}; primaries: {}", joinNames(output.at("supported_transfer_functions")),
            joinNames(output.at("supported_primaries"))
        );
      }
      for (const auto& surface : ok.at("surfaces")) {
        std::println(
            "surface {}: {} ({}) {}, {}", surface.value("id", ""), surface.value("title", ""),
            surface.value("app_id", ""), surface.value("transfer_function", "none"), surface.value("primaries", "none")
        );
        if (surface.at("mastering_luminance").is_object()) {
          const auto& luminance = surface.at("mastering_luminance");
          std::println(
              "  mastering luminance: {} to {} cd/m2; MaxCLL: {}; MaxFALL: {}", luminance.value("min", 0.0),
              luminance.value("max", 0.0), surface.value("max_cll", 0), surface.value("max_fall", 0)
          );
        } else {
          std::println(
              "  mastering luminance: unset; MaxCLL: {}; MaxFALL: {}", surface.value("max_cll", 0),
              surface.value("max_fall", 0)
          );
        }
        std::println(
            "  mastering display primaries: {}", surface.at("mastering_display_primaries").is_object() ? "set" : "unset"
        );
      }
    }

    void printTearing(const nlohmann::json& ok) {
      std::println("tearing control: {}", ok.value("protocol", false) ? "yes" : "no");
      for (const auto& output : ok.at("outputs")) {
        const auto& presentationVsync = output.at("last_presentation_vsync");
        const auto& presentationPresented = output.at("last_presentation_presented");
        const std::string presentation = presentationPresented.is_boolean() && !presentationPresented.get<bool>()
            ? "not presented"
            : presentationVsync.is_boolean() ? (presentationVsync.get<bool>() ? "vsync" : "async")
                                             : "unknown";
        std::println(
            "output {}: allowed {}, requested {}, last commit {}, last presentation {}", output.value("name", ""),
            output.value("allowed", false) ? "yes" : "no", output.value("requested", false) ? "yes" : "no",
            output.value("last_commit_tearing", false) ? "async" : "regular", presentation
        );
        if (const std::string fallback = output.value("fallback_reason", ""); !fallback.empty()) {
          std::println("  fallback: {}", fallback);
        }
      }
      for (const auto& surface : ok.at("surfaces")) {
        std::string rule = "inherit";
        if (surface.at("rule_override").is_boolean()) {
          rule = surface.at("rule_override").get<bool>() ? "force" : "deny";
        }
        std::println(
            "surface {}: {} ({}) output {}, hint {}, rule {}, fullscreen {}, eligible {}", surface.value("id", ""),
            surface.value("title", ""), surface.value("app_id", ""), surface.value("output", ""),
            surface.value("hint", "vsync"), rule, surface.value("fullscreen", false) ? "yes" : "no",
            surface.value("eligible", false) ? "yes" : "no"
        );
      }
    }

  } // namespace

  nlohmann::json IpcCommands::windows(Server& server, std::string_view /*arg*/) {
    nlohmann::json windows = nlohmann::json::array();
    for (const auto& v : server.views()) {
      if (!v->mapped()) {
        continue;
      }
      nlohmann::json entry;
      entry["id"] = v->extForeignIdentifier() != nullptr ? v->extForeignIdentifier() : "";
      entry["workspace"] = v->workspace() != nullptr ? v->workspace()->id() : "";
      entry["active"] = v->activated();
      entry["app_id"] = v->toplevel()->app_id != nullptr ? v->toplevel()->app_id : "";
      entry["title"] = v->toplevel()->title != nullptr ? v->toplevel()->title : "";
      entry["xdg_tag"] = v->xdgTag();
      entry["content_type"] = contentTypeName(v->contentType());
      entry["floating"] = v->floating();
      // The compositor's own notion of focus, which is what every action acts
      // on. Lets a caller (and the harness) see where focus went.
      entry["focused"] = v->workspace() != nullptr && v->workspace()->focusedView() == v.get();
      entry["urgent"] = v->urgent();
      entry["xwayland"] = v->xwayland();
      // Tiled windows report their layout slot, which the layout computes even for hidden workspaces; floats report
      // their own position. Ordering a listing by these positions then matches the strip (scrolling) or tile tree
      // (dwindle) regardless of visibility or in-flight animations.
      if (Workspace* workspace = v->workspace(); workspace != nullptr && workspace->layout().columnOf(v.get()) >= 0) {
        const wlr_box box = workspace->layout().targetBox(v.get());
        entry["x"] = box.x;
        entry["y"] = box.y;
      } else {
        entry["x"] = v->layoutTargetX();
        entry["y"] = v->layoutTargetY();
      }
      entry["w"] = v->toplevel()->base->geometry.width;
      entry["h"] = v->toplevel()->base->geometry.height;
      windows.push_back(std::move(entry));
    }
    return nlohmann::json{{"ok", windows}};
  }

  nlohmann::json IpcCommands::workspaces(Server& server, std::string_view /*arg*/) {
    nlohmann::json workspaces = nlohmann::json::array();
    const Output* preferred = server.outputFromWlr(server.preferredOutput());
    for (const auto& output : server.outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        Workspace* workspace = group->workspaceAt(index);
        if (workspace == nullptr) {
          continue;
        }
        workspaces.push_back({
            {"id", workspace->id()},
            {"name", workspace->name()},
            {"index", workspace->index() + 1},
            {"output", output->wlr()->name},
            {"active", workspace->active()},
            {"focused", output.get() == preferred && workspace->active()},
            {"layout", layoutModeName(workspace->layoutMode())},
        });
      }
    }
    return nlohmann::json{{"ok", workspaces}};
  }

  nlohmann::json IpcCommands::submap(Server& server, std::string_view /*arg*/) {
    nlohmann::json active = nullptr;
    if (server.inSubmap()) {
      active = server.activeSubmap();
    }
    return nlohmann::json{{"ok", std::move(active)}};
  }

  nlohmann::json IpcCommands::layers(Server& server, std::string_view /*arg*/) {
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& l : server.layerSurfaces()) {
      auto* s = l->layerSurface();
      if (s == nullptr) {
        continue;
      }
      nlohmann::json entry;
      entry["layer"] = layerName(s->current.layer);
      entry["namespace"] = s->namespace_ != nullptr ? s->namespace_ : "";
      entry["output"] = s->output != nullptr ? s->output->name : "";
      entry["mapped"] = l->mapped();
      layers.push_back(std::move(entry));
    }
    return nlohmann::json{{"ok", layers}};
  }

  nlohmann::json IpcCommands::color(Server& server, std::string_view /*arg*/) {
    nlohmann::json outputs = nlohmann::json::array();
    for (const auto& output : server.outputs()) {
      const wlr_output* wlrOutput = output->wlr();
      const wlr_output_image_description* description = wlrOutput->image_description;
      outputs.push_back({
          {"name", wlrOutput->name},
          {"hdr_mode", hdrModeName(output->hdrMode())},
          {"hdr_requested", output->hdrRequested()},
          {"hdr_active", output->hdrActive()},
          {"fallback_reason", output->hdrFallbackReason()},
          {"render_format", fourccName(wlrOutput->render_format)},
          {"transfer_function", description != nullptr ? transferFunctionName(description->transfer_function) : "none"},
          {"primaries", description != nullptr ? primariesName(description->primaries) : "none"},
          {"sdr_white", output->configuredSdrWhite()},
          {"supported_transfer_functions", supportedTransferFunctions(wlrOutput->supported_transfer_functions)},
          {"supported_primaries", supportedPrimaries(wlrOutput->supported_primaries)},
      });
    }

    nlohmann::json surfaces = nlohmann::json::array();
    for (const auto& view : server.views()) {
      if (!view->mapped()) {
        continue;
      }
      wlr_surface* surface = view->toplevel()->base->surface;
      const wlr_image_description_v1_data* description = server.surfaceTreeHdrDescription(surface);
      if (description == nullptr) {
        description = server.surfaceImageDescription(surface);
      }
      nlohmann::json entry = {
          {"id", view->extForeignIdentifier() != nullptr ? view->extForeignIdentifier() : ""},
          {"app_id", view->toplevel()->app_id != nullptr ? view->toplevel()->app_id : ""},
          {"title", view->toplevel()->title != nullptr ? view->toplevel()->title : ""},
          {"transfer_function", protocolTransferFunctionName(description != nullptr ? description->tf_named : 0)},
          {"primaries", protocolPrimariesName(description != nullptr ? description->primaries_named : 0)},
          {"max_cll", description != nullptr ? description->max_cll : 0},
          {"max_fall", description != nullptr ? description->max_fall : 0},
      };
      entry["mastering_display_primaries"] = nullptr;
      entry["mastering_luminance"] = nullptr;
      if (description != nullptr && description->has_mastering_display_primaries) {
        const auto& primaries = description->mastering_display_primaries;
        entry["mastering_display_primaries"] = {
            {"red", xy(primaries.red)},
            {"green", xy(primaries.green)},
            {"blue", xy(primaries.blue)},
            {"white", xy(primaries.white)},
        };
      }
      if (description != nullptr && description->has_mastering_luminance) {
        entry["mastering_luminance"] = {
            {"min", description->mastering_luminance.min},
            {"max", description->mastering_luminance.max},
        };
      }
      surfaces.push_back(std::move(entry));
    }

    return nlohmann::json{
        {"ok",
         {
             {"color_manager", server.colorManager() != nullptr},
             {"renderer",
              {
                  {"input_color_transform", server.renderer()->features.input_color_transform},
                  {"output_color_transform", server.renderer()->features.output_color_transform},
                  {"timeline", server.renderer()->features.timeline},
              }},
             {"outputs", std::move(outputs)},
             {"surfaces", std::move(surfaces)},
         }},
    };
  }

  nlohmann::json IpcCommands::tearing(Server& server, std::string_view /*arg*/) {
    nlohmann::json outputs = nlohmann::json::array();
    for (const auto& output : server.outputs()) {
      const bool requested = output->tearingRequested();
      nlohmann::json entry = {
          {"name", output->wlr()->name},
          {"allowed", output->configuredTearingAllowed()},
          {"requested", requested},
          {"last_commit_tearing", output->lastCommitTearing()},
          {"fallback_reason", requested ? output->tearingFallbackReason() : ""},
      };
      entry["last_presentation_flags"] = nullptr;
      entry["last_presentation_presented"] = nullptr;
      entry["last_presentation_vsync"] = nullptr;
      if (output->lastPresentationPresented()) {
        entry["last_presentation_presented"] = *output->lastPresentationPresented();
      }
      if (output->lastPresentationFlags()) {
        entry["last_presentation_flags"] = *output->lastPresentationFlags();
      }
      if (const std::optional<bool> vsync = output->lastPresentationVsync()) {
        entry["last_presentation_vsync"] = *vsync;
      }
      outputs.push_back(std::move(entry));
    }

    nlohmann::json surfaces = nlohmann::json::array();
    for (const auto& view : server.views()) {
      if (!view->mapped()) {
        continue;
      }
      Output* output = view->currentOutput();
      const bool asyncHint = output != nullptr
          ? output->clientTearingHintAsync(view.get())
          : wlr_tearing_control_manager_v1_surface_hint_from_surface(
                server.tearingControlManager(), view->toplevel()->base->surface
            ) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
      nlohmann::json entry = {
          {"id", view->extForeignIdentifier() != nullptr ? view->extForeignIdentifier() : ""},
          {"app_id", view->toplevel()->app_id != nullptr ? view->toplevel()->app_id : ""},
          {"title", view->toplevel()->title != nullptr ? view->toplevel()->title : ""},
          {"output", output != nullptr ? output->wlr()->name : ""},
          {"hint", asyncHint ? "async" : "vsync"},
          {"fullscreen", view->toplevel()->current.fullscreen},
          {"eligible", output != nullptr && output->tearingEligible(view.get())},
      };
      entry["rule_override"] = nullptr;
      if (const std::optional<bool> rule = view->tearingRuleOverride()) {
        entry["rule_override"] = *rule;
      }
      surfaces.push_back(std::move(entry));
    }

    return nlohmann::json{
        {"ok",
         {
             {"protocol", server.tearingControlManager() != nullptr},
             {"outputs", std::move(outputs)},
             {"surfaces", std::move(surfaces)},
         }},
    };
  }

  nlohmann::json IpcCommands::keyboardLayouts(Server& server, std::string_view /*arg*/) {
    const auto state = server.keyboardLayoutState();
    if (!state.has_value()) {
      return nlohmann::json{{"err", "no keyboard"}};
    }
    return nlohmann::json{{"ok", {{"names", state->names}, {"current_index", state->currentIndex}}}};
  }

  nlohmann::json IpcCommands::msg(Server& server, std::string_view arg) {
    Keybind bind{};
    if (!parseAction(std::string(arg), bind)) {
      return nlohmann::json{{"err", "unknown action: " + std::string(arg)}};
    }
    std::string error;
    server.executeKeybindAction(bind, &error);
    if (!error.empty()) {
      return nlohmann::json{{"err", error}};
    }
    return nlohmann::json{{"ok", nullptr}};
  }

  nlohmann::json IpcCommands::outputCreate(Server& server, std::string_view arg) {
    std::string error;
    const std::string name = server.createHeadlessOutput(std::string(arg), &error);
    if (!error.empty()) {
      return nlohmann::json{{"err", error}};
    }
    return nlohmann::json{{"ok", name}};
  }

  nlohmann::json IpcCommands::outputDestroy(Server& server, std::string_view arg) {
    std::string error;
    if (!server.destroyOutput(std::string(arg), &error)) {
      return nlohmann::json{{"err", error}};
    }
    return nlohmann::json{{"ok", nullptr}};
  }

  static constexpr IpcCommandSpec kIpcCommands[] = {
      {"msg", "<action> [args...]", "send an action to the compositor", true, &IpcCommands::msg, nullptr},
      {"windows", "", "list windows (app id and title)", false, &IpcCommands::windows, &printWindows},
      {"workspaces", "", "list workspaces and their layouts", false, &IpcCommands::workspaces, &printWorkspaces},
      {"submap", "", "show the active keybind submap", false, &IpcCommands::submap, &printSubmap},
      {"layers", "", "list layer-shell surfaces", false, &IpcCommands::layers, &printLayers},
      {"color", "", "show color-management state", false, &IpcCommands::color, &printColor},
      {"tearing", "", "show tearing-control state", false, &IpcCommands::tearing, &printTearing},
      {"keyboard-layouts", "", "list keyboard layouts", false, &IpcCommands::keyboardLayouts, nullptr},
      {"output-create", "<name>", "create a headless output (headless sessions only)", true, &IpcCommands::outputCreate,
       &printOutputName},
      {"output-destroy", "<name>", "destroy an output (headless sessions only)", true, &IpcCommands::outputDestroy,
       nullptr},
  };

  std::span<const IpcCommandSpec> ipcCommands() { return kIpcCommands; }

  const IpcCommandSpec* findIpcCommand(std::string_view name) {
    for (const auto& spec : kIpcCommands) {
      if (name == spec.name) {
        return &spec;
      }
    }
    return nullptr;
  }

} // namespace umbriel
