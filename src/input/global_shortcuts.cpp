#include "input/global_shortcuts.h"

#include "config/keybind_parse.h"
#include "input/shortcut_keysym.h"

#include <cmath>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace umbriel {
  namespace {
    constexpr uint32_t kTriggerModifiers =
        WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT | WLR_MODIFIER_LOGO;

    bool validText(std::string_view value, size_t maxBytes) {
      if (value.size() > maxBytes || value.contains('\0')) {
        return false;
      }
      try {
        nlohmann::json(value).dump();
      } catch (const nlohmann::json::exception&) {
        return false;
      }
      return true;
    }

    bool validTrigger(const GlobalShortcutTrigger& trigger) {
      std::array<char, 128> name{};
      return trigger.keysym != XKB_KEY_NoSymbol
          && (trigger.modifiers & ~kTriggerModifiers) == 0
          && trigger.keysym == xkb_keysym_to_lower(trigger.keysym)
          && (trigger.keysym < XKB_KEY_Shift_L || trigger.keysym > XKB_KEY_Hyper_R)
          && xkb_keysym_get_name(trigger.keysym, name.data(), name.size()) > 0;
    }

    const GlobalShortcut* approvedShortcut(const std::optional<GlobalShortcutGrant>& grant, std::string_view id) {
      if (grant) {
        const auto found = std::ranges::find(grant->approved, id, &GlobalShortcut::id);
        if (found != grant->approved.end()) {
          return &*found;
        }
      }
      return nullptr;
    }

    std::vector<GlobalShortcut> bindings(const GlobalShortcutSession& session) {
      std::vector<GlobalShortcut> result;
      result.reserve(session.shortcuts.size());
      for (const auto& shortcut : session.shortcuts) {
        if (shortcut.trigger) {
          result.push_back({shortcut.id, shortcut.description, shortcut.trigger});
        }
      }
      return result;
    }

    void appendUnique(std::vector<GlobalShortcutSessionId>& ids, GlobalShortcutSessionId id) {
      if (std::ranges::find(ids, id) == ids.end()) {
        ids.push_back(id);
      }
    }
  } // namespace

  bool validGlobalShortcutId(std::string_view value) { return !value.empty() && validText(value, 256); }

  std::optional<GlobalShortcutTrigger> parseGlobalShortcutOverride(std::string_view chord, uint32_t modKey) {
    Keybind bind;
    if (!validText(chord, 256)
        || (modKey & ~kTriggerModifiers) != 0
        || !parseChord(chord, bind)
        || bind.modifierOnly
        || bind.mouseButton != 0
        || bind.wheel != WheelDirection::None
        || !bind.submap.empty()
        || (bind.useMod && modKey == 0)) {
      return std::nullopt;
    }
    const GlobalShortcutTrigger trigger{bind.modifiers | (bind.useMod ? modKey : 0), bind.keysym};
    return validTrigger(trigger) ? std::optional{trigger} : std::nullopt;
  }

  bool validGlobalShortcutRequest(const GlobalShortcutRequest& shortcut) {
    return validGlobalShortcutId(shortcut.id)
        && validText(shortcut.description, 1024)
        && validText(shortcut.preferredTrigger, 256);
  }

  bool validGlobalShortcutList(std::span<const GlobalShortcut> shortcuts, bool boundOnly, size_t maxCount) {
    if (shortcuts.size() > maxCount) {
      return false;
    }
    std::set<std::string_view> ids;
    for (const auto& shortcut : shortcuts) {
      if (!validGlobalShortcutId(shortcut.id)
          || !validText(shortcut.description, 1024)
          || (boundOnly && !shortcut.trigger)
          || (shortcut.trigger && !validTrigger(*shortcut.trigger))
          || !ids.insert(shortcut.id).second) {
        return false;
      }
    }
    return true;
  }

  bool validGlobalShortcutGrant(const GlobalShortcutGrant& grant) {
    if (grant.id.size() != 32
        || !std::ranges::all_of(
            grant.id,
            [](char character) {
              return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            }
        )
        || !validGlobalShortcutId(grant.application)
        || !validGlobalShortcutList(grant.approved, false, kMaxGlobalShortcutGrantEntries)
        || !validGlobalShortcutList(grant.previous, true, kMaxGlobalShortcutGrantEntries)) {
      return false;
    }
    return std::ranges::all_of(grant.previous, [&](const auto& previous) {
      return std::ranges::find(grant.approved, previous.id, &GlobalShortcut::id) != grant.approved.end();
    });
  }

  std::optional<GlobalShortcutSessionId> GlobalShortcutsRegistry::createSession(std::string_view application) {
    if (!validGlobalShortcutId(application)
        || m_sessions.size() >= kMaxGlobalShortcutSessions
        || m_nextSession == std::numeric_limits<GlobalShortcutSessionId>::max()) {
      return std::nullopt;
    }
    const auto id = ++m_nextSession;
    Session session;
    session.state.id = id;
    session.state.application = application;
    m_sessions.emplace(id, std::move(session));
    return id;
  }

  bool GlobalShortcutsRegistry::canRestore(GlobalShortcutSessionId id) const {
    const auto found = m_sessions.find(id);
    return found != m_sessions.end()
        && !found->second.restoreAttempted
        && !found->second.listed
        && !found->second.state.bindAttempted;
  }

  bool GlobalShortcutsRegistry::restore(GlobalShortcutSessionId id, std::optional<GlobalShortcutGrant> grant) {
    if (!canRestore(id)) {
      return false;
    }
    auto& session = m_sessions.at(id);
    session.restoreAttempted = true;
    if (!grant || !validGlobalShortcutGrant(*grant) || grant->application != session.state.application) {
      return false;
    }
    session.state.grantId = grant->id;
    session.grant = std::move(grant);
    return true;
  }

  GlobalShortcutRegistration
  GlobalShortcutsRegistry::resolve(const Session& session, const GlobalShortcutRequest& request) const {
    GlobalShortcutRegistration result;
    result.id = request.id;
    result.description = request.description;
    if (const auto application = m_overrides.find(session.state.application); application != m_overrides.end()) {
      if (const auto configured = application->second.find(request.id); configured != application->second.end()) {
        result.candidate = configured->second;
        result.source = GlobalShortcutSource::Config;
        result.conflict = result.candidate ? GlobalShortcutConflict::None : GlobalShortcutConflict::Disabled;
        return result;
      }
    }
    if (const auto* stored = approvedShortcut(session.grant, request.id)) {
      result.candidate = stored->trigger;
      result.source = result.candidate ? GlobalShortcutSource::Stored : GlobalShortcutSource::None;
    } else if (
        const auto preferred = parseGlobalShortcutTrigger(request.preferredTrigger);
        preferred && preferredGlobalShortcutAllowed(*preferred)
    ) {
      result.candidate = preferred;
      result.source = GlobalShortcutSource::Preferred;
    }
    result.conflict = result.candidate ? GlobalShortcutConflict::None : GlobalShortcutConflict::Unassigned;
    return result;
  }

  bool GlobalShortcutsRegistry::reserved(const GlobalShortcutTrigger& trigger) const {
    const uint32_t escapeModifiers = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
    return (trigger.keysym >= XKB_KEY_XF86Switch_VT_1 && trigger.keysym <= XKB_KEY_XF86Switch_VT_12)
        || ((trigger.modifiers & escapeModifiers) == escapeModifiers
            && (trigger.keysym == XKB_KEY_Escape || (trigger.keysym >= XKB_KEY_F1 && trigger.keysym <= XKB_KEY_F12)))
        || std::ranges::find(m_reserved, trigger) != m_reserved.end();
  }

  std::optional<GlobalShortcutBind>
  GlobalShortcutsRegistry::beginBind(GlobalShortcutSessionId id, std::vector<GlobalShortcutRequest> shortcuts) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end() || found->second.state.bindAttempted) {
      return std::nullopt;
    }
    auto& session = found->second;
    session.state.bindAttempted = true;
    if (shortcuts.size() > kMaxGlobalShortcuts) {
      return std::nullopt;
    }
    std::set<std::string_view> ids;
    for (const auto& shortcut : shortcuts) {
      if (!validGlobalShortcutRequest(shortcut) || !ids.insert(shortcut.id).second) {
        return std::nullopt;
      }
    }
    std::set<std::pair<uint32_t, xkb_keysym_t>> claimed;
    for (const auto& [otherId, other] : m_sessions) {
      (void)otherId;
      for (const auto& shortcut : other.state.shortcuts) {
        if (shortcut.trigger) {
          claimed.emplace(shortcut.trigger->modifiers, shortcut.trigger->keysym);
        }
      }
    }
    GlobalShortcutBind pending;
    pending.shortcuts.reserve(shortcuts.size());
    pending.approval.reserve(shortcuts.size());
    for (const auto& request : shortcuts) {
      auto shortcut = resolve(session, request);
      if (!approvedShortcut(session.grant, request.id)) {
        pending.approval.push_back({shortcut.id, shortcut.description, shortcut.candidate});
      }
      if (shortcut.candidate) {
        if (reserved(*shortcut.candidate)) {
          shortcut.conflict = GlobalShortcutConflict::Reserved;
        } else if (!claimed.emplace(shortcut.candidate->modifiers, shortcut.candidate->keysym).second) {
          shortcut.conflict = GlobalShortcutConflict::InUse;
        }
      }
      pending.shortcuts.push_back(std::move(shortcut));
    }
    session.requests = std::move(shortcuts);
    session.state.shortcuts = pending.shortcuts;
    session.state.pending = true;
    return pending;
  }

  bool GlobalShortcutsRegistry::commitBind(GlobalShortcutSessionId id, std::optional<GlobalShortcutGrant> grant) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end()
        || !found->second.state.pending
        || m_nextOrder == std::numeric_limits<uint64_t>::max()) {
      return false;
    }
    auto& session = found->second;
    if (!grant) {
      grant = session.grant;
    }
    if (grant
        && (!validGlobalShortcutGrant(*grant)
            || grant->application != session.state.application
            || (session.grant && grant->id != session.grant->id))) {
      return false;
    }
    for (const auto& shortcut : session.state.shortcuts) {
      const auto* approved = approvedShortcut(grant, shortcut.id);
      const auto* previous = approvedShortcut(session.grant, shortcut.id);
      if (approved == nullptr || approved->trigger != (previous != nullptr ? previous->trigger : shortcut.candidate)) {
        return false;
      }
    }
    session.grant = std::move(grant);
    session.state.grantId = session.grant ? session.grant->id : std::string{};
    session.state.pending = false;
    session.state.bound = true;
    session.order = ++m_nextOrder;
    reconcile(id);
    return true;
  }

  bool GlobalShortcutsRegistry::cancelBind(GlobalShortcutSessionId id) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end() || !found->second.state.pending) {
      return false;
    }
    found->second.state.pending = false;
    found->second.state.shortcuts.clear();
    found->second.requests.clear();
    return true;
  }

  void GlobalShortcutsRegistry::forgetChanges(GlobalShortcutSessionId id) {
    std::erase(m_changes.changed, id);
    std::erase(m_changes.cancelled, id);
  }

  bool GlobalShortcutsRegistry::closeSession(GlobalShortcutSessionId id) {
    if (m_sessions.erase(id) == 0) {
      return false;
    }
    forgetChanges(id);
    reconcile();
    return true;
  }

  std::vector<GlobalShortcutSessionId> GlobalShortcutsRegistry::revokeApplication(std::string_view application) {
    std::vector<GlobalShortcutSessionId> removed;
    std::erase_if(m_sessions, [&](const auto& entry) {
      if (entry.second.state.application != application) {
        return false;
      }
      removed.push_back(entry.first);
      forgetChanges(entry.first);
      return true;
    });
    reconcile();
    return removed;
  }

  std::vector<GlobalShortcutSessionId> GlobalShortcutsRegistry::revokeAll() {
    std::vector<GlobalShortcutSessionId> removed;
    for (const auto& [id, session] : m_sessions) {
      (void)session;
      removed.push_back(id);
    }
    m_sessions.clear();
    m_changes = {};
    return removed;
  }

  bool GlobalShortcutsRegistry::configure(
      const GlobalShortcutOverrides& overrides, std::span<const GlobalShortcutTrigger> reserved, uint32_t modKey
  ) {
    if ((modKey & ~kTriggerModifiers) != 0) {
      return false;
    }
    decltype(m_overrides) parsed;
    for (const auto& [application, shortcuts] : overrides) {
      if (!validGlobalShortcutId(application)) {
        return false;
      }
      for (const auto& [id, chord] : shortcuts) {
        if (!validGlobalShortcutId(id)) {
          return false;
        }
        if (chord.empty()) {
          parsed[application][id] = std::nullopt;
          continue;
        }
        const auto trigger = parseGlobalShortcutOverride(chord, modKey);
        if (!trigger) {
          return false;
        }
        parsed[application][id] = *trigger;
      }
    }
    std::vector<GlobalShortcutTrigger> reservedTriggers;
    reservedTriggers.reserve(reserved.size());
    for (auto trigger : reserved) {
      trigger.modifiers &= ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
      trigger.keysym = xkb_keysym_to_lower(trigger.keysym);
      if (!validTrigger(trigger)) {
        return false;
      }
      reservedTriggers.push_back(trigger);
    }
    m_overrides = std::move(parsed);
    m_reserved = std::move(reservedTriggers);
    for (auto& [id, session] : m_sessions) {
      if (session.state.pending) {
        session.state.pending = false;
        session.state.shortcuts.clear();
        session.requests.clear();
        appendUnique(m_changes.cancelled, id);
      }
    }
    reconcile();
    return true;
  }

  void GlobalShortcutsRegistry::reconcile(GlobalShortcutSessionId initial) {
    std::vector<Session*> sessions;
    sessions.reserve(m_sessions.size());
    for (auto& [id, session] : m_sessions) {
      (void)id;
      if (session.state.bound) {
        sessions.push_back(&session);
      }
    }
    std::ranges::sort(sessions, {}, &Session::order);
    std::set<std::pair<uint32_t, xkb_keysym_t>> claimed;
    for (auto* session : sessions) {
      const auto previous = bindings(session->state);
      session->state.shortcuts.clear();
      for (const auto& request : session->requests) {
        auto shortcut = resolve(*session, request);
        if (shortcut.candidate) {
          if (reserved(*shortcut.candidate)) {
            shortcut.conflict = GlobalShortcutConflict::Reserved;
          } else if (!claimed.emplace(shortcut.candidate->modifiers, shortcut.candidate->keysym).second) {
            shortcut.conflict = GlobalShortcutConflict::InUse;
          } else {
            shortcut.trigger = shortcut.candidate;
          }
        }
        session->state.shortcuts.push_back(std::move(shortcut));
      }
      if (session->state.id != initial && bindings(session->state) != previous) {
        appendUnique(m_changes.changed, session->state.id);
      }
    }
  }

  std::optional<GlobalShortcutSession> GlobalShortcutsRegistry::session(GlobalShortcutSessionId id) const {
    const auto found = m_sessions.find(id);
    return found != m_sessions.end() ? std::optional{found->second.state} : std::nullopt;
  }

  std::vector<GlobalShortcutApplication>
  GlobalShortcutsRegistry::inspect(std::span<const GlobalShortcutGrant> grants) const {
    std::map<std::string, GlobalShortcutApplication> applications;
    std::set<std::pair<std::string, std::string>> represented;
    for (const auto& [id, session] : m_sessions) {
      auto& application = applications[session.state.application];
      application.application = session.state.application;
      application.live = true;
      for (const auto& shortcut : session.state.shortcuts) {
        application.shortcuts.push_back({id, session.state.pending, shortcut});
        represented.emplace(session.state.grantId, shortcut.id);
      }
    }
    for (const auto& grant : grants) {
      auto& application = applications[grant.application];
      application.application = grant.application;
      Session remembered;
      remembered.state.application = grant.application;
      remembered.grant = grant;
      for (const auto& approved : grant.approved) {
        if (represented.contains({grant.id, approved.id})) {
          continue;
        }
        const auto previous = std::ranges::find(grant.previous, approved.id, &GlobalShortcut::id);
        auto shortcut = resolve(
            remembered,
            {approved.id, previous != grant.previous.end() ? previous->description : approved.description, {}}
        );
        if (shortcut.candidate) {
          if (reserved(*shortcut.candidate)) {
            shortcut.conflict = GlobalShortcutConflict::Reserved;
          } else if (
              match(std::array{shortcut.candidate->keysym}, shortcut.candidate->keysym, shortcut.candidate->modifiers)
          ) {
            shortcut.conflict = GlobalShortcutConflict::InUse;
          }
        }
        application.shortcuts.push_back({std::nullopt, false, std::move(shortcut)});
      }
    }
    std::vector<GlobalShortcutApplication> result;
    result.reserve(applications.size());
    for (auto& [name, application] : applications) {
      (void)name;
      result.push_back(std::move(application));
    }
    return result;
  }

  std::optional<GlobalShortcutBinding> GlobalShortcutsRegistry::match(
      std::span<const xkb_keysym_t> keysyms, xkb_keysym_t rawKeysym, uint32_t modifiers
  ) const {
    const Session* first = nullptr;
    const GlobalShortcutRegistration* matched = nullptr;
    for (const auto& [id, session] : m_sessions) {
      (void)id;
      if (!session.state.bound || (first != nullptr && session.order >= first->order)) {
        continue;
      }
      for (const auto& shortcut : session.state.shortcuts) {
        if (shortcut.trigger && std::ranges::any_of(keysyms, [&](xkb_keysym_t keysym) {
              return matchesShortcutKeysym(
                  shortcut.trigger->modifiers, shortcut.trigger->keysym, modifiers, keysym, rawKeysym
              );
            })) {
          first = &session;
          matched = &shortcut;
          break;
        }
      }
    }
    if (matched == nullptr) {
      return std::nullopt;
    }
    return GlobalShortcutBinding{first->state.id, matched->id, *matched->trigger};
  }

  bool GlobalShortcutsRegistry::owns(const GlobalShortcutBinding& binding) const {
    const auto found = m_sessions.find(binding.session);
    return found != m_sessions.end()
        && found->second.state.bound
        && std::ranges::any_of(found->second.state.shortcuts, [&](const auto& shortcut) {
             return shortcut.id == binding.id && shortcut.trigger == binding.trigger;
           });
  }

  std::optional<std::vector<GlobalShortcut>> GlobalShortcutsRegistry::list(GlobalShortcutSessionId id) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end()) {
      return std::nullopt;
    }
    auto& session = found->second;
    session.listed = true;
    if (session.state.bindAttempted) {
      return bindings(session.state);
    }
    return session.grant ? session.grant->previous : std::vector<GlobalShortcut>{};
  }

  GlobalShortcutChanges GlobalShortcutsRegistry::takeChanges() { return std::exchange(m_changes, {}); }

} // namespace umbriel
