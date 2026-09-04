#include "cli/outputs.h"

#include "output/identity.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <wayland-client.h>

namespace umbriel {
  namespace {

    struct ModeInfo {
      zwlr_output_mode_v1* proxy = nullptr;
      int32_t width = 0;
      int32_t height = 0;
      int32_t refreshMHz = 0;
      bool preferred = false;
    };

    struct HeadInfo {
      zwlr_output_head_v1* proxy = nullptr;
      std::string name;
      std::string description;
      std::string make;
      std::string model;
      std::string serial;
      int32_t physWidthMm = 0;
      int32_t physHeightMm = 0;
      bool enabled = false;
      int32_t x = 0;
      int32_t y = 0;
      int32_t transform = 0;
      double scale = 1.0;
      int32_t adaptiveSync = -1; // -1 unknown
      zwlr_output_mode_v1* currentMode = nullptr;
      std::deque<ModeInfo> modes;
    };

    struct State {
      zwlr_output_manager_v1* manager = nullptr;
      uint32_t managerVersion = 0;
      std::deque<HeadInfo> heads;
      bool done = false;
    };

    // ── Mode listeners ──────────────────────────────────────────────────────

    void modeHandleSize(void* data, zwlr_output_mode_v1* /*mode*/, int32_t width, int32_t height) {
      auto* info = static_cast<ModeInfo*>(data);
      info->width = width;
      info->height = height;
    }

    void modeHandleRefresh(void* data, zwlr_output_mode_v1* /*mode*/, int32_t refresh) {
      auto* info = static_cast<ModeInfo*>(data);
      info->refreshMHz = refresh;
    }

    void modeHandlePreferred(void* data, zwlr_output_mode_v1* /*mode*/) {
      auto* info = static_cast<ModeInfo*>(data);
      info->preferred = true;
    }

    void modeHandleFinished(void* /*data*/, zwlr_output_mode_v1* /*mode*/) {
      // No-op; modes are not removed during initial enumeration.
    }

    const zwlr_output_mode_v1_listener kModeListener = {
        .size = modeHandleSize,
        .refresh = modeHandleRefresh,
        .preferred = modeHandlePreferred,
        .finished = modeHandleFinished,
    };

    // ── Head listeners ──────────────────────────────────────────────────────

    void headHandleName(void* data, zwlr_output_head_v1* /*head*/, const char* name) {
      auto* info = static_cast<HeadInfo*>(data);
      info->name = name != nullptr ? name : "";
    }

    void headHandleDescription(void* data, zwlr_output_head_v1* /*head*/, const char* description) {
      auto* info = static_cast<HeadInfo*>(data);
      info->description = description != nullptr ? description : "";
    }

    void headHandlePhysicalSize(void* data, zwlr_output_head_v1* /*head*/, int32_t width, int32_t height) {
      auto* info = static_cast<HeadInfo*>(data);
      info->physWidthMm = width;
      info->physHeightMm = height;
    }

    void headHandleMode(void* data, zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* mode) {
      auto* info = static_cast<HeadInfo*>(data);
      info->modes.emplace_back();
      info->modes.back().proxy = mode;
      zwlr_output_mode_v1_add_listener(mode, &kModeListener, &info->modes.back());
    }

    void headHandleEnabled(void* data, zwlr_output_head_v1* /*head*/, int32_t enabled) {
      auto* info = static_cast<HeadInfo*>(data);
      info->enabled = enabled != 0;
    }

    void headHandleCurrentMode(void* data, zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* mode) {
      auto* info = static_cast<HeadInfo*>(data);
      info->currentMode = mode;
    }

    void headHandlePosition(void* data, zwlr_output_head_v1* /*head*/, int32_t x, int32_t y) {
      auto* info = static_cast<HeadInfo*>(data);
      info->x = x;
      info->y = y;
    }

    void headHandleTransform(void* data, zwlr_output_head_v1* /*head*/, int32_t transform) {
      auto* info = static_cast<HeadInfo*>(data);
      info->transform = transform;
    }

    void headHandleScale(void* data, zwlr_output_head_v1* /*head*/, wl_fixed_t scale) {
      auto* info = static_cast<HeadInfo*>(data);
      info->scale = wl_fixed_to_double(scale);
    }

    void headHandleFinished(void* /*data*/, zwlr_output_head_v1* /*head*/) {
      // No-op for initial enumeration.
    }

    void headHandleMake(void* data, zwlr_output_head_v1* /*head*/, const char* make) {
      auto* info = static_cast<HeadInfo*>(data);
      info->make = make != nullptr ? make : "";
    }

    void headHandleModel(void* data, zwlr_output_head_v1* /*head*/, const char* model) {
      auto* info = static_cast<HeadInfo*>(data);
      info->model = model != nullptr ? model : "";
    }

    void headHandleSerialNumber(void* data, zwlr_output_head_v1* /*head*/, const char* serial) {
      auto* info = static_cast<HeadInfo*>(data);
      info->serial = serial != nullptr ? serial : "";
    }

    void headHandleAdaptiveSync(void* data, zwlr_output_head_v1* /*head*/, uint32_t state) {
      auto* info = static_cast<HeadInfo*>(data);
      info->adaptiveSync = static_cast<int32_t>(state);
    }

    const zwlr_output_head_v1_listener kHeadListener = {
        .name = headHandleName,
        .description = headHandleDescription,
        .physical_size = headHandlePhysicalSize,
        .mode = headHandleMode,
        .enabled = headHandleEnabled,
        .current_mode = headHandleCurrentMode,
        .position = headHandlePosition,
        .transform = headHandleTransform,
        .scale = headHandleScale,
        .finished = headHandleFinished,
        .make = headHandleMake,
        .model = headHandleModel,
        .serial_number = headHandleSerialNumber,
        .adaptive_sync = headHandleAdaptiveSync,
    };

    // ── Manager listeners ───────────────────────────────────────────────────

    void managerHandleHead(void* data, zwlr_output_manager_v1* /*manager*/, zwlr_output_head_v1* head) {
      auto* state = static_cast<State*>(data);
      state->heads.emplace_back();
      state->heads.back().proxy = head;
      zwlr_output_head_v1_add_listener(head, &kHeadListener, &state->heads.back());
    }

    void managerHandleDone(void* data, zwlr_output_manager_v1* /*manager*/, uint32_t /*serial*/) {
      auto* state = static_cast<State*>(data);
      state->done = true;
    }

    void managerHandleFinished(void* /*data*/, zwlr_output_manager_v1* /*manager*/) {
      // No-op.
    }

    const zwlr_output_manager_v1_listener kManagerListener = {
        .head = managerHandleHead,
        .done = managerHandleDone,
        .finished = managerHandleFinished,
    };

    // ── Registry ────────────────────────────────────────────────────────────

    void
    registryHandleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto* state = static_cast<State*>(data);
      if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
        uint32_t bindVersion = version < 4 ? version : 4;
        state->manager = static_cast<zwlr_output_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, bindVersion)
        );
        state->managerVersion = bindVersion;
        zwlr_output_manager_v1_add_listener(state->manager, &kManagerListener, state);
      }
    }

    void registryHandleGlobalRemove(void* /*data*/, wl_registry* /*registry*/, uint32_t /*name*/) {
      // No-op.
    }

    const wl_registry_listener kRegistryListener = {
        .global = registryHandleGlobal,
        .global_remove = registryHandleGlobalRemove,
    };

    // ── Output formatting ───────────────────────────────────────────────────

    constexpr const char* kTransformNames[] = {
        "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270",
    };

    const char* transformName(int32_t transform) {
      if (transform >= 0 && transform < 8) {
        return kTransformNames[transform];
      }
      return "unknown";
    }

    void printHead(const HeadInfo& head) {
      std::println("{} \"{}\"", head.name, head.description);
      std::println("  Enabled: {}", head.enabled ? "yes" : "no");
      const OutputIdentity identity{
          .connector = head.name,
          .make = head.make,
          .model = head.model,
          .serial = head.serial,
      };
      if (!head.make.empty() || !head.model.empty() || !head.serial.empty()) {
        std::println("  Config name: \"{}\"", outputDescriptor(identity));
        std::print("  ");
        bool first = true;
        if (!head.make.empty()) {
          std::print("Make: {}", head.make);
          first = false;
        }
        if (!head.model.empty()) {
          if (!first) {
            std::print("  ");
          }
          std::print("Model: {}", head.model);
          first = false;
        }
        if (!head.serial.empty()) {
          if (!first) {
            std::print("  ");
          }
          std::print("Serial: {}", head.serial);
        }
        std::println("");
      }
      if (head.physWidthMm > 0 || head.physHeightMm > 0) {
        std::println("  Physical size: {}x{} mm", head.physWidthMm, head.physHeightMm);
      }
      std::println("  Position: {},{}", head.x, head.y);
      std::println("  Transform: {}", transformName(head.transform));
      std::println("  Scale: {:f}", head.scale);
      if (head.adaptiveSync >= 0) {
        const char* syncState = "unknown";
        // zwlr_output_head_v1_adaptive_sync_state enum: 0=disabled, 1=enabled
        if (head.adaptiveSync == 0) {
          syncState = "disabled";
        } else if (head.adaptiveSync == 1) {
          syncState = "enabled";
        }
        std::println("  Adaptive sync: {}", syncState);
      }
      if (!head.modes.empty()) {
        std::println("  Modes:");
        for (const auto& mode : head.modes) {
          const bool isCurrent = mode.proxy == head.currentMode;
          const bool isPreferred = mode.preferred;
          std::print("    {}x{} @ {:.3f} Hz", mode.width, mode.height, static_cast<double>(mode.refreshMHz) / 1000.0);
          if (isPreferred || isCurrent) {
            std::print(" (");
            if (isPreferred) {
              std::print("preferred");
              if (isCurrent) {
                std::print(", ");
              }
            }
            if (isCurrent) {
              std::print("current");
            }
            std::print(")");
          }
          std::println("");
        }
      }
    }

    nlohmann::json jsonHead(const HeadInfo& head) {
      nlohmann::json modes = nlohmann::json::array();
      for (const auto& mode : head.modes) {
        modes.push_back({
            {"width", mode.width},
            {"height", mode.height},
            {"refresh_mhz", mode.refreshMHz},
            {"preferred", mode.preferred},
            {"current", mode.proxy == head.currentMode},
        });
      }

      nlohmann::json adaptiveSync = nullptr;
      if (head.adaptiveSync == 0) {
        adaptiveSync = false;
      } else if (head.adaptiveSync == 1) {
        adaptiveSync = true;
      }

      const bool hasIdentity = !head.make.empty() || !head.model.empty() || !head.serial.empty();
      const OutputIdentity identity{
          .connector = head.name,
          .make = head.make,
          .model = head.model,
          .serial = head.serial,
      };
      return {
          {"name", head.name},
          {"description", head.description},
          {"make", head.make},
          {"model", head.model},
          {"serial", head.serial},
          {"config_name", hasIdentity ? nlohmann::json(outputDescriptor(identity)) : nlohmann::json(nullptr)},
          {"physical_size", {{"width_mm", head.physWidthMm}, {"height_mm", head.physHeightMm}}},
          {"enabled", head.enabled},
          {"position", {{"x", head.x}, {"y", head.y}}},
          {"transform", transformName(head.transform)},
          {"scale", head.scale},
          {"adaptive_sync", std::move(adaptiveSync)},
          {"modes", std::move(modes)},
      };
    }

    void printHeadsJson(const std::deque<HeadInfo>& heads) {
      nlohmann::json output = nlohmann::json::array();
      for (const auto& head : heads) {
        output.push_back(jsonHead(head));
      }
      std::println("{}", output.dump());
    }

    void printHeadsHuman(const std::deque<HeadInfo>& heads) {
      for (size_t i = 0; i < heads.size(); ++i) {
        if (i > 0) {
          std::println("");
        }
        printHead(heads[i]);
      }
    }

  } // namespace

  int runOutputsCommand(bool json) {
    wl_display* display = wl_display_connect(nullptr);
    if (display == nullptr) {
      std::println(stderr, "error: cannot connect to Wayland display (is the compositor running?)");
      return EXIT_FAILURE;
    }

    State state{};
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &state);
    wl_display_roundtrip(display);

    if (state.manager == nullptr) {
      std::println(stderr, "error: compositor does not support wlr-output-management-unstable-v1");
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      return EXIT_FAILURE;
    }

    // Second roundtrip delivers head/mode events, terminated by manager.done.
    wl_display_roundtrip(display);

    if (json) {
      printHeadsJson(state.heads);
    } else {
      printHeadsHuman(state.heads);
    }

    // Cleanup.
    for (auto& head : state.heads) {
      for (auto& mode : head.modes) {
        zwlr_output_mode_v1_destroy(mode.proxy);
      }
      zwlr_output_head_v1_destroy(head.proxy);
    }
    zwlr_output_manager_v1_destroy(state.manager);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return EXIT_SUCCESS;
  }

} // namespace umbriel
