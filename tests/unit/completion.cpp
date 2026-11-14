// Completion is derived from the same tables `umbriel msg` and the CLI dispatch read. The closed vocabularies a
// position offers (regions, displays, layout modes, anchors, `skip-confirmation`) are asserted to parse through the
// real grammar, so a name renamed in one place fails here.

#include "check.h"
#include "cli/complete.h"
#include "config/keybind_parse.h"
#include "server/ipc.h"
#include "server/ipc_commands.h"
#include "view/window_region.h"

#include <algorithm>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  using umbriel::CompletionOutcome;
  using umbriel::CompletionValues;

  std::vector<std::string> complete(
      const std::vector<std::string>& words, const CompletionValues& values = {}, CompletionOutcome* outcome = nullptr
  ) {
    std::vector<std::string> candidates;
    const CompletionOutcome result =
        umbriel::completeWords(words, values, [&candidates](std::string_view value, std::string_view) {
          candidates.emplace_back(value);
        });
    if (outcome != nullptr) {
      *outcome = result;
    }
    return candidates;
  }

  bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
  }

  CompletionValues sampleValues() {
    CompletionValues values;
    values.outputs = {"DP-1", "HDMI-A-1"};
    values.windows = {"kitty:htop", "foot:shell"};
    values.workspaces = {"2", "web"};
    values.effectPresets = {"cinema", "glow"};
    values.effectPools = {"cinema", "motion"};
    values.scratchpads = {"music"};
    values.submaps = {"resize"};
    return values;
  }

  // A family that records being read, so a position can be checked against the values it actually uses.
  umbriel::CompletionList recording(std::vector<std::string>& reads, std::string name) {
    return umbriel::CompletionList([&reads, name = std::move(name)](std::vector<std::string>& target) {
      reads.push_back(name);
      target.push_back(name + ":1");
    });
  }

  // Every offered word must be accepted by the grammar it completes.
  void checkAllParse(
      std::string_view action, const std::vector<std::string>& candidates, std::vector<std::string> rest = {}
  ) {
    for (const std::string& candidate : candidates) {
      std::vector<std::string> words{std::string(action), candidate};
      words.insert(words.end(), rest.begin(), rest.end());
      umbriel::Keybind bind;
      if (!umbriel::parseActionWords(words, bind)) {
        std::println(stderr, "  candidate '{}' rejected for '{}'", candidate, action);
      }
      CHECK(umbriel::parseActionWords(words, bind));
    }
  }

} // namespace

UMBRIEL_TEST(firstWordOffersCommandsActionsAndFlags) {
  const std::vector<std::string> candidates = complete({""});
  CHECK(contains(candidates, "msg"));
  CHECK(contains(candidates, "subscribe"));
  CHECK(contains(candidates, "config"));
  CHECK(contains(candidates, "completions"));
  CHECK(contains(candidates, "complete"));
  CHECK(contains(candidates, "--version"));
  for (const umbriel::IpcCommandSpec& spec : umbriel::ipcCommands()) {
    // The Config group is reached as `config <sub>`, so its request names are not words the CLI accepts.
    if (spec.group == umbriel::IpcCommandGroup::Config) {
      CHECK(!contains(candidates, spec.name));
      continue;
    }
    CHECK(contains(candidates, spec.name));
  }
  CHECK(std::ranges::adjacent_find(candidates) == candidates.end());
}

UMBRIEL_TEST(configGroupCompletesTheWordsTheDispatchAccepts) {
  const std::vector<std::string> words = complete({"config", ""});
  CHECK(contains(words, "validate"));
  CHECK(contains(words, "schema"));
  for (const umbriel::IpcCommandSpec& spec : umbriel::ipcCommands()) {
    if (spec.group != umbriel::IpcCommandGroup::Config) {
      continue;
    }
    CHECK(contains(words, spec.name.substr(umbriel::kConfigCommandPrefix.size())));
  }

  // `config set` takes `<key> <value>`, so neither position may be completed from a family that is not a key.
  const std::vector<std::string> unset = complete({"config", "unset", ""}, sampleValues());
  CHECK(!contains(unset, "DP-1"));
}

UMBRIEL_TEST(firstWordFiltersOnPrefix) {
  const std::vector<std::string> candidates = complete({"win"});
  CHECK(contains(candidates, "windows"));
  CHECK(!contains(candidates, "msg"));
}

UMBRIEL_TEST(everyActionIsOfferedExactlyOnce) {
  const std::vector<std::string> candidates = complete({"msg", ""});
  CHECK_EQ(candidates.size(), umbriel::actionSpecs().size());
  CHECK(std::ranges::adjacent_find(candidates) == candidates.end());
  for (const umbriel::ActionSpec& spec : umbriel::actionSpecs()) {
    CHECK(contains(candidates, spec.name));
  }
}

UMBRIEL_TEST(actionPrefixFilters) {
  const std::vector<std::string> candidates = complete({"msg", "workspace-sw"});
  CHECK(contains(candidates, "workspace-switch"));
  CHECK(!contains(candidates, "window-close"));
}

UMBRIEL_TEST(msgJsonFlagsComeBeforeTheAction) {
  // `msg` takes its own `-j`/`--json` before the action, so the action word is still the one being completed.
  CHECK(contains(complete({"msg", "-"}), "--json"));
  CHECK(contains(complete({"msg", "-"}), "-j"));
  CHECK(contains(complete({"msg", "--json", "workspace-sw"}), "workspace-switch"));
  CHECK(!contains(complete({"msg", "--json", "workspace-sw"}), "window-close"));
  CHECK_EQ(complete({"msg", "-j", "overview-toggle", ""}), (std::vector<std::string>{}));
}

UMBRIEL_TEST(argumentVocabularyParsesThroughTheGrammar) {
  const CompletionValues values = sampleValues();

  const std::vector<std::string> regions = complete({"msg", "window-set-region", ""}, values);
  CHECK_EQ(regions.size(), umbriel::windowRegionNames().size());
  checkAllParse("window-set-region", regions);

  const std::vector<std::string> displays = complete({"msg", "column-set-display", ""}, values);
  CHECK(contains(displays, "normal"));
  CHECK(contains(displays, "tabbed"));
  checkAllParse("column-set-display", displays);

  const std::vector<std::string> modes = complete({"msg", "workspace-set-layout", ""}, values);
  CHECK(contains(modes, "toggle"));
  checkAllParse("workspace-set-layout", modes);

  const std::vector<std::string> anchors = complete({"msg", "window-move-to", ""}, values);
  CHECK_EQ(anchors.size(), umbriel::kWindowPositionAnchorNames.size());
  checkAllParse("window-move-to", anchors, {"0", "0"});

  const std::vector<std::string> quit = complete({"msg", "session-quit", ""}, values);
  CHECK_EQ(quit, std::vector<std::string>{std::string(umbriel::kSkipConfirmationWord)});
  checkAllParse("session-quit", quit);
}

UMBRIEL_TEST(toggleSequenceOffersEachRemainingModeOnce) {
  const std::vector<std::string> fresh = complete({"msg", "workspace-set-layout", "toggle", ""});
  CHECK_EQ(fresh.size(), umbriel::layoutModeNames().size());

  const std::vector<std::string> remaining = complete({"msg", "workspace-set-layout", "toggle", "master", ""});
  CHECK_EQ(remaining.size(), umbriel::layoutModeNames().size() - 1);
  CHECK(!contains(remaining, "master"));

  const std::vector<std::string> none = complete({"msg", "workspace-set-layout", "scrolling", ""});
  CHECK(none.empty());
}

UMBRIEL_TEST(valuesComeFromTheCallerAndParse) {
  const CompletionValues values = sampleValues();

  const std::vector<std::string> outputs = complete({"msg", "dpms-off", "DP"}, values);
  CHECK_EQ(outputs, std::vector<std::string>{"DP-1"});

  const std::vector<std::string> window = complete({"msg", "window-focus", "kitty"}, values);
  CHECK_EQ(window, std::vector<std::string>{"kitty:htop"});

  const std::vector<std::string> scratchpads = complete({"msg", "scratchpad-toggle", ""}, values);
  CHECK_EQ(scratchpads, std::vector<std::string>{"music"});

  const std::vector<std::string> submaps = complete({"msg", "submap", ""}, values);
  CHECK(contains(submaps, "resize"));
  CHECK(contains(submaps, "reset"));
}

UMBRIEL_TEST(implicitScratchpadIsOfferedWhenNoneIsConfigured) {
  const std::vector<std::string> candidates = complete({"msg", "scratchpad-toggle", ""});
  CHECK_EQ(candidates, std::vector<std::string>{"default"});
}

UMBRIEL_TEST(effectSetOffersPresetsPoolsAndOffWithoutDuplicates) {
  const CompletionValues values = sampleValues();
  const std::vector<std::string> candidates = complete({"msg", "effect-window-set", ""}, values);
  CHECK(contains(candidates, "cinema"));
  CHECK(contains(candidates, "glow"));
  CHECK(contains(candidates, "motion"));
  CHECK(contains(candidates, "off"));
  CHECK_EQ(std::ranges::count(candidates, "cinema"), 1);
}

UMBRIEL_TEST(effectCycleOffersTheNamesItCanCycleThrough) {
  const CompletionValues values = sampleValues();
  const std::vector<std::string> candidates = complete({"msg", "effect-window-cycle", ""}, values);
  CHECK(contains(candidates, "motion"));
  CHECK(contains(candidates, "glow"));
  // `off` suppresses a slot, which a cycle cannot select.
  CHECK(!contains(candidates, "off"));
}

UMBRIEL_TEST(flagsFollowTheGrammar) {
  const CompletionValues values = sampleValues();

  const std::vector<std::string> workspaceFlags = complete({"msg", "workspace-switch", "--"}, values);
  CHECK(contains(workspaceFlags, "--output"));
  CHECK(contains(workspaceFlags, "--name"));

  const std::vector<std::string> screenFlags = complete({"msg", "effect-screen-set", "--"}, values);
  CHECK_EQ(screenFlags, std::vector<std::string>{"--output"});

  CHECK(complete({"msg", "window-close", "--"}, values).empty());
}

UMBRIEL_TEST(aWorkspaceIsSelectedOnce) {
  const CompletionValues values = sampleValues();
  // `parseWorkspaceSelector` takes exactly one of a positional workspace or `--name`, so neither may follow the other.
  CHECK(complete({"msg", "workspace-switch", "--name", "web", ""}, values).empty());
  CHECK(!contains(complete({"msg", "workspace-switch", "web", "--"}, values), umbriel::kActionFlagNameWord));
  CHECK(contains(complete({"msg", "workspace-switch", "--"}, values), umbriel::kActionFlagNameWord));
  CHECK(contains(complete({"msg", "workspace-switch", ""}, values), "web"));
  CHECK(complete({"msg", "workspace-move-to-output", "DP-1", "--name", "web", ""}, values).empty());
  CHECK(complete({"msg", "workspace-switch", "web", "--name", ""}, values).empty());
  // A surplus positional is a line the parser refuses, so its trailing flag has no vocabulary either.
  CHECK(complete({"msg", "workspace-switch", "web", "extra", "--output", ""}, values).empty());
  CHECK(!complete({"msg", "workspace-move-to-output", "DP-1", ""}, values).empty());
  CHECK(complete({"msg", "workspace-move-to-output", "DP-1", "web", ""}, values).empty());
  CHECK(complete({"msg", "workspace-switch", "web", "--name", "web", "--output", ""}, values).empty());
}

UMBRIEL_TEST(usedFlagsAreNotOfferedAgain) {
  const CompletionValues values = sampleValues();
  const std::vector<std::string> candidates = complete({"msg", "workspace-switch", "--output", "DP-1", "--"}, values);
  CHECK_EQ(candidates, std::vector<std::string>{"--name"});
  // A flag twice is a line the parser refuses, so neither the flag nor its value is offered again.
  CHECK(complete({"msg", "workspace-switch", "--output", "DP-1", "--output", ""}, values).empty());
}

UMBRIEL_TEST(flagValuesCompleteFromTheMatchingFamily) {
  const CompletionValues values = sampleValues();
  CHECK_EQ(
      complete({"msg", "workspace-switch", "--output", ""}, values), (std::vector<std::string>{"DP-1", "HDMI-A-1"})
  );
  CHECK_EQ(
      complete({"msg", "window-move-to", "--window", ""}, values),
      (std::vector<std::string>{"kitty:htop", "foot:shell"})
  );
  CHECK_EQ(complete({"msg", "workspace-switch", "--name", ""}, values), (std::vector<std::string>{"2", "web"}));

  // A flag the action's grammar refuses gets no values, so accepting a line can never produce a rejected command.
  for (const auto& [action, flag] : std::vector<std::pair<std::string, std::string>>{
           {"window-close", "--output"},
           {"window-focus", "--output"},
           {"window-set-region", "--window"},
           {"workspace-switch", "--window"},
       }) {
    const std::vector<std::string> disallowed = complete({"msg", action, flag, ""}, values);
    CHECK(disallowed.empty());
  }

  // The refusal is not a positional shift: `workspace-move-to-output` reads a workspace second, and offering one after
  // a refused flag would produce a line the parser rejects.
  CHECK(complete({"msg", "workspace-move-to-output", "--window", ""}, values).empty());
}

UMBRIEL_TEST(spawnDelegatesToTheShell) {
  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"msg", "spawn", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::DelegateCommand);
  CHECK(complete({"msg", "spawn", "kit"}).empty());

  // A command line carries its own `--` words, which no action flag names.
  for (const std::vector<std::string>& words : std::vector<std::vector<std::string>>{
           {"msg", "spawn", "ls", "--color=auto", ""}, {"msg", "spawn", "kitty", "-e", "vim", ""}
       }) {
    CompletionOutcome delegated = CompletionOutcome::Candidates;
    complete(words, {}, &delegated);
    CHECK(delegated == CompletionOutcome::DelegateCommand);
  }
  CompletionOutcome flagPrefix = CompletionOutcome::Candidates;
  complete({"msg", "spawn", "--verb"}, {}, &flagPrefix);
  CHECK(flagPrefix == CompletionOutcome::DelegateCommand);
}

UMBRIEL_TEST(topLevelFlagsOwnTheirArguments) {
  // `-c` takes a configuration path and `-s` a command line; neither is followed by anything else, and a word the
  // dispatch does not know leaves no vocabulary at all.
  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"-c", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);
  complete({"-s", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::DelegateCommand);
  CHECK(complete({"-c", "umbriel.toml", ""}).empty());
  CHECK(complete({"-s", "kitty", ""}).empty());
  CHECK(complete({"bogus", ""}).empty());
}

UMBRIEL_TEST(argumentsStopWhenTheActionTakesNone) {
  CHECK(complete({"msg", "overview-toggle", ""}).empty());
  CHECK(complete({"msg", "overview-toggle", "x"}).empty());
}

UMBRIEL_TEST(subscribeCompletesEventsAndKeepsTheList) {
  const std::vector<std::string> all = complete({"subscribe", ""});
  CHECK_EQ(all.size(), umbriel::Ipc::kEventNames.size());
  for (const std::string_view name : umbriel::Ipc::kEventNames) {
    CHECK(contains(all, name));
  }
  const std::vector<std::string> chained = complete({"subscribe", "theme,win"});
  CHECK_EQ(chained, std::vector<std::string>{"theme,windows"});
}

UMBRIEL_TEST(configSubcommandsAndThePathFlag) {
  const std::vector<std::string> subcommands = complete({"config", ""});
  CHECK(contains(subcommands, "validate"));
  CHECK(contains(subcommands, "schema"));

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"config", "validate", "-c", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);

  CHECK_EQ(complete({"config", "schema", "--"}), std::vector<std::string>{"--json"});
}

UMBRIEL_TEST(completionsOffersItsShells) {
  CHECK_EQ(complete({"completions", ""}), (std::vector<std::string>{"bash", "zsh", "fish"}));
}

UMBRIEL_TEST(ipcCommandsCompleteTheirOwnSurface) {
  CHECK_EQ(complete({"windows", "-"}), (std::vector<std::string>{"--json", "-j"}));

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"config-replace", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);

  CHECK_EQ(complete({"input-capture-revoke", ""}), std::vector<std::string>{"all"});
}

UMBRIEL_TEST(valuesLoadOnlyWhenAPositionReadsThem) {
  std::vector<std::string> reads;
  CompletionValues values;
  values.outputs = recording(reads, "output");
  values.windows = recording(reads, "window");
  values.workspaces = recording(reads, "workspace");
  values.effectPresets = recording(reads, "preset");
  values.effectPools = recording(reads, "pool");
  values.scratchpads = recording(reads, "scratchpad");
  values.submaps = recording(reads, "submap");

  // A position whose words are compiled in asks for nothing, which is what keeps a session out of the static path.
  CHECK(!complete({"msg", "window-set-region", ""}, values).empty());
  CHECK(reads.empty());
  CHECK(complete({"msg", "overview-toggle", ""}, values).empty());
  CHECK(reads.empty());

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"msg", "spawn", "kitty", ""}, values, &outcome);
  CHECK(outcome == CompletionOutcome::DelegateCommand);
  CHECK(reads.empty());

  // A live position reads its own family, and only once however often the value is asked for.
  const std::vector<std::string> windows = complete({"msg", "window-close", ""}, values);
  CHECK_EQ(windows, std::vector<std::string>{"window:1"});
  CHECK_EQ(reads, std::vector<std::string>{"window"});
  complete({"msg", "window-focus", "--window", ""}, values);
  CHECK_EQ(reads, std::vector<std::string>{"window"});
}

UMBRIEL_TEST(configWordsCompleteTheWordBeforeTheCursor) {
  CHECK_EQ(complete({"config", "val"}), std::vector<std::string>{"validate"});
  CHECK_EQ(complete({"config", "s"}), (std::vector<std::string>{"schema", "set"}));

  // Every word of `config-replace` is a path, not just the first.
  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"config-replace", "/etc/umbriel.toml"}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);
}

UMBRIEL_TEST(scriptsDeferToTheBinary) {
  for (const umbriel::CompletionShell shell :
       {umbriel::CompletionShell::Bash, umbriel::CompletionShell::Zsh, umbriel::CompletionShell::Fish}) {
    const std::string script = umbriel::completionScript(shell);
    CHECK(!script.empty());
    CHECK(script.contains("umbriel complete"));
  }
  CHECK(umbriel::completionScript(umbriel::CompletionShell::Bash).contains("complete -F _umbriel umbriel"));
  const std::string zsh = umbriel::completionScript(umbriel::CompletionShell::Zsh);
  CHECK(zsh.contains("#compdef umbriel"));
  // zsh refuses compadd outside a completion function, so an eval must register instead of running the function body.
  CHECK(zsh.contains("[[ $funcstack[1] == _umbriel ]]"));
  CHECK(umbriel::completionScript(umbriel::CompletionShell::Fish).contains("complete -c umbriel"));
  // The callback hands a path over with exit 2 and a command line with exit 3, so each script needs both branches.
  CHECK(umbriel::completionScript(umbriel::CompletionShell::Bash).contains("code == 3"));
  CHECK(zsh.contains("code == 3"));
  CHECK(umbriel::completionScript(umbriel::CompletionShell::Fish).contains("code -eq 3"));
}

UMBRIEL_TEST(shellNamesRoundTrip) {
  CHECK(umbriel::completionShellNamed("bash") == umbriel::CompletionShell::Bash);
  CHECK(umbriel::completionShellNamed("zsh") == umbriel::CompletionShell::Zsh);
  CHECK(umbriel::completionShellNamed("fish") == umbriel::CompletionShell::Fish);
  CHECK(!umbriel::completionShellNamed("nushell"));
}

int main() { return RUN_TESTS(); }
