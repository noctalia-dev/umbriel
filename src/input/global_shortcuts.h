#pragma once

#include "input/global_shortcut_trigger.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  inline constexpr size_t kMaxGlobalShortcuts = 128;
  inline constexpr size_t kMaxGlobalShortcutGrantEntries = 512;
  inline constexpr size_t kMaxGlobalShortcutSessions = 64;

  struct GlobalShortcut {
    std::string id;
    std::string description;
    std::optional<GlobalShortcutTrigger> trigger;
    bool operator==(const GlobalShortcut&) const = default;
  };

  struct GlobalShortcutGrant {
    std::string id;
    std::string application;
    std::vector<GlobalShortcut> approved;
    std::vector<GlobalShortcut> previous;
    bool operator==(const GlobalShortcutGrant&) const = default;
  };

  bool validGlobalShortcutId(std::string_view value);
  // An empty override disables the shortcut and is handled separately by callers.
  std::optional<GlobalShortcutTrigger> parseGlobalShortcutOverride(std::string_view chord, uint32_t modKey);
  bool validGlobalShortcutList(
      std::span<const GlobalShortcut> shortcuts, bool boundOnly = false, size_t maxCount = kMaxGlobalShortcuts
  );
  bool validGlobalShortcutGrant(const GlobalShortcutGrant& grant);

  struct GlobalShortcutRequest {
    std::string id;
    std::string description;
    std::string preferredTrigger;
  };

  bool validGlobalShortcutRequest(const GlobalShortcutRequest& shortcut);

  enum class GlobalShortcutSource : uint8_t { None, Config, Stored, Preferred };
  enum class GlobalShortcutConflict : uint8_t { None, Unassigned, Disabled, Reserved, InUse };

  struct GlobalShortcutRegistration {
    std::string id;
    std::string description;
    std::optional<GlobalShortcutTrigger> candidate;
    std::optional<GlobalShortcutTrigger> trigger;
    GlobalShortcutSource source = GlobalShortcutSource::None;
    GlobalShortcutConflict conflict = GlobalShortcutConflict::Unassigned;
  };

  using GlobalShortcutSessionId = uint64_t;
  using GlobalShortcutOverrides = std::map<std::string, std::map<std::string, std::string>>;

  struct GlobalShortcutBinding {
    GlobalShortcutSessionId session;
    std::string id;
    GlobalShortcutTrigger trigger;
    bool operator==(const GlobalShortcutBinding&) const = default;
  };

  struct GlobalShortcutSession {
    GlobalShortcutSessionId id = 0;
    std::string application;
    std::string grantId;
    bool bindAttempted = false;
    bool pending = false;
    bool bound = false;
    std::vector<GlobalShortcutRegistration> shortcuts;
  };

  struct GlobalShortcutBind {
    std::vector<GlobalShortcutRegistration> shortcuts;
    std::vector<GlobalShortcut> approval;
  };

  struct GlobalShortcutChanges {
    std::vector<GlobalShortcutSessionId> changed;
    std::vector<GlobalShortcutSessionId> cancelled;
  };

  struct GlobalShortcutInspection {
    std::optional<GlobalShortcutSessionId> session;
    bool pending = false;
    GlobalShortcutRegistration shortcut;
  };

  struct GlobalShortcutApplication {
    std::string application;
    bool live = false;
    std::vector<GlobalShortcutInspection> shortcuts;
  };

  class GlobalShortcutsRegistry {
  public:
    [[nodiscard]] std::optional<GlobalShortcutSessionId> createSession(std::string_view application);
    bool canRestore(GlobalShortcutSessionId id) const;
    // Grant snapshots must come from a successful store restore or physical approval.
    [[nodiscard]] bool restore(GlobalShortcutSessionId id, std::optional<GlobalShortcutGrant> grant);
    [[nodiscard]] std::optional<GlobalShortcutBind>
    beginBind(GlobalShortcutSessionId id, std::vector<GlobalShortcutRequest> shortcuts);
    [[nodiscard]] bool commitBind(GlobalShortcutSessionId id, std::optional<GlobalShortcutGrant> grant = std::nullopt);
    [[nodiscard]] bool cancelBind(GlobalShortcutSessionId id);
    [[nodiscard]] bool closeSession(GlobalShortcutSessionId id);
    [[nodiscard]] std::vector<GlobalShortcutSessionId> revokeApplication(std::string_view application);
    [[nodiscard]] std::vector<GlobalShortcutSessionId> revokeAll();

    [[nodiscard]] bool configure(
        const GlobalShortcutOverrides& overrides, std::span<const GlobalShortcutTrigger> reserved, uint32_t modKey
    );
    std::optional<GlobalShortcutSession> session(GlobalShortcutSessionId id) const;
    std::optional<GlobalShortcutBinding>
    match(std::span<const xkb_keysym_t> keysyms, xkb_keysym_t rawKeysym, uint32_t modifiers) const;
    bool owns(const GlobalShortcutBinding& binding) const;
    // Listing closes the restore phase, including an empty result.
    std::optional<std::vector<GlobalShortcut>> list(GlobalShortcutSessionId id);
    GlobalShortcutChanges takeChanges();
    std::vector<GlobalShortcutApplication> inspect(std::span<const GlobalShortcutGrant> grants) const;

  private:
    struct Session {
      GlobalShortcutSession state;
      std::optional<GlobalShortcutGrant> grant;
      std::vector<GlobalShortcutRequest> requests;
      uint64_t order = 0;
      bool restoreAttempted = false;
      bool listed = false;
    };

    GlobalShortcutRegistration resolve(const Session& session, const GlobalShortcutRequest& request) const;
    bool reserved(const GlobalShortcutTrigger& trigger) const;
    void reconcile(GlobalShortcutSessionId initial = 0);
    void forgetChanges(GlobalShortcutSessionId id);

    std::map<GlobalShortcutSessionId, Session> m_sessions;
    std::map<std::string, std::map<std::string, std::optional<GlobalShortcutTrigger>>> m_overrides;
    std::vector<GlobalShortcutTrigger> m_reserved;
    GlobalShortcutChanges m_changes;
    GlobalShortcutSessionId m_nextSession = 0;
    uint64_t m_nextOrder = 0;
  };

} // namespace umbriel
