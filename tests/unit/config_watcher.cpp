#include "config/config_watcher.h"

#include "check.h"
#include "config/store.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <wayland-server-core.h>

using umbriel::ConfigStore;
using umbriel::ConfigWatcher;

namespace {
  class SymlinkedConfigTree {
  public:
    SymlinkedConfigTree()
        : m_base(std::filesystem::temp_directory_path() / ("umbriel-config-watcher-" + std::to_string(getpid()))),
          m_realDir(m_base / "z-real"), m_linkDir(m_base / "a-link") {
      std::filesystem::remove_all(m_base);
      std::filesystem::create_directories(m_realDir);
      std::filesystem::create_directory_symlink(m_realDir, m_linkDir);
      writeRoot(5);
      writeInclude(2);
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

    [[nodiscard]] std::filesystem::path root() const { return m_linkDir / "config.toml"; }

  private:
    std::filesystem::path m_base;
    std::filesystem::path m_realDir;
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
  CHECK(store.load(root.c_str()));

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

int main() { return RUN_TESTS(); }
