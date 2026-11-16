#include "input/global_shortcuts_bridge.h"

#include "check.h"
#include "core/private_state_file.h"
#include "input/global_shortcuts_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <umbriel-global-shortcuts-v1-client-protocol.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

using namespace umbriel;

namespace {
  using Authorization = GlobalShortcutsBridge::Authorization;

  struct Event {
    std::string kind;
    uint32_t request = 0;
    uint32_t status = 0;
    std::string id;
    std::string description;
    std::string trigger;
  };

  struct ClientSession {
    umbriel_global_shortcuts_session_v1* proxy = nullptr;
    std::vector<Event> events;
    std::string token;

    ~ClientSession() { destroy(); }
    ClientSession() = default;
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void destroy() {
      if (proxy != nullptr) {
        umbriel_global_shortcuts_session_v1_destroy(std::exchange(proxy, nullptr));
      }
    }
    void add(const char* id = "talk", const char* trigger = "CTRL+m", const char* description = "Action") const {
      umbriel_global_shortcuts_session_v1_add_shortcut(proxy, id, description, trigger);
    }
    void bind(uint32_t request = 1) const { umbriel_global_shortcuts_session_v1_bind(proxy, request); }
    void list(uint32_t request = 2) const { umbriel_global_shortcuts_session_v1_list(proxy, request); }
    void restore(std::string_view credential, uint32_t request = 3) const {
      const std::string copy(credential);
      umbriel_global_shortcuts_session_v1_restore(proxy, request, copy.c_str());
    }
    void cancel(uint32_t request = 1) const { umbriel_global_shortcuts_session_v1_cancel(proxy, request); }
    size_t count(std::string_view kind) const {
      return static_cast<size_t>(std::ranges::count(events, kind, &Event::kind));
    }
    bool result(std::string_view kind, uint32_t request, uint32_t status) const {
      return std::ranges::any_of(events, [&](const auto& event) {
        return event.kind == kind && event.request == request && event.status == status;
      });
    }
    std::vector<std::string> kinds() const {
      std::vector<std::string> result;
      result.reserve(events.size());
      for (const auto& event : events) {
        result.push_back(event.kind);
      }
      return result;
    }
  };

  ClientSession& state(void* data) { return *static_cast<ClientSession*>(data); }

  constexpr umbriel_global_shortcuts_session_v1_listener kSessionListener{
      .grant =
          [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, const char* credential) {
            state(data).token = credential;
            state(data).events.push_back({"grant", request, 0, {}, {}, {}});
          },
      .restored = [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request,
                     uint32_t status) { state(data).events.push_back({"restored", request, status, {}, {}, {}}); },
      .shortcut =
          [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, const char* id,
             const char* description,
             const char* trigger) { state(data).events.push_back({"shortcut", request, 0, id, description, trigger}); },
      .bound = [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request,
                  uint32_t status) { state(data).events.push_back({"bound", request, status, {}, {}, {}}); },
      .listed = [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request,
                   uint32_t status) { state(data).events.push_back({"listed", request, status, {}, {}, {}}); },
      .activated = [](void* data, umbriel_global_shortcuts_session_v1*, const char* id,
                      uint32_t time) { state(data).events.push_back({"activated", time, 0, id, {}, {}}); },
      .deactivated = [](void* data, umbriel_global_shortcuts_session_v1*, const char* id,
                        uint32_t time) { state(data).events.push_back({"deactivated", time, 0, id, {}, {}}); },
      .changed = [](void* data, umbriel_global_shortcuts_session_v1*, const char* id, const char* description,
                    const char* trigger) { state(data).events.push_back({"changed", 0, 0, id, description, trigger}); },
      .changes_done =
          [](void* data, umbriel_global_shortcuts_session_v1*) {
            state(data).events.push_back({"changes-done", 0, 0, {}, {}, {}});
          },
      .closed =
          [](void* data, umbriel_global_shortcuts_session_v1*) {
            state(data).events.push_back({"closed", 0, 0, {}, {}, {}});
          },
  };

  class Peer {
  public:
    explicit Peer(wl_display* server) : m_server(server) {
      std::array<int, 2> sockets{};
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        throw std::runtime_error("could not create a private Wayland connection");
      }
      if (wl_client_create(server, sockets[0]) == nullptr) {
        close(sockets[0]);
        close(sockets[1]);
        throw std::runtime_error("could not create the bridge test peer");
      }
      m_display = wl_display_connect_to_fd(sockets[1]);
      if (m_display == nullptr) {
        throw std::runtime_error("could not connect the bridge test peer");
      }
      m_registry = wl_display_get_registry(m_display);
      wl_registry_add_listener(m_registry, &kRegistryListener, this);
      sync();
      if (m_managers.empty()) {
        throw std::runtime_error("global shortcuts bridge was not advertised");
      }
      sync();
    }

    ~Peer() {
      m_sessions.clear();
      for (auto* manager : m_managers) {
        umbriel_global_shortcuts_manager_v1_destroy(manager);
      }
      wl_registry_destroy(m_registry);
      wl_display_disconnect(m_display);
    }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    void sync() const {
      bool done = false;
      wl_callback* callback = wl_display_sync(m_display);
      constexpr wl_callback_listener listener{
          .done = [](void* data, wl_callback*, uint32_t) { *static_cast<bool*>(data) = true; },
      };
      wl_callback_add_listener(callback, &listener, &done);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!done && std::chrono::steady_clock::now() < deadline) {
        if (wl_display_flush(m_display) < 0 && errno != EAGAIN) {
          break;
        }
        if (wl_event_loop_dispatch(wl_display_get_event_loop(m_server), 0) < 0) {
          break;
        }
        wl_display_flush_clients(m_server);
        pollfd descriptor{.fd = wl_display_get_fd(m_display), .events = POLLIN, .revents = 0};
        if (poll(&descriptor, 1, 1) > 0 && wl_display_dispatch(m_display) < 0) {
          break;
        }
      }
      wl_callback_destroy(callback);
      if (!done) {
        throw std::runtime_error("bridge test synchronization failed");
      }
    }

    ClientSession& create(const char* application = "org.test.App", size_t manager = 0) {
      auto session = std::make_unique<ClientSession>();
      session->proxy = umbriel_global_shortcuts_manager_v1_create_session(m_managers.at(manager), application);
      umbriel_global_shortcuts_session_v1_add_listener(session->proxy, &kSessionListener, session.get());
      auto& result = *session;
      m_sessions.push_back(std::move(session));
      return result;
    }

    void anotherManager() {
      m_managers.push_back(
          static_cast<umbriel_global_shortcuts_manager_v1*>(
              wl_registry_bind(m_registry, m_name, &umbriel_global_shortcuts_manager_v1_interface, 1)
          )
      );
    }

    void destroyManager() {
      umbriel_global_shortcuts_manager_v1_destroy(m_managers.back());
      m_managers.pop_back();
    }

  private:
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto& peer = *static_cast<Peer*>(data);
      if (std::strcmp(interface, umbriel_global_shortcuts_manager_v1_interface.name) == 0) {
        CHECK_EQ(version, uint32_t{1});
        peer.m_name = name;
        peer.m_managers.push_back(
            static_cast<umbriel_global_shortcuts_manager_v1*>(
                wl_registry_bind(registry, name, &umbriel_global_shortcuts_manager_v1_interface, 1)
            )
        );
      }
    }
    static constexpr wl_registry_listener kRegistryListener{
        .global = global,
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };

    wl_display* m_server;
    wl_display* m_display = nullptr;
    wl_registry* m_registry = nullptr;
    uint32_t m_name = 0;
    std::vector<umbriel_global_shortcuts_manager_v1*> m_managers;
    std::vector<std::unique_ptr<ClientSession>> m_sessions;
  };

  struct Approval {
    GlobalShortcutSessionId id;
    std::string application;
    GlobalShortcutBind bind;
  };

  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      std::strcpy(pattern.data(), "/tmp/umbriel-shortcuts-bridge.XXXXXX");
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create the private bridge test store");
      }
      directory = pattern.data();
      display = wl_display_create();
      if (display == nullptr) {
        throw std::runtime_error("could not create the bridge test display");
      }
      store = std::make_unique<GlobalShortcutsStore>(directory / "state/shortcuts.json");
      bridge = std::make_unique<GlobalShortcutsBridge>(display, registry, *store);
      bridge->setAuthorizationCancelled([this](auto id) { cancelled.push_back(id); });
    }
    ~Fixture() {
      peers.clear();
      wl_display_destroy_clients(display);
      bridge.reset();
      wl_display_destroy(display);
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Peer& peer() {
      peers.push_back(std::make_unique<Peer>(display));
      return *peers.back();
    }
    void acceptRequests() {
      bridge->setAuthorize([this](auto id, auto application, const auto& bind) {
        approvals.push_back({id, std::string(application), bind});
      });
    }
    void approveLast() { bridge->authorizeSession(approvals.back().id, Authorization::Approved); }
    GlobalShortcutCredential seed(const char* application = "org.test.App") {
      const GlobalShortcut shortcut{"talk", "Remembered", parseGlobalShortcutTrigger("CTRL+m")};
      const auto credential = store->approve(application, std::array{shortcut}).value();
      if (!store->recordBindings(credential.grant.id, application, std::array{shortcut})) {
        throw std::runtime_error("could not seed successful shortcut history");
      }
      return credential;
    }

    std::filesystem::path directory;
    wl_display* display = nullptr;
    GlobalShortcutsRegistry registry;
    std::unique_ptr<GlobalShortcutsStore> store;
    std::unique_ptr<GlobalShortcutsBridge> bridge;
    std::vector<Approval> approvals;
    std::vector<GlobalShortcutSessionId> cancelled;
    std::vector<std::unique_ptr<Peer>> peers;
  };
} // namespace

UMBRIEL_TEST(unapprovedClaimsRequireAnApprovalHandlerAndEmptyBindsSucceed) {
  Fixture fixture;
  auto& peer = fixture.peer();
  auto& session = peer.create();
  session.list();
  session.add();
  session.bind();
  peer.sync();
  CHECK(session.result("listed", 2, 0));
  CHECK(session.result("bound", 1, 2));
  CHECK(session.token.empty());
  CHECK(!std::filesystem::exists(fixture.directory / "state/shortcuts.json"));
  auto& empty = peer.create();
  empty.bind();
  peer.sync();
  CHECK(empty.result("bound", 1, 0));
  CHECK_EQ(empty.count("shortcut"), size_t{0});
}

UMBRIEL_TEST(approvalAndRestorePublishCredentialsBeforeSuccessAndRememberOnlyBoundShortcuts) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& first = peer.create();
  first.add();
  first.add("typing", "SHIFT+a");
  first.bind();
  peer.sync();
  CHECK(first.events.empty());
  CHECK_EQ(fixture.approvals.size(), size_t{1});
  CHECK_EQ(fixture.approvals.back().bind.approval.size(), size_t{2});
  fixture.approveLast();
  peer.sync();
  CHECK(first.kinds() == std::vector<std::string>({"grant", "shortcut", "bound"}));
  CHECK(first.result("bound", 1, 0));
  CHECK_EQ(first.events.at(1).trigger, "Ctrl+M");
  const auto token = first.token;
  first.destroy();
  auto& next = peer.create();
  next.restore(token);
  next.list();
  peer.sync();
  CHECK(next.kinds() == std::vector<std::string>({"grant", "restored", "shortcut", "listed"}));
  CHECK(next.result("restored", 3, 0));
  CHECK(next.token != token);
  CHECK_EQ(next.events.at(2).description, "Action");
  next.events.clear();
  next.add("talk", "CTRL+n", "Renamed");
  next.add("typing", "F16");
  next.bind();
  peer.sync();
  CHECK(next.kinds() == std::vector<std::string>({"shortcut", "bound"}));
  CHECK_EQ(next.events.front().trigger, "Ctrl+M");
  CHECK_EQ(next.events.front().description, "Renamed");
  CHECK_EQ(fixture.approvals.size(), size_t{1});
}

UMBRIEL_TEST(restoreOrderingAndApplicationChecksDoNotConsumeUnusableCredentials) {
  Fixture fixture;
  fixture.acceptRequests();
  const auto credential = fixture.seed();
  auto& peer = fixture.peer();
  auto& listed = peer.create();
  listed.list();
  listed.restore(credential.token);
  auto& wrong = peer.create("org.test.Other");
  wrong.restore(credential.token);
  auto& repeated = peer.create();
  repeated.restore("invalid");
  repeated.restore(credential.token, 4);
  auto& bound = peer.create();
  bound.bind();
  bound.restore(credential.token);
  peer.sync();
  CHECK(listed.result("restored", 3, 2));
  CHECK(wrong.result("restored", 3, 2));
  CHECK(repeated.result("restored", 3, 2));
  CHECK(repeated.result("restored", 4, 2));
  CHECK(bound.result("restored", 3, 2));
  auto& valid = peer.create();
  valid.restore(credential.token);
  peer.sync();
  CHECK(valid.result("restored", 3, 0));
  auto& replay = peer.create();
  replay.restore(credential.token);
  replay.add();
  replay.bind();
  peer.sync();
  CHECK(replay.result("restored", 3, 2));
  CHECK_EQ(fixture.approvals.size(), size_t{1});
  CHECK(replay.count("bound") == 0);
}

UMBRIEL_TEST(newShortcutApprovalExtendsOnlyTheRestoredGrantAndReplacesItsCredential) {
  Fixture fixture;
  fixture.acceptRequests();
  const auto credential = fixture.seed();
  auto& peer = fixture.peer();
  auto& session = peer.create();
  session.restore(credential.token);
  peer.sync();
  CHECK(session.result("restored", 3, 0));
  const auto previousToken = session.token;
  session.events.clear();
  session.add("talk", "CTRL+n");
  session.add("new", "F16");
  session.bind();
  peer.sync();
  CHECK_EQ(fixture.approvals.size(), size_t{1});
  CHECK_EQ(fixture.approvals.back().bind.approval.size(), size_t{1});
  CHECK_EQ(fixture.approvals.back().bind.approval.front().id, "new");
  fixture.approveLast();
  peer.sync();
  CHECK(session.kinds() == std::vector<std::string>({"grant", "shortcut", "shortcut", "bound"}));
  CHECK(session.result("bound", 1, 0));
  CHECK(session.token != previousToken);
  CHECK(!fixture.store->restore(previousToken, "org.test.App"));
  const auto metadata = fixture.store->list().value();
  CHECK_EQ(metadata.size(), size_t{1});
  CHECK_EQ(metadata.front().id, credential.grant.id);
  CHECK_EQ(metadata.front().approved.size(), size_t{2});
  CHECK_EQ(metadata.front().approved.front().trigger, parseGlobalShortcutTrigger("CTRL+m"));
}

UMBRIEL_TEST(invalidStagingFailsOnceWithoutAffectingOtherSessions) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  const std::vector<GlobalShortcutRequest> invalid{
      {"", "Action", "F16"},
      {std::string(257, 'a'), "Action", "F16"},
      {"talk", std::string(1025, 'a'), "F16"},
      {"talk", "Action", std::string(257, 'a')},
      {"talk", std::string(1, '\xff'), "F16"},
  };
  for (const auto& entry : invalid) {
    auto& session = peer.create();
    session.add(entry.id.c_str(), entry.preferredTrigger.c_str(), entry.description.c_str());
    session.add("valid", "F17");
    session.bind();
    session.bind(4);
    peer.sync();
    CHECK(session.result("bound", 1, 2));
    CHECK(session.result("bound", 4, 2));
    CHECK_EQ(session.count("bound"), size_t{2});
    session.destroy();
  }
  auto& duplicate = peer.create();
  duplicate.add();
  duplicate.add();
  duplicate.bind();
  auto& overflow = peer.create();
  for (size_t index = 0; index <= kMaxGlobalShortcuts; ++index) {
    const auto id = std::format("action-{}", index);
    overflow.add(id.c_str(), "F16");
  }
  overflow.bind();
  auto& boundary = peer.create();
  for (size_t index = 0; index < kMaxGlobalShortcuts; ++index) {
    const auto id = std::format("action-{}", index);
    boundary.add(id.c_str(), "F16");
  }
  boundary.bind();
  peer.sync();
  CHECK(duplicate.result("bound", 1, 2));
  CHECK(overflow.result("bound", 1, 2));
  CHECK(boundary.events.empty());
  CHECK_EQ(fixture.approvals.size(), size_t{1});
  CHECK_EQ(fixture.approvals.back().bind.approval.size(), kMaxGlobalShortcuts);
}

UMBRIEL_TEST(sessionLimitsAreRecoverableAcrossManagersAndAcrossClients) {
  Fixture fixture;
  auto& first = fixture.peer();
  first.anotherManager();
  auto& invalid = first.create("");
  auto& longId = first.create(std::string(257, 'a').c_str());
  auto& invalidUtf8 = first.create("\xff");
  for (int index = 0; index < 16; ++index) {
    first.create("org.test.App", static_cast<size_t>(index % 2));
  }
  auto& rejected = first.create();
  rejected.list();
  rejected.bind();
  first.sync();
  CHECK_EQ(invalid.count("closed"), size_t{1});
  CHECK_EQ(longId.count("closed"), size_t{1});
  CHECK_EQ(invalidUtf8.count("closed"), size_t{1});
  CHECK_EQ(rejected.count("closed"), size_t{1});
  CHECK(rejected.result("listed", 2, 2));
  CHECK(rejected.result("bound", 1, 2));
  for (int client = 0; client < 3; ++client) {
    auto& peer = fixture.peer();
    for (int index = 0; index < 16; ++index) {
      auto& session = peer.create();
      session.list();
    }
    peer.sync();
  }
  auto& extraPeer = fixture.peer();
  auto& full = extraPeer.create();
  full.list();
  extraPeer.sync();
  CHECK_EQ(full.count("closed"), size_t{1});
  fixture.peers.erase(fixture.peers.begin() + 1);
  first.sync();
  auto& available = extraPeer.create();
  available.list();
  extraPeer.sync();
  CHECK(available.result("listed", 2, 0));
  CHECK_EQ(available.count("closed"), size_t{0});
}

UMBRIEL_TEST(cancellationAndRepeatedBindsCannotCompleteAnApprovalTwice) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& session = peer.create();
  session.add();
  session.bind(0);
  session.cancel(20);
  session.bind(4);
  peer.sync();
  CHECK(session.result("bound", 4, 2));
  CHECK_EQ(session.count("bound"), size_t{1});
  session.cancel(0);
  session.cancel(0);
  peer.sync();
  CHECK(session.result("bound", 0, 1));
  CHECK_EQ(fixture.cancelled.size(), size_t{1});
  fixture.approveLast();
  peer.sync();
  CHECK_EQ(session.count("bound"), size_t{2});
  CHECK_EQ(session.count("grant"), size_t{0});
  CHECK(!std::filesystem::exists(fixture.directory / "state/shortcuts.json"));

  auto& duplicate = peer.create();
  duplicate.add();
  duplicate.bind();
  duplicate.list(1);
  peer.sync();
  CHECK(duplicate.result("bound", 1, 2));
  CHECK_EQ(duplicate.count("bound"), size_t{1});
  CHECK_EQ(duplicate.count("listed"), size_t{0});
  fixture.approveLast();
  peer.sync();
  CHECK_EQ(duplicate.count("grant"), size_t{0});
  CHECK_EQ(fixture.cancelled.size(), size_t{2});
}

UMBRIEL_TEST(approvalDecisionsDistinguishCancellationFromFailureAndAllowFreshSessions) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& declined = peer.create();
  declined.add();
  declined.bind();
  peer.sync();
  fixture.bridge->authorizeSession(fixture.approvals.back().id, Authorization::Cancelled);
  auto& failed = peer.create();
  failed.add();
  failed.bind();
  peer.sync();
  fixture.bridge->authorizeSession(fixture.approvals.back().id, Authorization::Failed);
  auto& retry = peer.create();
  retry.add();
  retry.bind();
  peer.sync();
  CHECK(declined.result("bound", 1, 1));
  CHECK(failed.result("bound", 1, 2));
  CHECK_EQ(fixture.approvals.size(), size_t{3});
  CHECK(!std::filesystem::exists(fixture.directory / "state/shortcuts.json"));
  fixture.approveLast();
  peer.sync();
  CHECK(retry.result("bound", 1, 0));
  CHECK_EQ(declined.count("grant"), size_t{0});
  CHECK_EQ(failed.count("grant"), size_t{0});
}

UMBRIEL_TEST(destroyAndDisconnectCancelPendingApprovalAndManagersDoNotOwnSessions) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& session = peer.create();
  session.add();
  session.bind();
  peer.sync();
  const auto id = fixture.approvals.back().id;
  peer.destroyManager();
  session.list();
  peer.sync();
  CHECK(fixture.cancelled.empty());
  CHECK(session.result("listed", 2, 0));
  session.destroy();
  peer.sync();
  CHECK(fixture.cancelled == std::vector<GlobalShortcutSessionId>{id});
  fixture.bridge->authorizeSession(id, Authorization::Approved);
  CHECK(!fixture.registry.session(id));
  auto& another = fixture.peer();
  auto& disconnected = another.create();
  disconnected.add();
  disconnected.bind();
  another.sync();
  const auto next = fixture.approvals.back().id;
  fixture.peers.pop_back();
  peer.sync();
  CHECK_EQ(fixture.cancelled.size(), size_t{2});
  CHECK(!fixture.registry.session(next));
  fixture.bridge->authorizeSession(next, Authorization::Approved);
  CHECK(!std::filesystem::exists(fixture.directory / "state/shortcuts.json"));
}

UMBRIEL_TEST(ownerClosureAndConfigChangesPublishCompleteSnapshotsAndPersistPromotions) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& owner = peer.create();
  owner.add();
  owner.bind();
  peer.sync();
  fixture.approveLast();
  peer.sync();
  auto& waiter = peer.create("org.test.Other");
  waiter.add();
  waiter.bind();
  peer.sync();
  fixture.approveLast();
  peer.sync();
  CHECK(waiter.result("bound", 1, 0));
  CHECK_EQ(waiter.count("shortcut"), size_t{0});
  waiter.events.clear();
  owner.destroy();
  peer.sync();
  CHECK(waiter.kinds() == std::vector<std::string>({"changed", "changes-done"}));
  CHECK_EQ(waiter.events.front().trigger, "Ctrl+M");
  const auto metadata = fixture.store->list().value();
  const auto saved = std::ranges::find(metadata, "org.test.Other", &GlobalShortcutGrant::application);
  CHECK(saved != metadata.end() && saved->previous.size() == 1);
  waiter.events.clear();
  CHECK(fixture.registry.configure({{"org.test.Other", {{"talk", ""}}}}, {}, 0));
  fixture.bridge->dispatchChanges();
  peer.sync();
  CHECK(waiter.kinds() == std::vector<std::string>{"changes-done"});
}

UMBRIEL_TEST(configChangesInvalidateApprovalBeforeAnyCredentialCanBeIssued) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& session = peer.create();
  session.add();
  session.bind();
  peer.sync();
  CHECK(fixture.registry.configure({}, {}, 0));
  fixture.approveLast();
  fixture.bridge->dispatchChanges();
  peer.sync();
  CHECK(session.result("bound", 1, 2));
  CHECK_EQ(session.count("bound"), size_t{1});
  CHECK_EQ(fixture.cancelled.size(), size_t{1});
  CHECK_EQ(session.count("grant"), size_t{0});
  CHECK(!std::filesystem::exists(fixture.directory / "state/shortcuts.json"));
}

UMBRIEL_TEST(persistenceFailureCannotReportSuccessfulApprovalOrLeaveALiveRegistration) {
  Fixture fixture;
  fixture.acceptRequests();
  const auto credential = fixture.seed();
  auto& peer = fixture.peer();
  auto& pending = peer.create();
  pending.add("new", "F16");
  pending.bind();
  peer.sync();
  {
    PrivateStateFileLock lock(fixture.directory / "state/shortcuts.json", ".global-shortcuts.lock");
    CHECK(static_cast<bool>(lock));
    fixture.approveLast();
    peer.sync();
  }
  CHECK(pending.result("bound", 1, 2));
  CHECK_EQ(pending.count("grant"), size_t{0});
  auto& restored = peer.create();
  restored.restore(credential.token);
  peer.sync();
  CHECK(restored.result("restored", 3, 0));
  restored.events.clear();
  {
    PrivateStateFileLock lock(fixture.directory / "state/shortcuts.json", ".global-shortcuts.lock");
    CHECK(static_cast<bool>(lock));
    restored.add();
    restored.bind();
    peer.sync();
  }
  CHECK(restored.kinds() == std::vector<std::string>({"bound", "closed"}));
  CHECK(restored.result("bound", 1, 2));
  const auto id = fixture.registry.createSession("org.test.Probe").value();
  const auto candidate = fixture.registry.beginBind(id, {{"talk", "Action", "CTRL+m"}}).value();
  CHECK_EQ(candidate.shortcuts.front().conflict, GlobalShortcutConflict::None);
}

UMBRIEL_TEST(revocationAndBridgeTeardownCloseSessionsWithoutDanglingResourceOwners) {
  Fixture fixture;
  fixture.acceptRequests();
  auto& peer = fixture.peer();
  auto& revoked = peer.create();
  revoked.add();
  revoked.bind();
  auto& other = peer.create("org.test.Other");
  other.add("other", "F16");
  other.bind();
  peer.sync();
  (void)fixture.registry.revokeApplication("org.test.App");
  fixture.bridge->dispatchChanges();
  peer.sync();
  CHECK(revoked.kinds() == std::vector<std::string>({"bound", "closed"}));
  CHECK(revoked.result("bound", 1, 2));
  CHECK_EQ(fixture.cancelled.size(), size_t{1});
  CHECK(other.events.empty());
  fixture.approveLast();
  peer.sync();
  CHECK(other.result("bound", 1, 0));
  other.events.clear();
  fixture.bridge.reset();
  other.list();
  other.bind(4);
  auto& inert = peer.create();
  inert.restore("invalid");
  peer.sync();
  CHECK(other.kinds() == std::vector<std::string>({"closed", "listed", "bound"}));
  CHECK(other.result("listed", 2, 2));
  CHECK(other.result("bound", 4, 2));
  CHECK_EQ(inert.count("closed"), size_t{1});
  CHECK(inert.result("restored", 3, 2));
}

int main() { return RUN_TESTS(); }
