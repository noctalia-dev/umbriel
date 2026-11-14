#pragma once

#include <string>
#include <string_view>

namespace umbriel {

  // The executable a spawn command names, when the command can be checked without running it.
  struct [[nodiscard]] SpawnProgram {
    // The program word to resolve. Empty when the command names nothing checkable.
    std::string program;
    // The command needs a shell to decide what runs (quoting, expansion, globbing, redirection, composition,
    // assignment-only, a shell builtin, or an assignment to `PATH`), so its program cannot be resolved ahead of
    // time.
    bool shell = false;
  };

  // Split a spawn command into the program it names and whether a shell is required to interpret it.
  //
  // Only a bare invocation is checked: any quoting, expansion, globbing, redirection, composition, or comment
  // character makes the whole command the shell's business. Leading `NAME=value` and `NAME+=value` words are
  // assignments and are skipped, unless they assign `PATH`, which changes where the shell resolves the program. A
  // shell builtin or keyword reports `shell`, because its absence from PATH says nothing about whether the command
  // runs.
  SpawnProgram spawnProgram(std::string_view command);

} // namespace umbriel
