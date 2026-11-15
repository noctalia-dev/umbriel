#include "input/x11_input_permissions.h"

#include "check.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

using umbriel::X11InputPermissions;

namespace {
  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      constexpr std::string_view prefix = "/tmp/umbriel-x11-input-permissions.XXXXXX";
      std::ranges::copy(prefix, pattern.begin());
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a private permission test directory");
      }
      directory = pattern.data();
      path = directory / "state/x11-input-permissions.json";
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
        throw std::runtime_error("could not seed the permission test store");
      }
    }

    std::filesystem::path directory;
    std::filesystem::path path;
  };
} // namespace

UMBRIEL_TEST(x11InputPermissionsRememberAcrossInstancesPrivately) {
  const Fixture fixture;
  X11InputPermissions permissions(fixture.path);
  CHECK(!permissions.contains("steam"));
  CHECK(permissions.list().empty());

  CHECK(permissions.remember("steam"));
  CHECK(permissions.remember("steam"));
  CHECK(permissions.contains("steam"));

  const X11InputPermissions reopened(fixture.path);
  CHECK(reopened.contains("steam"));
  CHECK_EQ(reopened.list(), std::vector<std::string>{"steam"});

  struct stat info{};
  CHECK_EQ(stat(fixture.path.c_str(), &info), 0);
  CHECK_EQ(info.st_mode & 0777, mode_t{0600});
}

UMBRIEL_TEST(x11InputPermissionsRevokeOneOrAll) {
  const Fixture fixture;
  X11InputPermissions permissions(fixture.path);
  CHECK(permissions.remember("steam"));
  CHECK(permissions.remember("xdotool"));

  CHECK(permissions.revoke("steam"));
  CHECK(!permissions.contains("steam"));
  CHECK(permissions.contains("xdotool"));
  CHECK(permissions.revoke("steam"));

  CHECK(permissions.revoke("all"));
  CHECK(permissions.list().empty());
}

UMBRIEL_TEST(x11InputPermissionsRejectUnsafeOrMalformedStores) {
  const Fixture fixture;
  X11InputPermissions permissions(fixture.path);
  CHECK(!permissions.remember(""));
  CHECK(!permissions.remember(std::string(257, 'a')));

  fixture.write(R"({"version":1,"applications":["steam"]})", 0622);
  CHECK(!permissions.contains("steam"));
  CHECK(!permissions.remember("other"));

  fixture.write(R"({"version":2,"applications":["steam"]})");
  CHECK(!permissions.contains("steam"));

  fixture.write(R"({"version":1,"applications":["steam",""]})");
  CHECK(!permissions.contains("steam"));

  fixture.write("not json");
  CHECK(!permissions.contains("steam"));
  CHECK(!permissions.remember("steam"));

  fixture.write(R"({"version":1,"applications":["steam"]})");
  CHECK(permissions.contains("steam"));
}

int main() { return RUN_TESTS(); }
