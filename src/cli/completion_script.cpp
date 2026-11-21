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
# Sets `_umbriel_words` to those words, `_umbriel_kept` to the part of the last one readline replaces (everything after
# the last unquoted, unescaped break character, or after a quote still open at the cursor), and `_umbriel_quote` to
# that open quote.
_umbriel_words() {
  local line=$1 char word= kept= open= escaped=0 quote=
  local -a words=()
  local i
  local breaks=${COMP_WORDBREAKS//[[:space:]\"\'\\]/}
  for ((i = 0; i < ${#line}; ++i)); do
    char=${line:i:1}
    if ((escaped)); then
      word+=$char
      kept+=$char
      escaped=0
    elif [[ $char == '\' ]]; then
      escaped=1
    elif [[ -n $quote ]]; then
      if [[ $char == "$quote" ]]; then
        quote=
      else
        word+=$char
        kept+=$char
        open+=$char
      fi
    elif [[ $char == "'" || $char == '"' ]]; then
      quote=$char
      open=
    elif [[ $char == ' ' || $char == $'\t' ]]; then
      if [[ -n $word ]]; then
        words+=("$word")
        word=
      fi
      kept=
    elif [[ -n $breaks && $breaks == *"$char"* ]]; then
      word+=$char
      kept=
    else
      word+=$char
      kept+=$char
    fi
  done
  words+=("$word")
  # An open quote is the only break character readline still holds, so what follows it is all it replaces; once the
  # quote is closed the word it made is what readline replaces whole.
  if [[ -n $quote ]]; then
    _umbriel_kept=$open
  else
    _umbriel_kept=$kept
  fi
  _umbriel_quote=$quote
  _umbriel_words=("${words[@]}")
}

# A value as the shell must re-read it: `%q` where the word is unquoted, the quote's own escapes inside an open
# double quote where a backslash is literal, and single quotes spliced in for the two characters the open quote
# cannot carry. The word ends closed, because readline adds its own closing quote only to a match that lacks one and
# then treats the word as finished.
_umbriel_escape() {
  local value=$1
  case "$_umbriel_quote" in
    '"')
      value=${value//\\/\\\\}
      value=${value//\"/\\\"}
      value=${value//\$/\\$}
      value=${value//\`/\\\`}
      # `!` starts history expansion even inside double quotes, and `\!` would keep its backslash.
      value=${value//!/\"\'!\'\"}
      [[ $value == *'"' ]] && value+='"'
      printf '%s' "$value"
      ;;
    "'")
      # An apostrophe would otherwise close the word before its end.
      value=${value//\'/\'\\\'\'}
      [[ $value == *"'" ]] && value+="'"
      printf '%s' "$value"
      ;;
    *) printf '%q' "$value" ;;
  esac
}

_umbriel() {
  local -a words=()
  local output code=0 value description
  _umbriel_words "${COMP_LINE:0:COMP_POINT}"
  words=("${_umbriel_words[@]}")
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
  # readline keeps the part of the word before the last unquoted, unescaped break character and replaces the rest, so
  # a candidate supplies only what is missing.
  local head=${words[-1]%"$_umbriel_kept"}
  while IFS=$'\t' read -r value description; do
    [[ -n $value ]] || continue
    [[ -n $head && $value == "$head"* ]] && value=${value#"$head"}
    COMPREPLY+=("$(_umbriel_escape "$value")")
  done <<<"$output"
  return 0
}
complete -F _umbriel umbriel
)BASH";

    constexpr std::string_view kZsh = R"ZSH(#compdef umbriel
# zsh completion for umbriel: installed by `meson install`, or load it with
#   eval "$(umbriel completions zsh)"
_umbriel() {
  local -a lines values displays args
  local line value description code=0
  # `(Q)` removes the quoting zsh keeps in `words`, and `CURRENT` is the word the cursor is on, so a value with a
  # space or a colon continues from where it left off and a cursor moved back completes the word it is on.
  args=("${(@Q)words[2,CURRENT]}")
  # `(Q)` cannot dequote a word whose quote is still open at the cursor, and the binary matches on the word as the
  # shell reads it, so the one quote it leaves open is dropped; a quote the value carries inside stays.
  if [[ -n $compstate[quote] ]]; then
    if [[ $args[-1] == $compstate[quote]* ]]; then
      args[-1]=${args[-1]#$compstate[quote]}
    else
      args[-1]=${args[-1]%$compstate[quote]}
    fi
  fi
  lines=("${(@f)$(umbriel complete -- "${args[@]}" 2>/dev/null)}") || code=$?
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
    # `_describe` splits both arrays on an unescaped `:`, so a value that carries one (every window id is
    # `app_id:title`) is escaped in the match array as well as in the description entry.
    values+=("${value//:/\\:}")
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
