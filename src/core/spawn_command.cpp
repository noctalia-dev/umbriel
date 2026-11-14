#include "core/spawn_command.h"

#include <algorithm>
#include <cstddef>

namespace umbriel {

  namespace {

    constexpr std::string_view kShellSyntax = "\"'\\$`*?[{|}()&;<>#!~\n\r";

    // Words the shell interprets itself, so PATH never decides whether the command runs. This is the union of bash's
    // `enable -a` builtins, dash's builtins, and the reserved words, because `/bin/sh` is not the same shell
    // everywhere.
    constexpr std::string_view kShellBuiltins[] = {
        ".",         ":",        "[",       "alias",   "bg",       "bind",    "break",    "builtin", "caller",
        "case",      "cd",       "command", "compgen", "complete", "compopt", "continue", "coproc",  "declare",
        "dirs",      "disown",   "do",      "done",    "echo",     "elif",    "else",     "enable",  "esac",
        "eval",      "exec",     "exit",    "export",  "false",    "fc",      "fg",       "fi",      "for",
        "function",  "getopts",  "hash",    "help",    "history",  "if",      "in",       "jobs",    "kill",
        "let",       "local",    "logout",  "mapfile", "popd",     "printf",  "pushd",    "pwd",     "read",
        "readarray", "readonly", "return",  "select",  "set",      "shift",   "shopt",    "source",  "suspend",
        "test",      "then",     "time",    "times",   "trap",     "true",    "type",     "typeset", "ulimit",
        "umask",     "unalias",  "unset",   "until",   "wait",     "while",   "{",        "}",
    };

    bool isAlpha(char character) {
      return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
    }

    // A shell name: an alphabetic or underscore start followed by alphanumerics and underscores.
    bool isName(std::string_view word) {
      if (word.empty() || (!isAlpha(word.front()) && word.front() != '_')) {
        return false;
      }
      return std::ranges::all_of(word, [](char character) {
        return isAlpha(character) || (character >= '0' && character <= '9') || character == '_';
      });
    }

    // The variable an assignment sets, or empty for a word that is not one. `NAME+=value` appends, which bash accepts
    // and dash treats as an ordinary command word; either way the compositor cannot answer for it.
    std::string_view assignmentName(std::string_view word) {
      const std::size_t equals = word.find('=');
      if (equals == 0 || equals == std::string_view::npos) {
        return {};
      }
      std::string_view name = word.substr(0, equals);
      if (name.ends_with('+')) {
        name.remove_suffix(1);
      }
      return isName(name) ? name : std::string_view{};
    }

    // A leading `NAME=value` word is an assignment, not the program.
    bool isAssignment(std::string_view word) { return !assignmentName(word).empty(); }

    // An assignment to `PATH` changes where the shell looks for the program, so the compositor's PATH cannot answer
    // for it.
    bool setsPath(std::string_view word) { return assignmentName(word) == "PATH"; }

    bool isBuiltin(std::string_view word) {
      return std::ranges::find(kShellBuiltins, word) != std::ranges::end(kShellBuiltins);
    }

  } // namespace

  SpawnProgram spawnProgram(std::string_view command) {
    if (command.find_first_of(kShellSyntax) != std::string_view::npos) {
      return {.program = {}, .shell = true};
    }

    std::size_t start = command.find_first_not_of(" \t");
    while (start != std::string_view::npos) {
      std::size_t end = command.find_first_of(" \t", start);
      if (end == std::string_view::npos) {
        end = command.size();
      }
      const std::string_view word = command.substr(start, end - start);
      if (!isAssignment(word)) {
        if (isBuiltin(word)) {
          return {.program = {}, .shell = true};
        }
        return {.program = std::string(word), .shell = false};
      }
      if (setsPath(word)) {
        return {.program = {}, .shell = true};
      }
      start = command.find_first_not_of(" \t", end);
    }

    // Only assignments, or nothing at all: the shell decides what that means.
    return {.program = {}, .shell = true};
  }

} // namespace umbriel
