#include "cli/complete.h"

namespace umbriel {

  namespace {
    // Every script defers to `umbriel complete`, so the vocabulary stays in the binary and cannot drift from the
    // tables. Exit 2 asks the shell for its own completion: a command line for `spawn`, a path for `-c`.
    constexpr std::string_view kBash =
        R"BASH(# bash completion for umbriel: installed by `meson install`, or load it with
#   eval "$(umbriel completions bash)"
_umbriel() {
  local -a candidates=()
  local output code=0 value description
  output=$(umbriel complete -- "${COMP_WORDS[@]:1}" 2>/dev/null) || code=$?
  if ((code == 2)); then
    compopt -o default 2>/dev/null || true
    return 0
  fi
  COMPREPLY=()
  while IFS=$'\t' read -r value description; do
    [[ -n $value ]] || continue
    candidates+=("$value")
  done <<<"$output"
  ((${#candidates[@]} > 0)) || return 0
  COMPREPLY=("${candidates[@]}")
  # Values such as window ids contain spaces; the filenames option makes readline quote them.
  compopt -o filenames 2>/dev/null || true
  return 0
}
complete -F _umbriel umbriel
)BASH";

    constexpr std::string_view kZsh = R"ZSH(#compdef umbriel
# zsh completion for umbriel: installed by `meson install`, or load it with
#   eval "$(umbriel completions zsh)"
_umbriel() {
  local -a lines values descriptions
  local line value description code=0
  lines=("${(@f)$(umbriel complete -- "${(@)words[2,-1]}" 2>/dev/null)}") || code=$?
  if ((code == 2)); then
    _path_files
    return
  fi
  for line in "${lines[@]}"; do
    [[ -n $line ]] || continue
    value=${line%%$'\t'*}
    description=''
    [[ $line == *$'\t'* ]] && description=${line#*$'\t'}
    values+=("$value")
    descriptions+=("$description")
  done
  ((${#values})) || return
  compadd -d descriptions -a values
}
# Run when zsh autoloads or calls the function; at the top level of an eval, register instead. compadd is only
# available inside a completion function, so calling it from the eval would fail.
if [[ $funcstack[1] == _umbriel ]]; then
  _umbriel "$@"
elif (( $+functions[compdef] )); then
  compdef _umbriel umbriel
fi
)ZSH";

    constexpr std::string_view kFish =
        R"FISH(# fish completion for umbriel: installed by `meson install`, or load it with
#   umbriel completions fish | source
function __umbriel_complete
    set -l tokens (commandline -opc)
    set -l current (commandline -ct)
    set -l lines (umbriel complete -- $tokens[2..-1] $current 2>/dev/null)
    set -l code $status
    if test $code -eq 2
        __fish_complete_path $current
        return
    end
    for line in $lines
        set -l parts (string split -m1 \t -- $line)
        if test (count $parts) -gt 1
            printf '%s\t%s\n' $parts[1] $parts[2]
        else
            printf '%s\n' $parts[1]
        end
    end
end
complete -c umbriel -f -a '(__umbriel_complete)'
)FISH";
  } // namespace

  std::string completionScript(CompletionShell shell) {
    switch (shell) {
    case CompletionShell::Bash:
      return std::string{kBash};
    case CompletionShell::Zsh:
      return std::string{kZsh};
    case CompletionShell::Fish:
      return std::string{kFish};
    }
    return {};
  }

} // namespace umbriel
