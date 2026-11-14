#include "core/spawn_command.h"

#include "check.h"

#include <string_view>

using umbriel::SpawnProgram;
using umbriel::spawnProgram;

namespace {

  void checkPlain(std::string_view command, std::string_view expected) {
    const SpawnProgram parsed = spawnProgram(command);
    CHECK(!parsed.shell);
    CHECK_EQ(parsed.program, std::string(expected));
  }

  void checkShell(std::string_view command) {
    const SpawnProgram parsed = spawnProgram(command);
    CHECK(parsed.shell);
    CHECK(parsed.program.empty());
  }

} // namespace

UMBRIEL_TEST(aBareProgramIsResolved) {
  checkPlain("kitty", "kitty");
  checkPlain("/usr/bin/kitty", "/usr/bin/kitty");
  checkPlain("./launch.sh", "./launch.sh");
}

UMBRIEL_TEST(argumentsAfterTheProgramDoNotMatter) {
  checkPlain("kitty --single-instance", "kitty");
  checkPlain("foot -e tmux new -A -s main", "foot");
  checkPlain("  kitty   --hold  ", "kitty");
}

UMBRIEL_TEST(leadingAssignmentsAreSkipped) {
  checkPlain("GDK_SCALE=2 kitty", "kitty");
  checkPlain("A=1 B_2=three kitty -e sh", "kitty");
  // A word with an invalid name is the program, not an assignment.
  checkPlain("2kitty=1 kitty", "2kitty=1");
}

UMBRIEL_TEST(shellSyntaxDefersToTheShell) {
  checkShell("grim -g \"$(slurp)\" shot.png");
  checkShell("kitty -e sh -c 'echo hi'");
  checkShell("sh -c $COMMAND");
  checkShell("kitty *.png");
  checkShell("a | b");
  checkShell("a && b");
  checkShell("a; b");
  checkShell("kitty > /tmp/out");
  checkShell("kitty &");
  checkShell("kitty # note");
  checkShell("$(which kitty)");
  checkShell("kitty ~/file");
  checkShell("kitty `date`");
  checkShell("kitty\necho next");
}

UMBRIEL_TEST(aPathAssignmentDefersToTheShell) {
  // The shell resolves the program against the assignment, so the compositor's PATH cannot answer for it.
  checkShell("PATH=/tmp/bin kitty");
  checkShell("PATH=/opt/bin:/usr/bin foo-app");
  checkShell("PATH+=/opt/bin kitty");
  checkShell("PATH+=-wrapped kitty");
  // Any other assignment leaves PATH alone and the program stays checkable.
  checkPlain("FOO=1 PATH_BACKUP=/tmp kitty", "kitty");
  checkPlain("PATHX+=/tmp kitty", "kitty");
}

UMBRIEL_TEST(aBuiltinIsTheShellsBusiness) {
  checkShell("echo hello");
  checkShell("true");
  checkShell("cd /tmp");
  checkShell("exec kitty");
  checkShell("coproc sleep 5");
  // Builtins and reserved words that are absent from PATH: refusing them would break commands that used to work.
  checkShell("umask 022");
  checkShell("export FOO=1");
  checkShell("set -e");
  checkShell("source ~/.profile");
  checkShell(". /etc/profile");
  checkShell("[ -d /tmp ]");
  checkShell("readonly X=1");
  checkShell("trap '' TERM");
  checkShell("wait");
  checkShell("times");
  checkShell("shift");
  checkShell("ulimit -n");
  checkShell("type kitty");
  checkShell("hash -r");
  checkShell("kill -TERM 1234");
  checkShell("let x=1");
  checkShell("declare -r X=1");
}

UMBRIEL_TEST(anEmptyOrAssignmentOnlyCommandNamesNothing) {
  checkShell("");
  checkShell("   ");
  checkShell("FOO=bar");
}

int main() { return RUN_TESTS(); }
