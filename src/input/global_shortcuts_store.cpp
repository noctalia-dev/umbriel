#include "input/global_shortcuts_store.h"

#include "core/private_state_file.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace umbriel {
  namespace {
    constexpr std::string_view kStoreLockName = ".global-shortcuts.lock";
    constexpr size_t kMaxGrants = 128;
    constexpr size_t kMaxStoreBytes = 4 * 1024 * 1024;

    bool hexString(std::string_view value, size_t length) {
      return value.size() == length && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
             });
    }

    nlohmann::json encodeShortcut(const GlobalShortcut& shortcut) {
      nlohmann::json trigger = nullptr;
      if (shortcut.trigger) {
        trigger = {{"modifiers", shortcut.trigger->modifiers}, {"keysym", shortcut.trigger->keysym}};
      }
      return {{"id", shortcut.id}, {"description", shortcut.description}, {"trigger", std::move(trigger)}};
    }

    std::optional<GlobalShortcut> decodeShortcut(const nlohmann::json& value) {
      if (!value.is_object()
          || value.size() != 3
          || !value.contains("id")
          || !value["id"].is_string()
          || !value.contains("description")
          || !value["description"].is_string()
          || !value.contains("trigger")) {
        return std::nullopt;
      }
      GlobalShortcut shortcut{value["id"].get<std::string>(), value["description"].get<std::string>(), std::nullopt};
      const auto& trigger = value["trigger"];
      if (!trigger.is_null()) {
        if (!trigger.is_object()
            || trigger.size() != 2
            || !trigger.contains("modifiers")
            || !trigger["modifiers"].is_number_unsigned()
            || !trigger.contains("keysym")
            || !trigger["keysym"].is_number_unsigned()) {
          return std::nullopt;
        }
        const auto modifiers = trigger["modifiers"].get<uint64_t>();
        const auto keysym = trigger["keysym"].get<uint64_t>();
        if (modifiers > std::numeric_limits<uint32_t>::max() || keysym > std::numeric_limits<xkb_keysym_t>::max()) {
          return std::nullopt;
        }
        shortcut.trigger = GlobalShortcutTrigger{static_cast<uint32_t>(modifiers), static_cast<xkb_keysym_t>(keysym)};
      }
      return shortcut;
    }
  } // namespace

  GlobalShortcutsStore::GlobalShortcutsStore(std::filesystem::path path) : m_path(std::move(path)) {}

  std::filesystem::path GlobalShortcutsStore::defaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
      return std::filesystem::path(state) / "umbriel/global-shortcuts.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".local/state/umbriel/global-shortcuts.json";
    }
    return {};
  }

  std::optional<std::vector<GlobalShortcutsStore::Record>> GlobalShortcutsStore::load() const {
    const auto contents = readPrivateStateFile(m_path, kMaxStoreBytes);
    if (!contents) {
      return std::nullopt;
    }
    std::vector<Record> records;
    if (contents->missing) {
      return records;
    }
    const auto data = nlohmann::json::parse(contents->bytes, nullptr, false);
    if (!data.is_object()
        || data.size() != 2
        || !data.contains("version")
        || !data["version"].is_number_unsigned()
        || data["version"] != 1
        || !data.contains("grants")
        || !data["grants"].is_array()
        || data["grants"].size() > kMaxGrants) {
      return std::nullopt;
    }
    for (const auto& item : data["grants"]) {
      if (!item.is_object()
          || item.size() != 5
          || !item.contains("id")
          || !item["id"].is_string()
          || !item.contains("application")
          || !item["application"].is_string()
          || !item.contains("token")
          || !item["token"].is_string()
          || !item.contains("approved")
          || !item["approved"].is_array()
          || item["approved"].size() > kMaxGlobalShortcutGrantEntries
          || !item.contains("previous")
          || !item["previous"].is_array()
          || item["previous"].size() > kMaxGlobalShortcutGrantEntries) {
        return std::nullopt;
      }
      Record record;
      record.grant.id = item["id"].get<std::string>();
      record.grant.application = item["application"].get<std::string>();
      record.token = item["token"].get<std::string>();
      for (const auto* key : {"approved", "previous"}) {
        auto& shortcuts = std::string_view(key) == "approved" ? record.grant.approved : record.grant.previous;
        for (const auto& entry : item[key]) {
          auto shortcut = decodeShortcut(entry);
          if (!shortcut) {
            return std::nullopt;
          }
          shortcuts.push_back(std::move(*shortcut));
        }
      }
      if (!validGlobalShortcutGrant(record.grant)
          || !hexString(record.token, 64)
          || std::ranges::any_of(records, [&](const auto& existing) {
               return existing.grant.id == record.grant.id || existing.token == record.token;
             })) {
        return std::nullopt;
      }
      records.push_back(std::move(record));
    }
    return records;
  }

  bool GlobalShortcutsStore::save(const std::vector<Record>& records) const {
    if (records.size() > kMaxGrants) {
      return false;
    }
    auto data = nlohmann::json{{"version", 1}, {"grants", nlohmann::json::array()}};
    for (const auto& record : records) {
      if (!validGlobalShortcutGrant(record.grant) || !hexString(record.token, 64)) {
        return false;
      }
      auto approved = nlohmann::json::array();
      auto previous = nlohmann::json::array();
      for (const auto& shortcut : record.grant.approved) {
        approved.push_back(encodeShortcut(shortcut));
      }
      for (const auto& shortcut : record.grant.previous) {
        previous.push_back(encodeShortcut(shortcut));
      }
      data["grants"].push_back({
          {"id", record.grant.id},
          {"application", record.grant.application},
          {"token", record.token},
          {"approved", std::move(approved)},
          {"previous", std::move(previous)},
      });
    }
    std::string bytes;
    try {
      bytes = data.dump() + '\n';
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    return bytes.size() <= kMaxStoreBytes && writePrivateStateFile(m_path, bytes, ".global-shortcuts-");
  }

  std::optional<GlobalShortcutCredential> GlobalShortcutsStore::approve(
      std::string_view application, std::span<const GlobalShortcut> shortcuts, std::string_view grantId
  ) {
    if (!validGlobalShortcutId(application)
        || !validGlobalShortcutList(shortcuts)
        || (!grantId.empty() && !hexString(grantId, 32))) {
      return std::nullopt;
    }
    PrivateStateFileLock lock(m_path, kStoreLockName);
    if (!lock) {
      return std::nullopt;
    }
    auto records = load();
    if (!records) {
      return std::nullopt;
    }
    auto found = std::ranges::find_if(*records, [&](const auto& record) { return record.grant.id == grantId; });
    if (grantId.empty()) {
      if (records->size() >= kMaxGrants) {
        return std::nullopt;
      }
      const auto id = randomHex(16);
      if (!id || std::ranges::any_of(*records, [&](const auto& record) { return record.grant.id == *id; })) {
        return std::nullopt;
      }
      Record record;
      record.grant.id = *id;
      record.grant.application = application;
      records->push_back(std::move(record));
      found = std::prev(records->end());
    } else if (found == records->end() || found->grant.application != application) {
      return std::nullopt;
    }
    for (const auto& shortcut : shortcuts) {
      const auto previous = std::ranges::find(found->grant.approved, shortcut.id, &GlobalShortcut::id);
      if (previous == found->grant.approved.end()) {
        found->grant.approved.push_back(shortcut);
      } else {
        if (previous->trigger != shortcut.trigger) {
          return std::nullopt;
        }
        previous->description = shortcut.description;
      }
    }
    const auto token = randomHex(32);
    if (!token || std::ranges::any_of(*records, [&](const auto& record) { return record.token == *token; })) {
      return std::nullopt;
    }
    found->token = *token;
    if (!save(*records)) {
      return std::nullopt;
    }
    return GlobalShortcutCredential{found->grant, *token};
  }

  std::optional<GlobalShortcutCredential>
  GlobalShortcutsStore::restore(std::string_view token, std::string_view application) {
    if (!hexString(token, 64) || !validGlobalShortcutId(application)) {
      return std::nullopt;
    }
    PrivateStateFileLock lock(m_path, kStoreLockName);
    if (!lock) {
      return std::nullopt;
    }
    auto records = load();
    if (!records) {
      return std::nullopt;
    }
    const auto found = std::ranges::find(*records, token, &Record::token);
    if (found == records->end() || found->grant.application != application) {
      return std::nullopt;
    }
    const auto replacement = randomHex(32);
    if (!replacement
        || std::ranges::any_of(*records, [&](const auto& record) { return record.token == *replacement; })) {
      return std::nullopt;
    }
    found->token = *replacement;
    if (!save(*records)) {
      return std::nullopt;
    }
    return GlobalShortcutCredential{found->grant, *replacement};
  }

  bool GlobalShortcutsStore::recordBindings(
      std::string_view grantId, std::string_view application, std::span<const GlobalShortcut> shortcuts
  ) {
    if (!hexString(grantId, 32) || !validGlobalShortcutId(application) || !validGlobalShortcutList(shortcuts, true)) {
      return false;
    }
    PrivateStateFileLock lock(m_path, kStoreLockName);
    if (!lock) {
      return false;
    }
    auto records = load();
    if (!records) {
      return false;
    }
    const auto found = std::ranges::find_if(*records, [&](const auto& record) { return record.grant.id == grantId; });
    if (found == records->end() || found->grant.application != application) {
      return false;
    }
    for (const auto& shortcut : shortcuts) {
      if (std::ranges::find(found->grant.approved, shortcut.id, &GlobalShortcut::id) == found->grant.approved.end()) {
        return false;
      }
      const auto previous = std::ranges::find(found->grant.previous, shortcut.id, &GlobalShortcut::id);
      if (previous == found->grant.previous.end()) {
        found->grant.previous.push_back(shortcut);
      } else {
        *previous = shortcut;
      }
    }
    return save(*records);
  }

  std::optional<std::vector<GlobalShortcutGrant>> GlobalShortcutsStore::list() const {
    PrivateStateFileLock lock(m_path, kStoreLockName);
    if (!lock) {
      return std::nullopt;
    }
    const auto records = load();
    if (!records) {
      return std::nullopt;
    }
    std::vector<GlobalShortcutGrant> result;
    result.reserve(records->size());
    for (const auto& record : *records) {
      result.push_back(record.grant);
    }
    return result;
  }

  bool GlobalShortcutsStore::revoke(std::optional<std::string_view> application) {
    if (application && !validGlobalShortcutId(*application)) {
      return false;
    }
    PrivateStateFileLock lock(m_path, kStoreLockName);
    if (!lock) {
      return false;
    }
    auto records = load();
    if (!records) {
      if (application) {
        return false;
      }
      records.emplace();
    }
    std::erase_if(*records, [&](const auto& record) {
      return !application || record.grant.application == *application;
    });
    return save(*records);
  }

  bool GlobalShortcutsStore::revokeApplication(std::string_view application) { return revoke(application); }
  bool GlobalShortcutsStore::revokeAll() { return revoke(std::nullopt); }

} // namespace umbriel
