#include "input/global_shortcuts_bridge.h"

#include "input/global_shortcuts_input.h"
#include "input/global_shortcuts_store.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <umbriel-global-shortcuts-v1-server-protocol.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace umbriel {
  namespace {
    constexpr size_t kMaxClientSessions = 16;
    constexpr uint32_t kSuccess = 0;
    constexpr uint32_t kCancelled = 1;
    constexpr uint32_t kFailure = 2;

    uint32_t timeMsec() {
      return static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count()
      );
    }
  } // namespace

  struct GlobalShortcutsBridge::Impl {
    struct Session {
      Impl& owner;
      wl_resource* resource;
      GlobalShortcutSessionId id;
      std::vector<GlobalShortcutRequest> staged;
      std::vector<GlobalShortcut> approval;
      std::optional<uint32_t> pending;
      bool stagingFailed = false;

      Session(Impl& owner, wl_resource* resource, GlobalShortcutSessionId id)
          : owner(owner), resource(resource), id(id) {}
    };

    GlobalShortcutsRegistry& registry;
    GlobalShortcutsStore& store;
    wl_global* global = nullptr;
    std::map<GlobalShortcutSessionId, std::unique_ptr<Session>> sessions;
    std::vector<wl_resource*> managers;
    AuthorizationRequest authorize;
    AuthorizationCancelled cancelled;
    GlobalShortcutsInput input;
    bool tearingDown = false;
    bool dispatching = false;

    Impl(wl_display* display, GlobalShortcutsRegistry& registry, GlobalShortcutsStore& store)
        : registry(registry), store(store),
          input(registry, [this](const GlobalShortcutBinding& binding, bool pressed, uint32_t time) {
            const auto found = sessions.find(binding.session);
            if (found != sessions.end()) {
              if (pressed) {
                umbriel_global_shortcuts_session_v1_send_activated(found->second->resource, binding.id.c_str(), time);
              } else {
                umbriel_global_shortcuts_session_v1_send_deactivated(found->second->resource, binding.id.c_str(), time);
              }
            }
          }) {
      global = wl_global_create(
          display, &umbriel_global_shortcuts_manager_v1_interface, 1, this,
          [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            auto& owner = *static_cast<Impl*>(data);
            wl_resource* resource =
                wl_resource_create(client, &umbriel_global_shortcuts_manager_v1_interface, version, id);
            if (resource == nullptr) {
              wl_client_post_no_memory(client);
              return;
            }
            wl_resource_set_implementation(resource, &kManager, &owner, [](wl_resource* resource) {
              if (auto* owner = static_cast<Impl*>(wl_resource_get_user_data(resource))) {
                std::erase(owner->managers, resource);
              }
            });
            owner.managers.push_back(resource);
          }
      );
      if (global == nullptr) {
        throw std::runtime_error("could not create the global shortcuts bridge");
      }
    }

    ~Impl() {
      tearingDown = true;
      input.suspend(timeMsec());
      wl_global_destroy(global);
      while (!sessions.empty()) {
        close(sessions.begin()->first);
      }
      for (auto* resource : managers) {
        wl_resource_set_user_data(resource, nullptr);
      }
    }

    static Session* session(wl_resource* resource) {
      return static_cast<Session*>(wl_resource_get_user_data(resource));
    }

    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void createSession(wl_client* client, wl_resource* manager, uint32_t id, const char* application) {
      wl_resource* resource = wl_resource_create(client, &umbriel_global_shortcuts_session_v1_interface, 1, id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      // Rejected resources remain inert until the client destroys them.
      wl_resource_set_implementation(resource, &kSession, nullptr, [](wl_resource* resource) {
        if (auto* state = session(resource)) {
          auto& owner = state->owner;
          owner.close(state->id, false);
          owner.dispatchChanges();
        }
      });
      auto* owner = static_cast<Impl*>(wl_resource_get_user_data(manager));
      const auto count = owner == nullptr
          ? size_t{0}
          : static_cast<size_t>(std::ranges::count_if(owner->sessions, [client](const auto& entry) {
              return wl_resource_get_client(entry.second->resource) == client;
            }));
      const auto sessionId =
          owner != nullptr && count < kMaxClientSessions ? owner->registry.createSession(application) : std::nullopt;
      if (!sessionId) {
        umbriel_global_shortcuts_session_v1_send_closed(resource);
        return;
      }
      auto state = std::make_unique<Session>(*owner, resource, *sessionId);
      wl_resource_set_user_data(resource, state.get());
      owner->sessions.emplace(*sessionId, std::move(state));
    }

    void finishPending(Session& state, uint32_t status, bool notify = false) {
      const auto request = std::exchange(state.pending, std::nullopt);
      if (!request) {
        return;
      }
      (void)registry.cancelBind(state.id);
      state.approval.clear();
      umbriel_global_shortcuts_session_v1_send_bound(state.resource, *request, status);
      if (notify && cancelled) {
        cancelled(state.id);
      }
    }

    bool duplicate(Session& state, uint32_t request) {
      if (state.pending != request) {
        return false;
      }
      finishPending(state, kFailure, true);
      return true;
    }

    void close(GlobalShortcutSessionId id, bool sendClosed = true) {
      const auto found = sessions.find(id);
      if (found == sessions.end()) {
        return;
      }
      auto* resource = found->second->resource;
      const auto request = found->second->pending;
      (void)registry.closeSession(id);
      input.reconcile(timeMsec());
      wl_resource_set_user_data(resource, nullptr);
      sessions.erase(found);
      if (sendClosed) {
        if (request) {
          umbriel_global_shortcuts_session_v1_send_bound(resource, *request, kFailure);
        }
        umbriel_global_shortcuts_session_v1_send_closed(resource);
      }
      if (request && cancelled) {
        cancelled(id);
      }
    }

    void restore(Session& state, uint32_t request, std::string_view token) {
      if (duplicate(state, request)) {
        return;
      }
      // Check ordering before consuming a durable credential.
      if (!registry.canRestore(state.id)) {
        umbriel_global_shortcuts_session_v1_send_restored(state.resource, request, kFailure);
        return;
      }
      const auto snapshot = registry.session(state.id);
      const auto credential = store.restore(token, snapshot->application);
      const bool restored = registry.restore(state.id, credential ? std::optional{credential->grant} : std::nullopt);
      if (restored) {
        umbriel_global_shortcuts_session_v1_send_grant(state.resource, request, credential->token.c_str());
      }
      umbriel_global_shortcuts_session_v1_send_restored(state.resource, request, restored ? kSuccess : kFailure);
    }

    void stage(Session& state, const char* id, const char* description, const char* preferred) {
      const auto snapshot = registry.session(state.id);
      if (!snapshot || snapshot->bindAttempted || state.stagingFailed) {
        return;
      }
      // Validate lengths before copying untrusted strings into staging.
      if (state.staged.size() >= kMaxGlobalShortcuts
          || std::char_traits<char>::length(id) > 256
          || std::char_traits<char>::length(description) > 1024
          || std::char_traits<char>::length(preferred) > 256) {
        state.stagingFailed = true;
        state.staged.clear();
        return;
      }
      GlobalShortcutRequest shortcut{id, description, preferred};
      if (!validGlobalShortcutRequest(shortcut)
          || std::ranges::find(state.staged, shortcut.id, &GlobalShortcutRequest::id) != state.staged.end()) {
        state.stagingFailed = true;
        state.staged.clear();
        return;
      }
      state.staged.push_back(std::move(shortcut));
    }

    static void sendShortcut(wl_resource* resource, uint32_t request, const GlobalShortcut& shortcut) {
      const auto trigger = describeGlobalShortcutTrigger(*shortcut.trigger);
      umbriel_global_shortcuts_session_v1_send_shortcut(
          resource, request, shortcut.id.c_str(), shortcut.description.c_str(), trigger.c_str()
      );
    }

    void bind(Session& state, uint32_t request) {
      if (duplicate(state, request)) {
        return;
      }
      const bool failed = std::exchange(state.stagingFailed, false);
      const auto pending = registry.beginBind(state.id, std::exchange(state.staged, {}));
      if (!pending || failed) {
        if (pending) {
          (void)registry.cancelBind(state.id);
        }
        umbriel_global_shortcuts_session_v1_send_bound(state.resource, request, kFailure);
        return;
      }
      state.pending = request;
      state.approval = pending->approval;
      if (pending->approval.empty()) {
        completeBind(state);
      } else if (authorize) {
        const auto snapshot = registry.session(state.id);
        authorize(state.id, snapshot->application, *pending);
      } else {
        finishPending(state, kFailure);
      }
    }

    void completeBind(Session& state) {
      const auto snapshot = registry.session(state.id);
      if (!snapshot || !snapshot->pending) {
        finishPending(state, kFailure, true);
        return;
      }
      std::optional<GlobalShortcutCredential> credential;
      if (!state.approval.empty()) {
        credential = store.approve(snapshot->application, state.approval, snapshot->grantId);
        if (!credential) {
          finishPending(state, kFailure);
          return;
        }
      }
      if (!registry.commitBind(state.id, credential ? std::optional{credential->grant} : std::nullopt)) {
        finishPending(state, kFailure);
        return;
      }
      const auto bound = registry.list(state.id).value();
      const auto current = registry.session(state.id).value();
      if (!current.grantId.empty() && !store.recordBindings(current.grantId, current.application, bound)) {
        close(state.id);
        dispatchChanges();
        return;
      }
      const auto request = *std::exchange(state.pending, std::nullopt);
      state.approval.clear();
      if (credential) {
        umbriel_global_shortcuts_session_v1_send_grant(state.resource, request, credential->token.c_str());
      }
      for (const auto& shortcut : bound) {
        sendShortcut(state.resource, request, shortcut);
      }
      umbriel_global_shortcuts_session_v1_send_bound(state.resource, request, kSuccess);
      dispatchChanges();
    }

    void list(Session& state, uint32_t request) {
      if (duplicate(state, request)) {
        return;
      }
      const auto bound = registry.list(state.id);
      if (bound) {
        for (const auto& shortcut : *bound) {
          sendShortcut(state.resource, request, shortcut);
        }
      }
      umbriel_global_shortcuts_session_v1_send_listed(state.resource, request, bound ? kSuccess : kFailure);
    }

    void dispatchChanges() {
      if (dispatching || tearingDown) {
        return;
      }
      dispatching = true;
      for (;;) {
        input.reconcile(timeMsec());
        std::vector<GlobalShortcutSessionId> removed;
        for (const auto& [id, state] : sessions) {
          (void)state;
          if (!registry.session(id)) {
            removed.push_back(id);
          }
        }
        for (const auto id : removed) {
          close(id);
        }
        const auto changes = registry.takeChanges();
        if (changes.changed.empty() && changes.cancelled.empty()) {
          break;
        }
        for (const auto id : changes.cancelled) {
          if (const auto found = sessions.find(id); found != sessions.end()) {
            finishPending(*found->second, kFailure, true);
          }
        }
        for (const auto id : changes.changed) {
          const auto found = sessions.find(id);
          if (found == sessions.end()) {
            continue;
          }
          const auto snapshot = registry.session(id).value();
          const auto bound = registry.list(id).value();
          if (!snapshot.grantId.empty() && !store.recordBindings(snapshot.grantId, snapshot.application, bound)) {
            close(id);
            continue;
          }
          for (const auto& shortcut : bound) {
            const auto trigger = describeGlobalShortcutTrigger(*shortcut.trigger);
            umbriel_global_shortcuts_session_v1_send_changed(
                found->second->resource, shortcut.id.c_str(), shortcut.description.c_str(), trigger.c_str()
            );
          }
          umbriel_global_shortcuts_session_v1_send_changes_done(found->second->resource);
        }
      }
      dispatching = false;
    }

    static const struct umbriel_global_shortcuts_manager_v1_interface kManager;
    static const struct umbriel_global_shortcuts_session_v1_interface kSession;
  };

  const struct umbriel_global_shortcuts_manager_v1_interface GlobalShortcutsBridge::Impl::kManager{
      .destroy = destroy,
      .create_session = createSession,
  };

  const struct umbriel_global_shortcuts_session_v1_interface GlobalShortcutsBridge::Impl::kSession{
      .destroy = destroy,
      .restore =
          [](wl_client*, wl_resource* resource, uint32_t request, const char* token) {
            if (auto* state = session(resource)) {
              state->owner.restore(*state, request, token);
            } else {
              umbriel_global_shortcuts_session_v1_send_restored(resource, request, kFailure);
            }
          },
      .add_shortcut =
          [](wl_client*, wl_resource* resource, const char* id, const char* description, const char* preferred) {
            if (auto* state = session(resource)) {
              state->owner.stage(*state, id, description, preferred);
            }
          },
      .bind =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            if (auto* state = session(resource)) {
              state->owner.bind(*state, request);
            } else {
              umbriel_global_shortcuts_session_v1_send_bound(resource, request, kFailure);
            }
          },
      .list =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            if (auto* state = session(resource)) {
              state->owner.list(*state, request);
            } else {
              umbriel_global_shortcuts_session_v1_send_listed(resource, request, kFailure);
            }
          },
      .cancel =
          [](wl_client*, wl_resource* resource, uint32_t request) {
            if (auto* state = session(resource); state != nullptr && state->pending == request) {
              state->owner.finishPending(*state, kCancelled, true);
            }
          },
  };

  GlobalShortcutsBridge::GlobalShortcutsBridge(
      wl_display* display, GlobalShortcutsRegistry& registry, GlobalShortcutsStore& store
  )
      : m_impl(std::make_unique<Impl>(display, registry, store)) {}

  GlobalShortcutsBridge::~GlobalShortcutsBridge() = default;

  void GlobalShortcutsBridge::setAuthorize(AuthorizationRequest request) { m_impl->authorize = std::move(request); }
  void GlobalShortcutsBridge::setAuthorizationCancelled(AuthorizationCancelled cancelled) {
    m_impl->cancelled = std::move(cancelled);
  }

  void GlobalShortcutsBridge::authorizeSession(GlobalShortcutSessionId id, Authorization authorization) {
    const auto found = m_impl->sessions.find(id);
    if (found == m_impl->sessions.end() || !found->second->pending) {
      return;
    }
    switch (authorization) {
    case Authorization::Approved:
      m_impl->completeBind(*found->second);
      break;
    case Authorization::Cancelled:
      m_impl->finishPending(*found->second, kCancelled);
      break;
    case Authorization::Failed:
      m_impl->finishPending(*found->second, kFailure);
      break;
    }
  }

  void GlobalShortcutsBridge::dispatchChanges() { m_impl->dispatchChanges(); }

  GlobalShortcutsInput& GlobalShortcutsBridge::input() { return m_impl->input; }

} // namespace umbriel
