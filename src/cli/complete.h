#pragma once

#include "config/effects.h"

#include <array>
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

  std::span<const CompletionShellName> completionShellNames();

  // One family of values completion cannot derive from the compiled tables. The engine reads it only where the words at
  // hand can use it, so a loader is asked at most once and a position that needs no values never pays for one.
  class CompletionList {
  public:
    using Filler = std::function<void(std::vector<std::string>&)>;

    CompletionList() = default;
    CompletionList(std::initializer_list<std::string> items);
    explicit CompletionList(Filler filler);

    // A literal list keeps its order; a filler's list is sorted on the first read and never filled twice.
    const std::vector<std::string>& items() const;

  private:
    mutable std::vector<std::string> m_items;
    Filler m_filler;
    mutable bool m_filled = false;
  };

  // The effect names a position may reference, filtered by the slot kind and the preset/pool constraint the action
  // imposes. One list is filled per kind asked for, so a position that reaches no effect pays for none.
  class EffectCandidates {
  public:
    using Filler = std::function<void(std::vector<std::string>& presets, std::vector<std::string>& pools, EffectKind)>;

    EffectCandidates() = default;
    explicit EffectCandidates(Filler filler);

    const std::vector<std::string>& presets(EffectKind kind) const;
    const std::vector<std::string>& pools(EffectKind kind) const;

  private:
    struct KindLists {
      std::vector<std::string> presets;
      std::vector<std::string> pools;
    };

    void fill(EffectKind kind) const;

    Filler m_filler;
    mutable std::array<KindLists, kEffectKindCount> m_byKind{};
    mutable std::array<bool, kEffectKindCount> m_filled{};
  };

  // Configuration-derived entries need no session; the live ones come from a running compositor and are optional.
  struct CompletionValues {
    CompletionList outputs;
    CompletionList windows;
    CompletionList workspaces;
    EffectCandidates effects;
    CompletionList scratchpads;
    CompletionList submaps;
  };

  enum class [[nodiscard]] CompletionOutcome : uint8_t {
    Candidates,      // zero or more sink calls; the shell offers them and nothing else
    Delegate,        // the position is a filesystem path: the shell completes paths
    DelegateCommand, // the position is a command line: the shell completes command names
  };

  using CompletionSink = std::function<void(std::string_view value, std::string_view description)>;

  // `words` are the words after the executable; the last is the word being completed and may be empty, so
  // `umbriel msg <TAB>` is `{"msg", ""}`. Names are offered in the order the tables declare them.
  CompletionOutcome
  completeWords(std::span<const std::string> words, const CompletionValues& values, const CompletionSink& sink);

  std::optional<CompletionShell> completionShellNamed(std::string_view name);

  // The script for a shell, printed by `umbriel completions <shell>` and installed by Meson.
  std::string completionScript(CompletionShell shell);

} // namespace umbriel
