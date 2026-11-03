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

  struct Group {
    ext_workspace_group_handle_v1* handle = nullptr;
    std::vector<ext_workspace_handle_v1*> workspaces;
    std::vector<wl_output*> outputs;
  };

  struct Output {
    wl_output* handle = nullptr;
    std::string name;
  };

  struct State {
    wl_registry* registry = nullptr;
    ext_workspace_manager_v1* manager = nullptr;
    std::vector<std::unique_ptr<Group>> groups;
    std::vector<std::unique_ptr<Workspace>> workspaces;
    std::vector<std::unique_ptr<Output>> outputs;
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
  void groupOutputEnter(void* data, ext_workspace_group_handle_v1*, wl_output* output) {
    static_cast<Group*>(data)->outputs.push_back(output);
  }
  void groupOutputLeave(void* data, ext_workspace_group_handle_v1*, wl_output* output) {
    std::erase(static_cast<Group*>(data)->outputs, output);
  }
  void groupWorkspaceEnter(void* data, ext_workspace_group_handle_v1*, ext_workspace_handle_v1* workspace) {
    static_cast<Group*>(data)->workspaces.push_back(workspace);
  }
  void groupWorkspaceLeave(void* data, ext_workspace_group_handle_v1*, ext_workspace_handle_v1* workspace) {
    auto& workspaces = static_cast<Group*>(data)->workspaces;
    std::erase(workspaces, workspace);
  }
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
    auto entry = std::make_unique<Group>();
    entry->handle = group;
    ext_workspace_group_handle_v1_add_listener(group, &kGroupListener, entry.get());
    state->groups.push_back(std::move(entry));
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

  constexpr wl_output_listener kOutputListener{
      .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
                     int32_t) {},
      .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
      .done = [](void*, wl_output*) {},
      .scale = [](void*, wl_output*, int32_t) {},
      .name = [](void* data, wl_output*, const char* name) { static_cast<Output*>(data)->name = name; },
      .description = [](void*, wl_output*, const char*) {},
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::string_view(interface) == wl_output_interface.name) {
      auto output = std::make_unique<Output>();
      output->handle =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
      wl_output_add_listener(output->handle, &kOutputListener, output.get());
      state->outputs.push_back(std::move(output));
      return;
    }
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

// Default output is the active workspace of each group, one per line. `--all`
// lists every visible workspace as "id<TAB>name"; `--active` lists active ones in the
// same form. `--groups` adds the output name as a third field. `--create OUTPUT NAME`
// sends one protocol create request to the group advertising that output.
int main(int argc, char** argv) {
  const bool listAll = argc == 2 && std::string_view(argv[1]) == "--all";
  const bool listActive = argc == 2 && std::string_view(argv[1]) == "--active";
  const bool listGroups = argc == 2 && std::string_view(argv[1]) == "--groups";
  const bool create = argc == 4 && std::string_view(argv[1]) == "--create";
  if (!((argc == 1) || listAll || listActive || listGroups || create)) {
    std::println(stderr, "usage: workspace-client [--all|--active|--groups|--create OUTPUT NAME]");
    return 2;
  }
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

  if (create) {
    const auto group = std::ranges::find_if(state.groups, [&](const auto& candidate) {
      return std::ranges::any_of(state.outputs, [&](const auto& output) {
        return output->name == argv[2]
            && std::ranges::find(candidate->outputs, output->handle) != candidate->outputs.end();
      });
    });
    if (group == state.groups.end()) {
      std::println(stderr, "workspace-client: output group not found: {}", argv[2]);
      return 1;
    }
    ext_workspace_group_handle_v1_create_workspace((*group)->handle, argv[3]);
    ext_workspace_manager_v1_commit(state.manager);
    if (wl_display_roundtrip(display) < 0) {
      std::println(stderr, "workspace-client: create request roundtrip failed");
      return 1;
    }
  }

  int active = 0;
  for (const auto& workspace : state.workspaces) {
    if (workspace->removed || (workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN) != 0) {
      continue;
    }
    if (listAll) {
      std::println("{}\t{}", workspace->id, workspace->name);
    }
    if (listGroups) {
      for (const auto& group : state.groups) {
        if (std::ranges::find(group->workspaces, workspace->handle) == group->workspaces.end()) {
          continue;
        }
        for (const auto& output : state.outputs) {
          if (std::ranges::find(group->outputs, output->handle) != group->outputs.end()) {
            std::println("{}\t{}\t{}", workspace->id, workspace->name, output->name);
          }
        }
      }
    }
    if ((workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0) {
      if (listActive) {
        std::println("{}\t{}", workspace->id, workspace->name);
      } else if (!listAll && !listGroups && !create) {
        std::println("{}", workspace->name);
      }
      ++active;
    }
  }

  for (const auto& workspace : state.workspaces) {
    ext_workspace_handle_v1_destroy(workspace->handle);
  }
  for (const auto& group : state.groups) {
    ext_workspace_group_handle_v1_destroy(group->handle);
  }
  if (state.manager != nullptr) {
    ext_workspace_manager_v1_destroy(state.manager);
  }
  for (const auto& output : state.outputs) {
    wl_output_destroy(output->handle);
  }
  wl_registry_destroy(state.registry);
  wl_display_disconnect(display);

  if (!state.done || (!listAll && !listActive && !listGroups && !create && active != 1)) {
    std::println(stderr, "workspace-client: expected one active workspace, got {}", active);
    return 1;
  }
  return 0;
}
