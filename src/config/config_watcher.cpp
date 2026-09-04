#include "config/config_watcher.h"

#include "core/log.h"

#include <cerrno>
#include <set>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("config");
  }

  ConfigWatcher::ConfigWatcher(wl_event_loop* loop, std::function<void()> onChange) : m_onChange(std::move(onChange)) {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
      kLog.warn("config: inotify unavailable; hot reload disabled (use the config-reload keybind)");
      return;
    }

    m_fdSource = wl_event_loop_add_fd(loop, m_fd, WL_EVENT_READABLE, onInotify, this);
    m_timer = wl_event_loop_add_timer(loop, onTimer, this);
    if (m_fdSource == nullptr || m_timer == nullptr) {
      kLog.warn(
          "config: unable to register inotify event sources; hot reload disabled (use the config-reload keybind)"
      );
      if (m_fdSource != nullptr) {
        wl_event_source_remove(m_fdSource);
        m_fdSource = nullptr;
      }
      if (m_timer != nullptr) {
        wl_event_source_remove(m_timer);
        m_timer = nullptr;
      }
      close(m_fd);
      m_fd = -1;
    }
  }

  ConfigWatcher::~ConfigWatcher() {
    if (m_fdSource != nullptr) {
      wl_event_source_remove(m_fdSource);
    }
    if (m_timer != nullptr) {
      wl_event_source_remove(m_timer);
    }
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  void ConfigWatcher::watch(const std::vector<std::filesystem::path>& files) {
    if (m_fd < 0) {
      return;
    }

    std::set<std::filesystem::path> nextFiles;
    for (const std::filesystem::path& file : files) {
      std::error_code error;
      std::filesystem::path fullPath = std::filesystem::absolute(file, error);
      if (error) {
        fullPath = file;
      }
      fullPath = fullPath.lexically_normal();
      nextFiles.insert(fullPath);

      // Keep every lexical symlink component observable. Watching only the
      // resolved directory misses an atomic replacement of a symlinked config
      // directory, because that event belongs to the symlink's parent.
      for (std::filesystem::path component = fullPath; !component.empty();) {
        error.clear();
        if (std::filesystem::is_symlink(component, error) && !error) {
          nextFiles.insert(component);
        }
        const std::filesystem::path parent = component.parent_path();
        if (parent == component) {
          break;
        }
        component = parent;
      }

      error.clear();
      std::filesystem::path resolved = std::filesystem::weakly_canonical(fullPath, error);
      if (!error) {
        nextFiles.insert(std::move(resolved));
      }

      // weakly_canonical cannot follow a dangling final symlink. Resolve that
      // entry directly so recreating its target can recover automatically.
      error.clear();
      if (std::filesystem::is_symlink(fullPath, error) && !error) {
        error.clear();
        std::filesystem::path target = std::filesystem::read_symlink(fullPath, error);
        if (!error) {
          if (target.is_relative()) {
            target = fullPath.parent_path() / target;
          }
          error.clear();
          std::filesystem::path targetPath = std::filesystem::absolute(target, error);
          if (error) {
            targetPath = target;
          }
          nextFiles.insert(targetPath.lexically_normal());
        }
      }
    }

    std::set<std::filesystem::path> directories;
    const std::set<std::filesystem::path> requestedFiles = nextFiles;
    for (const std::filesystem::path& file : requestedFiles) {
      std::filesystem::path directory = file.parent_path();
      while (!directory.empty()) {
        std::error_code error;
        if (std::filesystem::is_directory(directory, error) && !error) {
          directories.insert(directory);
          break;
        }
        // If a parent component does not exist yet, watch its nearest existing
        // ancestor for the component's creation. The reload callback rearms
        // watches one level deeper until the requested file can be observed.
        nextFiles.insert(directory.lexically_normal());
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
          break;
        }
        directory = parent;
      }
    }

    std::unordered_map<int, std::set<std::filesystem::path>> nextDirectoryWatches;
    for (const std::filesystem::path& directory : directories) {
      const int descriptor = inotify_add_watch(
          m_fd, directory.c_str(),
          IN_CREATE
              | IN_CLOSE_WRITE
              | IN_ATTRIB
              | IN_MOVED_FROM
              | IN_MOVED_TO
              | IN_DELETE
              | IN_MODIFY
              | IN_DELETE_SELF
              | IN_MOVE_SELF
      );
      if (descriptor < 0) {
        kLog.debug("config: unable to watch directory {}", directory.string());
        continue;
      }
      // inotify identifies the watched inode, not the pathname. A symlinked
      // directory and its canonical target therefore share one descriptor,
      // while config paths can legitimately use either spelling.
      nextDirectoryWatches[descriptor].insert(directory);
    }

    // Add or refresh desired watches before removing obsolete ones. Events
    // queued for a directory that remains relevant therefore keep a valid
    // descriptor mapping across reloads.
    for (const auto& [descriptor, watchedDirectories] : m_dirWatches) {
      (void)watchedDirectories;
      if (!nextDirectoryWatches.contains(descriptor)) {
        inotify_rm_watch(m_fd, descriptor);
      }
    }
    m_files = std::move(nextFiles);
    m_dirWatches = std::move(nextDirectoryWatches);
  }

  int ConfigWatcher::onInotify(int fd, uint32_t /*mask*/, void* data) {
    auto* self = static_cast<ConfigWatcher*>(data);
    alignas(inotify_event) char buffer[4096];
    bool changed = false;

    while (true) {
      const ssize_t size = read(fd, buffer, sizeof(buffer));
      if (size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (size == 0) {
        break;
      }

      size_t offset = 0;
      while (offset < static_cast<size_t>(size)) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          changed = true;
        } else if (event->len == 0 && (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
          changed = true;
        } else if (event->len > 0) {
          const auto directories = self->m_dirWatches.find(event->wd);
          if (directories != self->m_dirWatches.end()) {
            for (const std::filesystem::path& directory : directories->second) {
              const std::filesystem::path path = (directory / event->name).lexically_normal();
              if (self->m_files.contains(path)) {
                changed = true;
                break;
              }
            }
          }
        }
        offset += sizeof(inotify_event) + event->len;
      }
    }

    if (changed && self->m_timer != nullptr) {
      wl_event_source_timer_update(self->m_timer, 150);
    }
    return 0;
  }

  int ConfigWatcher::onTimer(void* data) {
    auto* self = static_cast<ConfigWatcher*>(data);
    self->m_onChange();
    return 0;
  }

} // namespace umbriel
