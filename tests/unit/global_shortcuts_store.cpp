#include "input/global_shortcuts_store.h"

#include "check.h"
#include "core/private_state_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace umbriel;

namespace {
  bool gRejectReplacement = false;
}

extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __wrap_renameat(int fromDirectory, const char* from, int toDirectory, const char* to) {
  if (gRejectReplacement) {
    errno = EIO;
    return -1;
  }
  return __real_renameat(fromDirectory, from, toDirectory, to);
}

namespace {
  GlobalShortcut shortcut(std::string id, std::string_view chord = {}) {
    const auto parsed = chord.empty() ? std::optional<GlobalShortcutTrigger>{} : parseGlobalShortcutTrigger(chord);
    if (!chord.empty() && !parsed) {
      throw std::runtime_error("invalid shortcut chord in test setup");
    }
    return {std::move(id), "Action", parsed};
  }

  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      constexpr std::string_view prefix = "/tmp/umbriel-global-shortcuts.XXXXXX";
      std::ranges::copy(prefix, pattern.begin());
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a private shortcut test directory");
      }
      directory = pattern.data();
      path = directory / "state/shortcuts.json";
    }
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(std::string_view bytes, mode_t mode = 0600) const {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      file.close();
      if (!file || chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("could not write the private shortcut test store");
      }
    }

    std::string read() const {
      std::ifstream file(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path directory;
    std::filesystem::path path;
  };

  GlobalShortcutCredential approve(GlobalShortcutsStore& store, std::string_view application = "org.test.App") {
    const auto credential = store.approve(application, std::array{shortcut("talk", "CTRL+m"), shortcut("unset")});
    if (!credential) {
      throw std::runtime_error("could not approve the test shortcut grant");
    }
    return *credential;
  }

  void rejectedStore(const Fixture& fixture, const GlobalShortcutCredential& credential) {
    const auto before = fixture.read();
    GlobalShortcutsStore store(fixture.path);
    CHECK(!store.restore(credential.token, credential.grant.application));
    CHECK(!store.approve("org.test.Other", std::array{shortcut("other", "F16")}));
    CHECK(!store.list());
    CHECK_EQ(fixture.read(), before);
  }
} // namespace

UMBRIEL_TEST(durableRestoreRotatesCredentialsAndRefreshesEveryStoreInstance) {
  Fixture fixture;
  GlobalShortcutsStore first(fixture.path);
  GlobalShortcutsStore stale(fixture.path);
  const auto original = approve(first);
  const auto other = approve(stale, "org.test.Other");
  CHECK_EQ(original.token.size(), size_t{64});
  CHECK(original.token != other.token);
  CHECK(original.grant.id != other.grant.id);
  CHECK(original.grant.previous.empty());
  CHECK(!stale.restore(original.token, "org.test.Other"));
  const auto rotated = stale.restore(original.token, "org.test.App");
  CHECK(rotated.has_value());
  if (!rotated) {
    return;
  }
  CHECK(rotated->token != original.token);
  CHECK_EQ(rotated->grant, original.grant);
  CHECK(!first.restore(original.token, "org.test.App"));
  GlobalShortcutsStore restarted(fixture.path);
  const auto again = restarted.restore(rotated->token, "org.test.App");
  CHECK(again.has_value());
  CHECK_EQ(first.list()->size(), size_t{2});
  CHECK(stale.restore(other.token, "org.test.Other").has_value());
}

UMBRIEL_TEST(physicalApprovalExtendsOnlyTheNamedGrantAndPreservesExistingPreferences) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto original = approve(store);
  const auto before = fixture.read();
  CHECK(!store.approve("org.test.Other", std::array{shortcut("new", "F16")}, original.grant.id));
  CHECK(!store.approve("org.test.App", std::array{shortcut("talk", "CTRL+n")}, original.grant.id));
  CHECK_EQ(fixture.read(), before);
  const auto extended = store.approve("org.test.App", std::array{shortcut("new", "F16")}, original.grant.id);
  CHECK(extended.has_value());
  if (!extended) {
    return;
  }
  CHECK_EQ(extended->grant.id, original.grant.id);
  CHECK_EQ(extended->grant.approved.size(), size_t{3});
  CHECK_EQ(extended->grant.approved.front().trigger, original.grant.approved.front().trigger);
  CHECK(!store.restore(original.token, "org.test.App"));
  CHECK(store.restore(extended->token, "org.test.App").has_value());
}

UMBRIEL_TEST(bindingHistoryCannotExpandScopeAndDoesNotReplaceApprovedCandidates) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto before = fixture.read();
  CHECK(!store.recordBindings(credential.grant.id, "org.test.App", std::array{shortcut("new", "F16")}));
  CHECK(!store.recordBindings(credential.grant.id, "org.test.Other", std::array{shortcut("talk", "F16")}));
  CHECK(!store.recordBindings(credential.grant.id, "org.test.App", std::array{shortcut("unset")}));
  CHECK_EQ(fixture.read(), before);
  const auto overridden = shortcut("talk", "F16");
  CHECK(store.recordBindings(credential.grant.id, "org.test.App", std::array{overridden}));
  CHECK(store.recordBindings(credential.grant.id, "org.test.App", std::array{shortcut("unset", "F17")}));
  CHECK(store.recordBindings(credential.grant.id, "org.test.App", {}));
  const auto listed = store.list();
  CHECK(listed && listed->size() == 1);
  if (!listed || listed->empty()) {
    return;
  }
  CHECK_EQ(listed->front().approved, credential.grant.approved);
  CHECK_EQ(listed->front().previous.size(), size_t{2});
  CHECK_EQ(listed->front().previous.front(), overridden);
  CHECK(store.restore(credential.token, "org.test.App").has_value());
}

UMBRIEL_TEST(registryAndStoreRestoreOnlyTheSubsetThatActuallyBound) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  GlobalShortcutsRegistry registry;
  const auto id = registry.createSession("org.test.App").value();
  const auto pending = registry.beginBind(id, {{"first", "First", "F16"}, {"second", "Second", "F16"}}).value();
  const auto credential = store.approve("org.test.App", pending.approval).value();
  CHECK(registry.commitBind(id, credential.grant));
  const auto bound = registry.list(id).value();
  CHECK_EQ(bound.size(), size_t{1});
  CHECK(store.recordBindings(credential.grant.id, "org.test.App", bound));
  CHECK(registry.closeSession(id));

  GlobalShortcutsStore restarted(fixture.path);
  GlobalShortcutsRegistry restored;
  const auto authorization = restarted.restore(credential.token, "org.test.App").value();
  const auto next = restored.createSession("org.test.App").value();
  CHECK(restored.restore(next, authorization.grant));
  CHECK(restored.list(next).value() == bound);
  CHECK(restored.configure({{"org.test.App", {{"first", "F17"}, {"second", ""}}}}, {}, 0));
  const auto again = restored.beginBind(next, {{"first", "New label", "F20"}, {"second", "Second", "F21"}}).value();
  CHECK(again.approval.empty());
  CHECK(restored.commitBind(next));
  const auto overridden = restored.list(next).value();
  CHECK(overridden.front().trigger == parseGlobalShortcutTrigger("F17"));
  CHECK(restarted.recordBindings(authorization.grant.id, "org.test.App", overridden));
  const auto metadata = restarted.list().value();
  CHECK_EQ(metadata.front().approved, credential.grant.approved);
  CHECK_EQ(metadata.front().previous, overridden);
}

UMBRIEL_TEST(storeTransactionsCannotBypassContentionOrConsumeCredentialsWhileBusy) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto before = fixture.read();
  {
    PrivateStateFileLock lock(fixture.path, ".global-shortcuts.lock");
    CHECK(static_cast<bool>(lock));
    CHECK(!store.restore(credential.token, "org.test.App"));
    CHECK(!store.approve("org.test.Other", {}));
    CHECK(!store.recordBindings(credential.grant.id, "org.test.App", {}));
    CHECK(!store.revokeApplication("org.test.App"));
    CHECK(!store.revokeAll());
    CHECK(!store.list());
  }
  CHECK_EQ(fixture.read(), before);
  CHECK(store.restore(credential.token, "org.test.App").has_value());
}

UMBRIEL_TEST(failedAtomicWritesLeaveTheLastCredentialAndHistoryUsable) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto before = fixture.read();
  gRejectReplacement = true;
  CHECK(!store.restore(credential.token, "org.test.App"));
  CHECK(!store.approve("org.test.App", std::array{shortcut("new", "F16")}, credential.grant.id));
  CHECK(!store.recordBindings(credential.grant.id, "org.test.App", std::array{shortcut("talk", "F16")}));
  CHECK(!store.revokeApplication("org.test.App"));
  gRejectReplacement = false;
  CHECK_EQ(fixture.read(), before);
  for (const auto& entry : std::filesystem::directory_iterator(fixture.path.parent_path())) {
    CHECK(!entry.path().filename().string().starts_with(".global-shortcuts-"));
  }
  CHECK(store.restore(credential.token, "org.test.App").has_value());
}

UMBRIEL_TEST(malformedRecordsNeverPartiallyRestoreOrOverwriteOtherGrants) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto valid = nlohmann::json::parse(fixture.read());
  std::vector<nlohmann::json> invalid;
  auto data = valid;
  data["version"] = 2;
  invalid.push_back(data);
  data = valid;
  data["grants"].push_back(data["grants"].front());
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["application"] = "";
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["approved"].push_back(data["grants"][0]["approved"].front());
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["approved"][0]["trigger"]["keysym"] = uint64_t{1} << 40U;
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["approved"][0]["trigger"]["modifiers"] = -1;
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["previous"] = data["grants"][0]["approved"];
  invalid.push_back(data);
  data = valid;
  data["grants"][0]["previous"] = nlohmann::json::array({data["grants"][0]["approved"][0]});
  data["grants"][0]["previous"][0]["id"] = "never-approved";
  invalid.push_back(data);
  for (const auto& document : invalid) {
    fixture.write(document.dump());
    rejectedStore(fixture, credential);
  }
  fixture.write("{");
  rejectedStore(fixture, credential);
}

UMBRIEL_TEST(privateStoreModesAndFileKindsCannotBeBypassedThroughRestoration) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto valid = fixture.read();
  struct stat info{};
  CHECK_EQ(stat(fixture.path.c_str(), &info), 0);
  CHECK_EQ(info.st_mode & 0777, 0600U);
  CHECK_EQ(stat(fixture.path.parent_path().c_str(), &info), 0);
  CHECK_EQ(info.st_mode & 0777, 0700U);
  fixture.write(valid, 0644);
  rejectedStore(fixture, credential);
  fixture.write(valid + std::string(4 * 1024 * 1024, ' '));
  rejectedStore(fixture, credential);
  fixture.write(valid);
  const auto target = fixture.directory / "target.json";
  std::filesystem::rename(fixture.path, target);
  std::filesystem::create_symlink(target, fixture.path);
  rejectedStore(fixture, credential);
  CHECK(std::filesystem::is_symlink(fixture.path));
  std::filesystem::remove(fixture.path);
  CHECK_EQ(mkfifo(fixture.path.c_str(), 0600), 0);
  CHECK(!store.restore(credential.token, "org.test.App"));
  CHECK(!store.approve("org.test.App", {}));
  CHECK(std::filesystem::is_fifo(fixture.path));
}

UMBRIEL_TEST(symlinkDirectoriesCannotRestoreAnOtherwiseValidGrant) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  const auto credential = approve(store);
  const auto alias = fixture.directory / "alias";
  std::filesystem::create_directory_symlink(fixture.path.parent_path(), alias);
  GlobalShortcutsStore throughAlias(alias / fixture.path.filename());
  CHECK(!throughAlias.restore(credential.token, "org.test.App"));
  CHECK(!throughAlias.approve("org.test.Other", {}));
  CHECK(store.restore(credential.token, "org.test.App").has_value());
}

UMBRIEL_TEST(invalidApprovalInputCannotCreateAStoreOrWidenAnExistingGrant) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  CHECK(!store.approve("", {}));
  CHECK(!store.approve(std::string(257, 'a'), {}));
  CHECK(!store.approve("org.test.App", std::array{shortcut("", "F16")}));
  CHECK(!store.approve("org.test.App", std::array{shortcut("same", "F16"), shortcut("same", "F17")}));
  auto invalid = shortcut("invalid", "F16");
  invalid.description = std::string(1025, 'a');
  CHECK(!store.approve("org.test.App", std::array{invalid}));
  invalid.description = std::string(1, '\xff');
  CHECK(!store.approve("org.test.App", std::array{invalid}));
  CHECK(!std::filesystem::exists(fixture.path));
  const auto credential = approve(store);
  CHECK(!store.restore(std::string(64, 'z'), "org.test.App"));
  CHECK(!store.approve("org.test.App", {}, std::string(32, 'b')));
  CHECK(store.restore(credential.token, "org.test.App").has_value());
}

UMBRIEL_TEST(scopeAndRecordLimitsFailWithoutConsumingExistingPermission) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  std::vector<GlobalShortcut> approved;
  approved.reserve(kMaxGlobalShortcuts + 1);
  for (size_t index = 0; index < kMaxGlobalShortcuts; ++index) {
    approved.push_back(shortcut(std::format("action-{}", index), "F16"));
  }
  const auto credential = store.approve("org.test.App", approved).value();
  approved.push_back(shortcut("too-many", "F17"));
  CHECK(!store.approve("org.test.Other", approved));
  auto current = credential;
  for (size_t first = kMaxGlobalShortcuts; first < kMaxGlobalShortcutGrantEntries; first += kMaxGlobalShortcuts) {
    std::vector<GlobalShortcut> additions;
    additions.reserve(kMaxGlobalShortcuts);
    for (size_t index = first; index < first + kMaxGlobalShortcuts; ++index) {
      additions.push_back(shortcut(std::format("action-{}", index), "F16"));
    }
    const auto extended = store.approve("org.test.App", additions, current.grant.id);
    CHECK(extended.has_value());
    if (!extended) {
      return;
    }
    current = *extended;
  }
  CHECK_EQ(current.grant.approved.size(), kMaxGlobalShortcutGrantEntries);
  CHECK(!store.approve("org.test.App", std::array{approved.back()}, current.grant.id));
  CHECK(store.restore(current.token, "org.test.App").has_value());
  GlobalShortcutsRegistry registry;
  const auto id = registry.createSession("org.test.App").value();
  CHECK(registry.restore(id, current.grant));
  const auto pending = registry.beginBind(id, {{current.grant.approved.back().id, "Existing action", "F20"}}).value();
  CHECK(pending.approval.empty());
  CHECK(registry.commitBind(id));

  auto data = nlohmann::json::parse(fixture.read());
  auto record = data["grants"][0];
  record["approved"] = nlohmann::json::array({record["approved"][0]});
  data["grants"] = nlohmann::json::array();
  for (unsigned index = 0; index < 129; ++index) {
    record["id"] = std::format("{:032x}", index);
    record["token"] = std::format("{:064x}", index);
    data["grants"].push_back(record);
  }
  fixture.write(data.dump());
  CHECK(!store.list());
}

UMBRIEL_TEST(revocationRemovesEveryGrantForOneApplicationAndKeepsOtherApplications) {
  Fixture fixture;
  GlobalShortcutsStore store(fixture.path);
  GlobalShortcutsStore stale(fixture.path);
  const auto first = approve(store);
  const auto second = approve(store);
  const auto other = approve(store, "org.test.Other");
  const auto literalAll = approve(store, "all");
  CHECK(store.revokeApplication("all"));
  CHECK(!stale.restore(literalAll.token, "all"));
  CHECK(store.revokeApplication("org.test.App"));
  CHECK(!stale.restore(first.token, "org.test.App"));
  CHECK(!stale.restore(second.token, "org.test.App"));
  CHECK(stale.restore(other.token, "org.test.Other").has_value());
  CHECK(store.revokeAll());
  CHECK(stale.list()->empty());
  fixture.write("malformed");
  CHECK(!store.revokeApplication("org.test.App"));
  CHECK(store.revokeAll());
  CHECK(stale.list()->empty());
}

int main() { return RUN_TESTS(); }
