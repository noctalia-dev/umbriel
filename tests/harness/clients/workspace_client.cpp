#include "ext-workspace-v1-client-protocol.h"

#include <algorithm>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-client.h>

namespace {
  struct Workspace {
    ext_workspace_handle_v1* handle = nullptr;
    std::string id;
    std::string name;
    uint32_t state = 0;
    bool removed = false;
  };

  struct State {
    wl_registry* registry = nullptr;
    ext_workspace_manager_v1* manager = nullptr;
    std::vector<ext_workspace_group_handle_v1*> groups;
    std::vector<std::unique_ptr<Workspace>> workspaces;
    bool done = false;
  };

  void workspaceId(void* data, ext_workspace_handle_v1*, const char* id) {
    static_cast<Workspace*>(data)->id = id != nullptr ? id : "";
  }

  void workspaceName(void* data, ext_workspace_handle_v1*, const char* name) {
    static_cast<Workspace*>(data)->name = name != nullptr ? name : "";
  }

  void workspaceCoordinates(void*, ext_workspace_handle_v1*, wl_array*) {}

  void workspaceState(void* data, ext_workspace_handle_v1*, uint32_t state) {
    static_cast<Workspace*>(data)->state = state;
  }

  void workspaceCapabilities(void*, ext_workspace_handle_v1*, uint32_t) {}

  void workspaceRemoved(void* data, ext_workspace_handle_v1*) { static_cast<Workspace*>(data)->removed = true; }

  constexpr ext_workspace_handle_v1_listener kWorkspaceListener{
      .id = workspaceId,
      .name = workspaceName,
      .coordinates = workspaceCoordinates,
      .state = workspaceState,
      .capabilities = workspaceCapabilities,
      .removed = workspaceRemoved,
  };

  void groupCapabilities(void*, ext_workspace_group_handle_v1*, uint32_t) {}
  void groupOutputEnter(void*, ext_workspace_group_handle_v1*, wl_output*) {}
  void groupOutputLeave(void*, ext_workspace_group_handle_v1*, wl_output*) {}
  void groupWorkspaceEnter(void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {}
  void groupWorkspaceLeave(void*, ext_workspace_group_handle_v1*, ext_workspace_handle_v1*) {}
  void groupRemoved(void*, ext_workspace_group_handle_v1*) {}

  constexpr ext_workspace_group_handle_v1_listener kGroupListener{
      .capabilities = groupCapabilities,
      .output_enter = groupOutputEnter,
      .output_leave = groupOutputLeave,
      .workspace_enter = groupWorkspaceEnter,
      .workspace_leave = groupWorkspaceLeave,
      .removed = groupRemoved,
  };

  void managerGroup(void* data, ext_workspace_manager_v1*, ext_workspace_group_handle_v1* group) {
    auto* state = static_cast<State*>(data);
    state->groups.push_back(group);
    ext_workspace_group_handle_v1_add_listener(group, &kGroupListener, state);
  }

  void managerWorkspace(void* data, ext_workspace_manager_v1*, ext_workspace_handle_v1* handle) {
    auto* state = static_cast<State*>(data);
    auto workspace = std::make_unique<Workspace>();
    workspace->handle = handle;
    ext_workspace_handle_v1_add_listener(handle, &kWorkspaceListener, workspace.get());
    state->workspaces.push_back(std::move(workspace));
  }

  void managerDone(void* data, ext_workspace_manager_v1*) { static_cast<State*>(data)->done = true; }
  void managerFinished(void* data, ext_workspace_manager_v1*) { static_cast<State*>(data)->done = true; }

  constexpr ext_workspace_manager_v1_listener kManagerListener{
      .workspace_group = managerGroup,
      .workspace = managerWorkspace,
      .done = managerDone,
      .finished = managerFinished,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::string_view(interface) != ext_workspace_manager_v1_interface.name) {
      return;
    }
    state->manager = static_cast<ext_workspace_manager_v1*>(
        wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, std::min(version, 1U))
    );
    ext_workspace_manager_v1_add_listener(state->manager, &kManagerListener, state);
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };
} // namespace

// Default output is the active workspace of each group, one per line. `--all` lists every workspace as "id<TAB>name".
int main(int argc, char** argv) {
  const bool listAll = argc > 1 && std::string_view(argv[1]) == "--all";
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "workspace-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state;
  state.registry = wl_display_get_registry(display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  for (int roundtrip = 0; roundtrip < 4 && !state.done; ++roundtrip) {
    if (wl_display_roundtrip(display) < 0) {
      break;
    }
  }

  int active = 0;
  for (const auto& workspace : state.workspaces) {
    if (workspace->removed) {
      continue;
    }
    if (listAll) {
      std::println("{}\t{}", workspace->id, workspace->name);
    }
    if ((workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0) {
      if (!listAll) {
        std::println("{}", workspace->name);
      }
      ++active;
    }
  }

  for (const auto& workspace : state.workspaces) {
    ext_workspace_handle_v1_destroy(workspace->handle);
  }
  for (ext_workspace_group_handle_v1* group : state.groups) {
    ext_workspace_group_handle_v1_destroy(group);
  }
  if (state.manager != nullptr) {
    ext_workspace_manager_v1_destroy(state.manager);
  }
  wl_registry_destroy(state.registry);
  wl_display_disconnect(display);

  if (!state.done || (!listAll && active != 1)) {
    std::println(stderr, "workspace-client: expected one active workspace, got {}", active);
    return 1;
  }
  return 0;
}
