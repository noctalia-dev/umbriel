#include "cli/complete.h"

namespace umbriel {

  namespace {
    // Every script defers to `umbriel complete`, so the vocabulary stays in the binary and cannot drift from the
    // tables. Exit 2 asks the shell for a path, exit 3 for a command name.
    constexpr std::string_view kBash =
        R"BASH(# bash completion for umbriel: installed by `meson install`, or load it with
#   eval "$(umbriel completions bash)"
# The words up to the cursor, as the line spells them: readline splits COMP_WORDS on `COMP_WORDBREAKS`, which
# includes `:`, and leaves backslash escapes in place, so neither reaches `umbriel complete` intact otherwise.
_umbriel_words() {
  local line=$1 char word= escaped=0 quote=
  local -a words=()
  local i
  for ((i = 0; i < ${#line}; ++i)); do
    char=${line:i:1}
    if ((escaped)); then
      word+=$char
      escaped=0
    elif [[ $char == '\' ]]; then
      escaped=1
    elif [[ -n $quote ]]; then
      [[ $char == "$quote" ]] && quote= || word+=$char
    elif [[ $char == "'" || $char == '"' ]]; then
      quote=$char
    elif [[ $char == ' ' || $char == $'\t' ]]; then
      if [[ -n $word ]]; then
        words+=("$word")
        word=
      fi
    else
      word+=$char
    fi
  done
  words+=("$word")
  printf '%s\n' "${words[@]}"
}

_umbriel() {
  local -a candidates=()
  local -a words=()
  local output code=0 value description spaced=0
  mapfile -t words < <(_umbriel_words "${COMP_LINE:0:COMP_POINT}")
  output=$(umbriel complete -- "${words[@]:1}" 2>/dev/null) || code=$?
  if ((code == 2)); then
    compopt -o default 2>/dev/null || true
    return 0
  fi
  if ((code == 3)); then
    mapfile -t COMPREPLY < <(compgen -c -- "${COMP_WORDS[COMP_CWORD]}" | sort -u)
    return 0
  fi
  COMPREPLY=()
  while IFS=$'\t' read -r value description; do
    [[ -n $value ]] || continue
    candidates+=("$value")
    [[ $value == *[[:space:]\\]* ]] && spaced=1
  done <<<"$output"
  ((${#candidates[@]} > 0)) || return 0
  COMPREPLY=("${candidates[@]}")
  # A value with a space or a quote needs readline's quoting; without such a candidate the filenames option would
  # append a slash to a name that happens to match a directory instead of ending the word.
  ((spaced)) && compopt -o filenames 2>/dev/null
  return 0
}
complete -F _umbriel umbriel
)BASH";

    constexpr std::string_view kZsh = R"ZSH(#compdef umbriel
# zsh completion for umbriel: installed by `meson install`, or load it with
#   eval "$(umbriel completions zsh)"
_umbriel() {
  local -a lines values displays
  local line value description code=0
  # `(Q)` removes the quoting zsh keeps in `words`, and `CURRENT` is the word the cursor is on, so a value with a
  # space or a colon continues from where it left off and a cursor moved back completes the word it is on.
  lines=("${(@f)$(umbriel complete -- "${(@Q)words[2,CURRENT]}" 2>/dev/null)}") || code=$?
  if ((code == 2)); then
    _path_files
    return
  fi
  if ((code == 3)); then
    _command_names
    return
  fi
  for line in "${lines[@]}"; do
    [[ -n $line ]] || continue
    value=${line%%$'\t'*}
    description=''
    [[ $line == *$'\t'* ]] && description=${line#*$'\t'}
    values+=("$value")
    # `_describe` reads each entry of its first array as `completion:description` and takes the second array as the
    # completions to add, so the value stays the match and the description is only how the menu annotates it. A colon
    # in the value is escaped because it separates the two.
    displays+=("${value//:/\\:}:$description")
  done
  ((${#values})) || return
  _describe -V 'umbriel' displays values
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
    # `-o` unescapes the tokens it returns, so the token under the cursor needs the same treatment before it reaches
    # `umbriel complete`, whose values may carry the space the shell escaped.
    set -l current (string unescape -- (commandline -ct))
    set -l lines (umbriel complete -- $tokens[2..-1] $current 2>/dev/null)
    set -l code $status
    if test $code -eq 2
        __fish_complete_path $current
        return
    end
    if test $code -eq 3
        __fish_complete_command $current
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
