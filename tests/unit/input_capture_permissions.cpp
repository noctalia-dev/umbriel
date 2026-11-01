#include "input/input_capture_permissions.h"

#include "check.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using umbriel::InputCapturePermissions;

namespace {
  constexpr std::string_view kId = "11111111111111111111111111111111";
  constexpr std::string_view kToken = "2222222222222222222222222222222222222222222222222222222222222222";

  std::string record(std::string_view id = kId, std::string_view token = kToken, unsigned capabilities = 3) {
    return std::format(
        R"({{"id":"{}","token":"{}","application":"org.test.Capture","capabilities":{}}})", id, token, capabilities
    );
  }

  std::string document(std::string_view records) { return std::format(R"({{"version":1,"grants":[{}]}})", records); }

  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      constexpr std::string_view prefix = "/tmp/umbriel-capture-permissions.XXXXXX";
      std::ranges::copy(prefix, pattern.begin());
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a private permission test directory");
      }
      directory = pattern.data();
      path = directory / "state/permissions.json";
    }
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(std::string_view bytes, mode_t permissions = 0600) const {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      if (!output || chmod(path.c_str(), permissions) != 0) {
        throw std::runtime_error("could not seed the private permission test store");
      }
    }

    [[nodiscard]] std::string read() const {
      std::ifstream input(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::filesystem::path directory;
    std::filesystem::path path;
  };

  void rejectedStore(const Fixture& fixture) {
    const std::string before = fixture.read();
    InputCapturePermissions permissions(fixture.path);
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.New", 3, 2, 11));
    CHECK(fixture.read() == before);
  }
} // namespace

UMBRIEL_TEST(ephemeralRestoreRotatesTheTokenAndExpiresOnlyItsOwningPeer) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto first = permissions.issue("org.test.First", 3, 1, 11);
  const auto second = permissions.issue("org.test.Second", 2, 1, 22);
  CHECK(first && second);
  if (!first || !second) {
    return;
  }
  CHECK(!std::filesystem::exists(fixture.path));
  CHECK(!permissions.restore(first->token, "org.test.First", 3, 1, 22));
  const auto rotated = permissions.restore(first->token, "org.test.First", 1, 1, 11);
  CHECK(rotated.has_value());
  if (!rotated) {
    return;
  }
  CHECK_EQ(rotated->id, first->id);
  CHECK(rotated->token != first->token);
  CHECK(!permissions.restore(first->token, "org.test.First", 1, 1, 11));
  InputCapturePermissions anotherInstance(fixture.path);
  CHECK(!anotherInstance.restore(rotated->token, "org.test.First", 1, 1, 11));
  permissions.expirePeer(22);
  CHECK(!permissions.restore(second->token, "org.test.Second", 2, 1, 22));
  const auto surviving = permissions.restore(rotated->token, "org.test.First", 3, 1, 11);
  CHECK(surviving.has_value());
  if (surviving) {
    permissions.expirePeer(11);
    CHECK(!permissions.restore(surviving->token, "org.test.First", 3, 1, 11));
  }
  CHECK(permissions.list().empty());
}

UMBRIEL_TEST(durableRestoreRefreshesStaleInstancesAndRejectsConsumedCredentials) {
  Fixture fixture;
  InputCapturePermissions issuer(fixture.path);
  InputCapturePermissions stale(fixture.path);
  const auto grant = issuer.issue("org.test.Capture", 3, 2, 11);
  CHECK(grant.has_value());
  if (!grant) {
    return;
  }
  const auto restored = stale.restore(grant->token, "org.test.Capture", 2, 2, 22);
  CHECK(restored.has_value());
  if (!restored) {
    return;
  }
  CHECK_EQ(restored->id, grant->id);
  CHECK(restored->token != grant->token);
  CHECK(!issuer.restore(grant->token, "org.test.Capture", 3, 2, 11));
  stale.expirePeer(22);
  InputCapturePermissions reopened(fixture.path);
  const auto next = reopened.restore(restored->token, "org.test.Capture", 1, 2, 33);
  CHECK(next.has_value());
  CHECK(!stale.restore(restored->token, "org.test.Capture", 2, 2, 22));
  if (next) {
    CHECK(next->token != restored->token);
    CHECK_EQ(next->id, grant->id);
  }
}

UMBRIEL_TEST(failedScopeUpgradesDoNotConsumeAnOtherwiseValidCredential) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  CHECK(!permissions.issue("org.test.Capture", 0, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 4, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 1, 3, 11));
  CHECK(!permissions.issue("org.test.Capture", 1, 0, 11));
  CHECK(!permissions.issue("org.test.Capture", 1, 2, 0));
  CHECK(!permissions.issue(std::string(257, 'a'), 1, 2, 11));
  const auto grant = permissions.issue("org.test.Capture", 1, 1, 11);
  CHECK(grant.has_value());
  if (!grant) {
    return;
  }
  CHECK(!permissions.restore(grant->token, "org.test.Other", 1, 1, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 3, 1, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 1, 2, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 1, 1, 0));
  CHECK(!permissions.restore(std::string(64, 'z'), "org.test.Capture", 1, 1, 11));
  CHECK(permissions.restore(grant->token, "org.test.Capture", 1, 1, 11).has_value());
}

UMBRIEL_TEST(blankApplicationIdsRequireIndependentTokensAndSupportIndividualRevocation) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  InputCapturePermissions stale(fixture.path);
  const auto first = permissions.issue("", 3, 2, 11);
  const auto second = permissions.issue("", 3, 2, 22);
  CHECK(first && second);
  if (!first || !second) {
    return;
  }
  CHECK(first->id != second->id);
  CHECK(first->token != second->token);
  CHECK(!stale.restore(first->token, "org.test.Named", 3, 2, 33));
  CHECK(stale.revoke(first->id));
  CHECK(!permissions.restore(first->token, "", 3, 2, 11));
  CHECK(!permissions.revoke("unknown-permission"));
  const auto remaining = permissions.restore(second->token, "", 3, 2, 44);
  CHECK(remaining.has_value());
  CHECK(stale.revoke("all"));
  if (remaining) {
    CHECK(!permissions.restore(remaining->token, "", 3, 2, 44));
  }
  CHECK(permissions.list().empty());
}

UMBRIEL_TEST(downgradingDurablePermissionRemovesTheDurableCredential) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto durable = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(durable.has_value());
  if (!durable) {
    return;
  }
  const auto ephemeral = permissions.restore(durable->token, "org.test.Capture", 3, 1, 22);
  CHECK(ephemeral.has_value());
  if (!ephemeral) {
    return;
  }
  CHECK_EQ(ephemeral->persistMode, 1U);
  InputCapturePermissions reopened(fixture.path);
  CHECK(reopened.list().empty());
  CHECK(!reopened.restore(durable->token, "org.test.Capture", 3, 2, 11));
  CHECK(!reopened.restore(ephemeral->token, "org.test.Capture", 3, 1, 22));
  permissions.expirePeer(22);
  CHECK(!permissions.restore(ephemeral->token, "org.test.Capture", 3, 1, 22));
}

UMBRIEL_TEST(restoringForOneUseConsumesPermissionWithoutIssuingAnotherToken) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto durable = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(durable.has_value());
  if (!durable) {
    return;
  }
  const auto once = permissions.restore(durable->token, "org.test.Capture", 3, 0, 22);
  CHECK(once.has_value());
  if (once) {
    CHECK_EQ(once->persistMode, 0U);
    CHECK(once->token.empty());
  }
  CHECK(!permissions.restore(durable->token, "org.test.Capture", 3, 2, 11));
  CHECK(permissions.revoke(durable->id));
  InputCapturePermissions reopened(fixture.path);
  CHECK(reopened.list().empty());
}

UMBRIEL_TEST(durableCredentialsUsePrivateFilesAndTightenTheirOwnedDirectory) {
  Fixture fixture;
  std::filesystem::create_directories(fixture.path.parent_path());
  CHECK_EQ(chmod(fixture.path.parent_path().c_str(), 0755), 0);
  InputCapturePermissions permissions(fixture.path);
  const auto grant = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(grant.has_value());
  struct stat file{};
  struct stat directory{};
  CHECK_EQ(stat(fixture.path.c_str(), &file), 0);
  CHECK_EQ(stat(fixture.path.parent_path().c_str(), &directory), 0);
  CHECK_EQ(file.st_mode & 0777, 0600U);
  CHECK_EQ(directory.st_mode & 0777, 0700U);
  CHECK_EQ(file.st_uid, geteuid());
  if (grant) {
    CHECK(permissions.revoke(grant->id));
    InputCapturePermissions reopened(fixture.path);
    CHECK(!reopened.restore(grant->token, "org.test.Capture", 3, 2, 11));
  }
}

UMBRIEL_TEST(durableTransactionsCannotBypassTheStoreLockOrConsumeTokensWhileBusy) {
  Fixture fixture;
  fixture.write(document(record()));
  InputCapturePermissions permissions(fixture.path);
  const auto lockPath = fixture.path.parent_path() / ".input-capture-permissions.lock";
  const int lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  CHECK(lock >= 0);
  if (lock < 0) {
    return;
  }
  CHECK_EQ(flock(lock, LOCK_EX | LOCK_NB), 0);
  CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
  CHECK(!permissions.issue("org.test.New", 3, 2, 11));
  CHECK(!permissions.revoke(kId));
  CHECK_EQ(close(lock), 0);
  CHECK(permissions.restore(kToken, "org.test.Capture", 3, 2, 11).has_value());
}

UMBRIEL_TEST(malformedStoresNeverPartiallyAuthorizeOrGetOverwrittenByNewGrants) {
  constexpr std::string_view otherToken = "3333333333333333333333333333333333333333333333333333333333333333";
  const std::array documents{
      std::string{"{"},
      std::string{R"({"version":2,"grants":[]})"},
      std::string{R"({"version":1,"grants":{}})"},
      document(record() + "," + record(kId, otherToken)),
      document(record() + "," + record("44444444444444444444444444444444", otherToken, 4)),
      document(record("short-id")),
  };
  for (const auto& bytes : documents) {
    Fixture fixture;
    fixture.write(bytes);
    rejectedStore(fixture);
  }
}

UMBRIEL_TEST(insecureOversizedAndNonregularStoresCannotRestoreCredentials) {
  {
    Fixture fixture;
    fixture.write(document(record()), 0644);
    rejectedStore(fixture);
  }
  {
    Fixture fixture;
    fixture.write(document(record()) + std::string(256 * 1024, ' '));
    rejectedStore(fixture);
  }
  {
    Fixture fixture;
    std::filesystem::create_directories(fixture.path.parent_path());
    CHECK_EQ(mkfifo(fixture.path.c_str(), 0600), 0);
    InputCapturePermissions permissions(fixture.path);
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.Capture", 3, 2, 11));
    CHECK(std::filesystem::is_fifo(fixture.path));
  }
}

UMBRIEL_TEST(symlinkStoresAndDirectoriesCannotAuthorizeOrReplaceTheirTargets) {
  {
    Fixture fixture;
    const std::string bytes = document(record());
    fixture.write(bytes);
    const auto target = fixture.path.parent_path() / "target.json";
    std::filesystem::rename(fixture.path, target);
    std::filesystem::create_symlink(target, fixture.path);
    rejectedStore(fixture);
    CHECK(std::filesystem::is_symlink(fixture.path));
  }
  {
    Fixture fixture;
    fixture.write(document(record()));
    const auto alias = fixture.directory / "alias";
    std::filesystem::create_directory_symlink(fixture.path.parent_path(), alias);
    InputCapturePermissions permissions(alias / fixture.path.filename());
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.New", 3, 2, 11));
    CHECK(fixture.read() == document(record()));
  }
}

int main() { return RUN_TESTS(); }
