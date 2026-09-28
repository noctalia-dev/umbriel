#pragma once

#include <cstdarg>
#include <format>
#include <string_view>
#include <utility>

extern "C" {
#include <wlr/util/log.h>
}

enum class LogLevel { Debug, Info, Warn, Error };

namespace detail {
  void logMessage(LogLevel level, const char* section, std::string_view msg);
} // namespace detail

template <typename... Args> void logDebug(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Debug, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logInfo(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Info, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logWarn(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Warn, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logError(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Error, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

class Logger {
public:
  explicit constexpr Logger(const char* section) : m_section(section) {}

  template <typename... Args> void debug(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Debug, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Info, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void warn(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Warn, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Error, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

private:
  const char* m_section;
};

// Opens (or rotates) ~/.cache/umbriel/umbriel.log. Also redirects fd 1/2 to ~/.cache/umbriel/umbriel-stderr.log (or
// /dev/null) if either points at a TTY (greetd wires them to /dev/tty1), since synchronous VT writes on the compositor
// main thread can turn a per-frame wlroots error into a hard livelock. Libraries that bypass wlr_log (glibc asserts,
// mesa, dbus) inherit the redirected fds.
void initLogFile();
void setConsoleLogging(bool enabled);

// Bridges wlroots/scenefx logging into the logger above. Pass to wlr_log_init instead of nullptr: wlroots' built-in
// handler writes straight to stderr with no rate limiting, which lets a per-frame failure stall the compositor on
// console I/O.
void wlrLogHandler(enum wlr_log_importance importance, const char* fmt, va_list args);
