#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  enum class CompletionShell : uint8_t { Bash, Zsh, Fish };

  // Every shell with a script, in the order `umbriel completions` and its help list them.
  struct CompletionShellName {
    CompletionShell shell;
    std::string_view name;
  };

  [[nodiscard]] std::span<const CompletionShellName> completionShellNames();

  // One family of values completion cannot derive from the compiled tables. The engine reads it only where the words at
  // hand can use it, so a loader is asked at most once and a position that needs no values never pays for one.
  class CompletionList {
  public:
    using Filler = std::function<void(std::vector<std::string>&)>;

    CompletionList() = default;
    CompletionList(std::initializer_list<std::string> items);
    CompletionList(std::vector<std::string> items);
    explicit CompletionList(Filler filler);

    // Sorted on the first read; a filler that ran is never asked again.
    [[nodiscard]] const std::vector<std::string>& items() const;

  private:
    mutable std::vector<std::string> m_items;
    Filler m_filler;
    mutable bool m_filled = false;
  };

  // Configuration-derived entries need no session; the live ones come from a running compositor and are optional.
  struct CompletionValues {
    CompletionList outputs;
    CompletionList windows;
    CompletionList workspaces;
    CompletionList effectPresets;
    CompletionList effectPools;
    CompletionList scratchpads;
    CompletionList submaps;
  };

  enum class CompletionOutcome : uint8_t {
    Candidates, // zero or more sink calls; the shell offers them and nothing else
    Delegate,   // the position belongs to the shell: a command line or a path
  };

  using CompletionSink = std::function<void(std::string_view value, std::string_view description)>;

  // `words` are the words after the executable; the last is the word being completed and may be empty, so
  // `umbriel msg <TAB>` is `{"msg", ""}`. Names are offered in the order the tables declare them.
  [[nodiscard]] CompletionOutcome
  completeWords(std::span<const std::string> words, const CompletionValues& values, const CompletionSink& sink);

  [[nodiscard]] std::optional<CompletionShell> completionShellNamed(std::string_view name);

  // The script for a shell, printed by `umbriel completions <shell>` and installed by Meson.
  [[nodiscard]] std::string completionScript(CompletionShell shell);

} // namespace umbriel
