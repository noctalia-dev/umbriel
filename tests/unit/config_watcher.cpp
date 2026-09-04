#include "config/config_watcher.h"

#include "check.h"
#include "config/store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <wayland-server-core.h>

using umbriel::ConfigStore;
using umbriel::ConfigWatcher;

namespace {
  class ScopedEnvironment {
  public:
    ScopedEnvironment(const char* name, const std::string& value) : m_name(name) {
      if (const char* previous = std::getenv(name)) {
        m_previous = previous;
      }
      setenv(name, value.c_str(), 1);
    }

    ~ScopedEnvironment() {
      if (m_previous) {
        setenv(m_name.c_str(), m_previous->c_str(), 1);
      } else {
        unsetenv(m_name.c_str());
      }
    }

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
  };

  class DefaultConfigTree {
  public:
    DefaultConfigTree()
        : m_base(
              std::filesystem::temp_directory_path() / ("umbriel-config-watcher-priority-" + std::to_string(getpid()))
          ),
          m_userHome(m_base / "user"), m_systemDir(m_base / "system") {
      std::filesystem::remove_all(m_base);
      std::filesystem::create_directories(m_userHome);
      write(m_systemDir / "umbriel/config.toml", 17);
    }

    ~DefaultConfigTree() { std::filesystem::remove_all(m_base); }

    DefaultConfigTree(const DefaultConfigTree&) = delete;
    DefaultConfigTree& operator=(const DefaultConfigTree&) = delete;

    void writeUser(int gap) const { write(userConfig(), gap); }
    void writeUserTarget(int gap) const { write(userTarget(), gap); }

    void linkUserToTarget(int gap) const {
      writeUserTarget(gap);
      std::filesystem::create_directories(userConfig().parent_path());
      std::filesystem::create_symlink(userTarget(), userConfig());
    }

    void removeUserTarget() const { std::filesystem::remove(userTarget()); }

    [[nodiscard]] const std::filesystem::path& userHome() const { return m_userHome; }
    [[nodiscard]] const std::filesystem::path& systemDir() const { return m_systemDir; }
    [[nodiscard]] std::filesystem::path userConfig() const { return m_userHome / "umbriel/config.toml"; }
    [[nodiscard]] std::filesystem::path userTarget() const { return m_base / "target/config.toml"; }
    [[nodiscard]] std::filesystem::path systemConfig() const { return m_systemDir / "umbriel/config.toml"; }

  private:
    static void write(const std::filesystem::path& path, int gap) {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream stream(path);
      stream << "[layout]\ngap = " << gap << '\n';
    }

    std::filesystem::path m_base;
    std::filesystem::path m_userHome;
    std::filesystem::path m_systemDir;
  };

  class SymlinkedConfigTree {
  public:
    SymlinkedConfigTree()
        : m_base(std::filesystem::temp_directory_path() / ("umbriel-config-watcher-" + std::to_string(getpid()))),
          m_realDir(m_base / "z-real"), m_nextDir(m_base / "y-next"), m_linkDir(m_base / "a-link") {
      std::filesystem::remove_all(m_base);
      std::filesystem::create_directories(m_realDir);
      std::filesystem::create_directories(m_nextDir);
      std::filesystem::create_directory_symlink(m_realDir, m_linkDir);
      writeRoot(5);
      writeInclude(2);
      std::ofstream stream(m_nextDir / "config.toml");
      stream << "[layout]\ngap = 13\n";
    }

    ~SymlinkedConfigTree() { std::filesystem::remove_all(m_base); }

    SymlinkedConfigTree(const SymlinkedConfigTree&) = delete;
    SymlinkedConfigTree& operator=(const SymlinkedConfigTree&) = delete;

    void writeRoot(int gap) const {
      std::ofstream stream(m_realDir / "config.toml");
      stream << "[include]\nfiles = [\"noctalia.toml\"]\n\n[layout]\ngap = " << gap << '\n';
    }

    void writeInclude(int borderWidth) const {
      std::ofstream stream(m_realDir / "noctalia.toml");
      stream << "[appearance]\nborder_width = " << borderWidth << '\n';
    }

    void retarget() const {
      const std::filesystem::path replacement = m_base / "replacement-link";
      std::filesystem::create_directory_symlink(m_nextDir, replacement);
      std::filesystem::rename(replacement, m_linkDir);
    }

    [[nodiscard]] std::filesystem::path root() const { return m_linkDir / "config.toml"; }

  private:
    std::filesystem::path m_base;
    std::filesystem::path m_realDir;
    std::filesystem::path m_nextDir;
    std::filesystem::path m_linkDir;
  };

  bool dispatchUntil(wl_event_loop* loop, int& reloads, int expected) {
    for (int attempt = 0; attempt < 20 && reloads < expected; ++attempt) {
      if (wl_event_loop_dispatch(loop, 50) < 0) {
        return false;
      }
    }
    return reloads >= expected;
  }
} // namespace

UMBRIEL_TEST(anIncludedFileReloadsThroughASymlinkedConfigDirectory) {
  const SymlinkedConfigTree tree;
  const std::string root = tree.root().string();
  ConfigStore& store = umbriel::configStore();
  store.load(root.c_str());

  CHECK_EQ(store.config().layout.gap, 5);
  CHECK_EQ(store.config().appearance.borderWidth, 2);

  wl_event_loop* loop = wl_event_loop_create();
  CHECK(loop != nullptr);
  if (loop == nullptr) {
    return;
  }

  int reloads = 0;
  std::unique_ptr<ConfigWatcher> watcher;
  watcher = std::make_unique<ConfigWatcher>(loop, [&] {
    const umbriel::ConfigReloadResult result = store.reload();
    if (result.success) {
      ++reloads;
    }
    watcher->watch(store.watchPaths());
  });
  watcher->watch(store.watchPaths());

  tree.writeRoot(9);
  CHECK(dispatchUntil(loop, reloads, 1));
  CHECK_EQ(store.config().layout.gap, 9);

  tree.writeInclude(7);
  CHECK(dispatchUntil(loop, reloads, 2));
  CHECK_EQ(store.config().appearance.borderWidth, 7);

  watcher.reset();
  wl_event_loop_destroy(loop);
}

UMBRIEL_TEST(aRetargetedSymlinkedConfigDirectoryReloadsFromTheNewTarget) {
  const SymlinkedConfigTree tree;
  const std::string root = tree.root().string();
  ConfigStore& store = umbriel::configStore();
  store.load(root.c_str());

  CHECK_EQ(store.config().layout.gap, 5);

  wl_event_loop* loop = wl_event_loop_create();
  CHECK(loop != nullptr);
  if (loop == nullptr) {
    return;
  }

  int reloads = 0;
  std::unique_ptr<ConfigWatcher> watcher;
  watcher = std::make_unique<ConfigWatcher>(loop, [&] {
    const umbriel::ConfigReloadResult result = store.reload();
    if (result.success) {
      ++reloads;
    }
    watcher->watch(store.watchPaths());
  });
  watcher->watch(store.watchPaths());

  tree.retarget();
  const bool reloaded = dispatchUntil(loop, reloads, 1);
  CHECK(reloaded);
  if (reloaded) {
    CHECK_EQ(store.config().layout.gap, 13);
  }

  watcher.reset();
  wl_event_loop_destroy(loop);
}

UMBRIEL_TEST(aNewHigherPriorityUserConfigIsAdoptedWithoutRestart) {
  const DefaultConfigTree tree;
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", tree.userHome().string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", tree.systemDir().string());

  ConfigStore& store = umbriel::configStore();
  store.load(nullptr);

  CHECK_EQ(store.rootPath(), tree.systemConfig());
  CHECK_EQ(store.config().layout.gap, 17);

  wl_event_loop* loop = wl_event_loop_create();
  CHECK(loop != nullptr);
  if (loop == nullptr) {
    return;
  }

  int adoptions = 0;
  std::unique_ptr<ConfigWatcher> watcher;
  watcher = std::make_unique<ConfigWatcher>(loop, [&] {
    const umbriel::ConfigReloadResult result = store.reload();
    if (result.success && store.rootPath() == tree.userConfig() && store.config().layout.gap == 19) {
      ++adoptions;
    }
    watcher->watch(store.watchPaths());
  });
  watcher->watch(store.watchPaths());

  tree.writeUser(19);
  CHECK(dispatchUntil(loop, adoptions, 1));

  watcher.reset();
  wl_event_loop_destroy(loop);
}

UMBRIEL_TEST(aDeletedSymlinkTargetIsRewatchedWhenItReappears) {
  const DefaultConfigTree tree;
  tree.linkUserToTarget(19);
  const ScopedEnvironment configHome("XDG_CONFIG_HOME", tree.userHome().string());
  const ScopedEnvironment configDirs("XDG_CONFIG_DIRS", tree.systemDir().string());

  ConfigStore& store = umbriel::configStore();
  store.load(nullptr);

  CHECK_EQ(store.rootPath(), tree.userConfig());
  CHECK_EQ(store.config().layout.gap, 19);

  wl_event_loop* loop = wl_event_loop_create();
  CHECK(loop != nullptr);
  if (loop == nullptr) {
    return;
  }

  int reloads = 0;
  std::unique_ptr<ConfigWatcher> watcher;
  watcher = std::make_unique<ConfigWatcher>(loop, [&] {
    const umbriel::ConfigReloadResult result = store.reload();
    if (result.success) {
      ++reloads;
    }
    watcher->watch(store.watchPaths());
  });
  watcher->watch(store.watchPaths());

  tree.removeUserTarget();
  CHECK(dispatchUntil(loop, reloads, 1));
  CHECK_EQ(store.rootPath(), tree.systemConfig());
  CHECK_EQ(store.config().layout.gap, 17);

  tree.writeUserTarget(23);
  const bool reloaded = dispatchUntil(loop, reloads, 2);
  CHECK(reloaded);
  if (reloaded) {
    CHECK_EQ(store.rootPath(), tree.userConfig());
    CHECK_EQ(store.config().layout.gap, 23);
  }

  watcher.reset();
  wl_event_loop_destroy(loop);
}

int main() { return RUN_TESTS(); }
