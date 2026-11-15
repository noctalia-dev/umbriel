// Completion is derived from the same tables `umbriel msg` and the CLI dispatch read. The closed vocabularies a
// position offers (regions, displays, layout modes, anchors, `skip-confirmation`) are asserted to parse through the
// real grammar, so a name renamed in one place fails here. The generated bash script is driven in a real shell
// against a stub `umbriel`, so what it hands readline is checked as behaviour rather than as text.

#include "check.h"
#include "cli/cli_words.h"
#include "cli/complete.h"
#include "config/keybind_parse.h"
#include "input/input_capture.h"
#include "server/ipc.h"
#include "server/ipc_commands.h"
#include "view/window_region.h"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
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
    values.effects = umbriel::EffectCandidates([](std::vector<std::string>& presets, std::vector<std::string>& pools,
                                                  umbriel::EffectKind kind) {
      if (kind == umbriel::EffectKind::Window) {
        presets = {"cinema", "glow"};
        pools = {"cinema", "motion"};
      }
    });
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

UMBRIEL_TEST(effectCycleOffersOnlyThePoolsItCanCycleThrough) {
  const CompletionValues values = sampleValues();
  const std::vector<std::string> candidates = complete({"msg", "effect-window-cycle", ""}, values);
  CHECK(contains(candidates, "motion"));
  // A cycle resolves a pool, so a preset would be refused; `off` suppresses a slot, which a cycle cannot select.
  CHECK(!contains(candidates, "glow"));
  CHECK(!contains(candidates, "off"));
}

UMBRIEL_TEST(effectPositionsReadTheSlotKind) {
  std::vector<umbriel::EffectKind> asked;
  CompletionValues values;
  values.effects = umbriel::EffectCandidates(
      [&asked](std::vector<std::string>& presets, std::vector<std::string>& pools, umbriel::EffectKind kind) {
        asked.push_back(kind);
        presets = {std::string{umbriel::effectKindName(kind)}};
        pools = {std::string{umbriel::effectKindName(kind)}};
      }
  );
  // Each slot reads names of its own kind, and the border slot is its own kind rather than the window one.
  const auto first = [&values](const char* action) {
    const std::vector<std::string> names = complete({"msg", action, ""}, values);
    return names.empty() ? std::string{} : names.front();
  };
  CHECK_EQ(first("effect-window-set"), std::string{"window"});
  CHECK_EQ(first("effect-border-set"), std::string{"border"});
  CHECK_EQ(first("effect-screen-set"), std::string{"screen"});
  CHECK_EQ(first("effect-cursor-set"), std::string{"cursor"});
  CHECK(!asked.empty());
  for (const umbriel::EffectKind kind : asked) {
    CHECK(kind != umbriel::EffectKind::Animation);
  }
  // Each kind is filled once, however many positions ask for it.
  const auto count = std::ranges::count(asked, umbriel::EffectKind::Window);
  CHECK_EQ(count, 1);
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
  // The arity is the parser's for every kind, not only the workspace ones.
  CHECK(complete({"msg", "output-sdr-white", "200", "extra", "--"}, values).empty());
  CHECK(complete({"msg", "effect-screen-set", "vignette", "extra", "--"}, values).empty());
  CHECK(!complete({"msg", "window-move-to", "top_left", "10", "20", "--"}, values).empty());
  CHECK(complete({"msg", "window-move-to", "top_left", "10", "20", "extra", "--"}, values).empty());
  // `--output` is only reachable once a workspace selector is on the line.
  CHECK(!contains(complete({"msg", "workspace-move-to-output", "--"}, values), umbriel::kActionFlagOutputWord));
  CHECK(!contains(complete({"msg", "workspace-move-to-output", "DP-1", "--"}, values), umbriel::kActionFlagOutputWord));
  CHECK(contains(
      complete({"msg", "workspace-move-to-output", "DP-1", "--name", "web", "--"}, values),
      umbriel::kActionFlagOutputWord
  ));
  CHECK_EQ(
      complete({"msg", "workspace-move-to-output", "DP-1", "web", "--output", ""}, values),
      (std::vector<std::string>{"DP-1", "HDMI-A-1"})
  );
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
  // The first tail word is a command name; once it is given, the rest of the line belongs to that command.
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
    CHECK(delegated == CompletionOutcome::Delegate);
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
  // The two startup flags may appear in either order, each with one value, and nothing else follows them.
  CHECK_EQ(complete({"-c", "umbriel.toml", ""}), std::vector<std::string>{"-s"});
  CHECK_EQ(complete({"-s", "kitty", ""}), std::vector<std::string>{"-c"});
  CHECK(complete({"-c", "umbriel.toml", "-s", "kitty", ""}).empty());
  CHECK(complete({"bogus", ""}).empty());
  // The walk consumes each flag and its value as a pair, so a value that spells a flag stays a value.
  CHECK_EQ(complete({"-c", "-s", ""}), std::vector<std::string>{"-s"});
  CHECK_EQ(complete({"-s", "-c", ""}), std::vector<std::string>{"-c"});
  // A flag already on the line is not offered again, and a word the dispatch refuses leaves nothing.
  CHECK(complete({"-c", "a.toml", "-c", ""}).empty());
  CHECK(complete({"-c", "a.toml", "bogus", ""}).empty());
}

UMBRIEL_TEST(valuesTheWordGrammarRefusesAreNotOffered) {
  CompletionValues values;
  // A configured name the parser treats as a flag, and a submap name it refuses outright, are not candidates.
  values.outputs = {"eDP-1", "--x"};
  values.submaps = {"music", "disable", "--flag"};
  CHECK_EQ(complete({"msg", "dpms-off", ""}, values), std::vector<std::string>{"eDP-1"});
  CHECK_EQ(
      complete({"msg", "submap", ""}, values),
      (std::vector<std::string>{"music", std::string{umbriel::kSubmapResetWord}})
  );
}

UMBRIEL_TEST(digitOnlyWorkspaceNamesCompleteOnlyByName) {
  CompletionValues values;
  values.workspaces = {"0", "2", "web", "--flag"};
  // A positional word of digits is an index, never the name of the workspace at that index, and `--flag` is a flag
  // word, so neither can stand where the grammar expects a selector.
  CHECK_EQ(complete({"msg", "workspace-switch", ""}, values), std::vector<std::string>{"web"});
  // Ones given after `--name` are taken verbatim, so every name there is a candidate.
  CHECK_EQ(
      complete({"msg", "workspace-switch", "--name", ""}, values), (std::vector<std::string>{"0", "2", "web", "--flag"})
  );
}

UMBRIEL_TEST(flagValuesKeepTheWordsAPositionalRefuses) {
  CompletionValues values;
  values.outputs = {"DP-1", "--x"};
  values.windows = {"kitty:htop", "--wid"};
  CHECK_EQ(complete({"msg", "dpms-off", ""}, values), std::vector<std::string>{"DP-1"});
  CHECK_EQ(complete({"msg", "output-sdr-white", "--output", ""}, values), (std::vector<std::string>{"DP-1", "--x"}));
  CHECK_EQ(
      complete({"msg", "window-move-to", "--window", ""}, values), (std::vector<std::string>{"kitty:htop", "--wid"})
  );
}

UMBRIEL_TEST(moveToOutputOffersTheCurrentDestination) {
  CompletionValues values;
  values.outputs = {"DP-1"};
  CHECK_EQ(
      complete({"msg", "workspace-move-to-output", ""}, values),
      (std::vector<std::string>{"DP-1", std::string{umbriel::kCurrentOutputWord}})
  );
}

UMBRIEL_TEST(spawnTakesAnyCommandWordAsTheCommandName) {
  // A command name is the first completed tail word even when it begins with `--`, which no action flag names.
  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"msg", "spawn", "--json", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);
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
  // `subscribe` accepts the JSON flags ahead of its events, so a dash position offers them.
  CHECK_EQ(complete({"subscribe", "-"}), (std::vector<std::string>{"--json", "-j"}));
}

UMBRIEL_TEST(configSubcommandsAndThePathFlag) {
  const std::vector<std::string> subcommands = complete({"config", ""});
  CHECK(contains(subcommands, "validate"));
  CHECK(contains(subcommands, "schema"));

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"config", "validate", "-c", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);

  CHECK_EQ(complete({"config", "schema", "-"}), (std::vector<std::string>{"--json", "-j"}));
}

UMBRIEL_TEST(configValidateKeepsItsFlagsAfterAValue) {
  // `validateConfig` takes `-c` and the JSON flags in any order, so every position after a value still completes.
  CHECK(contains(complete({"config", "validate", "--json", ""}), umbriel::kCliConfigFlag));
  CHECK(contains(complete({"config", "validate", "-c", "umbriel.toml", ""}), umbriel::kCliJsonFlag));
  CHECK(contains(complete({"config", "validate", "-j", ""}), umbriel::kCliConfigFlag));

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"config", "validate", "-c", ""}, {}, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);
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
  // The dispatch strips the JSON flags before reading arguments, so they do not shift the argument position.
  CHECK_EQ(complete({"input-capture-revoke", "--json", ""}), std::vector<std::string>{"all"});
  // `config unset` takes `--all` beside `key`; a dash-prefixed choice completes with the flags.
  CHECK(contains(complete({"config", "unset", "-"}), "--all"));

  // The Config group is reached as `config <sub>`; a request name on its own is a line the dispatch refuses.
  CHECK(complete({"config-set", "--"}).empty());
}

UMBRIEL_TEST(closedVocabulariesCompleteTheirWords) {
  // A dash-prefixed choice such as `config unset --all` is an argument, so it completes on an empty word.
  CHECK_EQ(complete({"config", "unset", ""}), (std::vector<std::string>{std::string{umbriel::kUnsetAllWord}}));
  CHECK_EQ(complete({"input-capture-revoke", ""}), (std::vector<std::string>{std::string{umbriel::kRevokeAllWord}}));

  const CompletionValues values = sampleValues();
  CHECK_EQ(complete({"output-destroy", ""}, values), (std::vector<std::string>{"DP-1", "HDMI-A-1"}));
  // The harness vocabularies are closed, so each word the parser reads is offered.
  CHECK_EQ(complete({"animation-capture-fail", ""}), (std::vector<std::string>{"on", "off"}));
  CHECK_EQ(complete({"event-pacing-hold", ""}), (std::vector<std::string>{"on", "off"}));
  CHECK_EQ(complete({"blur-composite-split", ""}), (std::vector<std::string>{"on", "off"}));
  CHECK_EQ(complete({"swipe-test", ""}), (std::vector<std::string>{"begin", "update", "end"}));
  // A command that reads its words in order offers nothing past the first.
  CHECK(complete({"output-destroy", "DP-1", ""}, values).empty());
  CHECK(complete({"swipe-test", "begin", ""}).empty());
  // A command that takes no argument leaves no position once one arrives, so its flags stop completing too.
  CHECK(complete({"windows", "bogus", "-"}).empty());
  CHECK(complete({"input-capture-revoke", "all", "extra", "-"}).empty());
}

UMBRIEL_TEST(valuesLoadOnlyWhenAPositionReadsThem) {
  std::vector<std::string> reads;
  CompletionValues values;
  values.outputs = recording(reads, "output");
  values.windows = recording(reads, "window");
  values.workspaces = recording(reads, "workspace");
  values.effects = umbriel::EffectCandidates(
      [&reads](std::vector<std::string>& presets, std::vector<std::string>& pools, umbriel::EffectKind) {
        reads.emplace_back("effect preset");
        reads.emplace_back("effect pool");
        presets.emplace_back("preset:1");
        pools.emplace_back("pool:1");
      }
  );
  values.scratchpads = recording(reads, "scratchpad");
  values.submaps = recording(reads, "submap");

  // A position whose words are compiled in asks for nothing, which is what keeps a session out of the static path.
  CHECK(!complete({"msg", "window-set-region", ""}, values).empty());
  CHECK(reads.empty());
  CHECK(complete({"msg", "overview-toggle", ""}, values).empty());
  CHECK(reads.empty());

  CompletionOutcome outcome = CompletionOutcome::Candidates;
  complete({"msg", "spawn", "kitty", ""}, values, &outcome);
  CHECK(outcome == CompletionOutcome::Delegate);
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

std::vector<std::string> lines(std::string_view text) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      result.emplace_back(text.substr(start));
      break;
    }
    result.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

// `[a][b]` reads as clearly as an empty list does, where a bare vector would print as unprintable.
std::string joined(const std::vector<std::string>& values) {
  std::string text;
  for (const std::string& value : values) {
    text += '[' + value + ']';
  }
  return text;
}

// Drives the generated bash script in a real shell. A stub `umbriel` first on PATH records the words it is handed
// and prints the reply, so an assertion covers what the script gives readline instead of the script's own text.
class BashCompletion {
public:
  struct Case {
    // `line` is the line up to the cursor (COMP_LINE), `reply` the `<value>\t<description>` lines the real binary
    // would print, `status` the code it would exit with, and `cursorWord` the COMP_WORDS[COMP_CWORD] the
    // command-name delegation reads.
    Case(std::string line, std::string reply, int status = 0, std::string cursorWord = {})
        : line(std::move(line)), reply(std::move(reply)), status(status), cursorWord(std::move(cursorWord)) {}

    std::string line;
    std::string reply;
    int status;
    std::string cursorWord;
  };

  struct Result {
    std::vector<std::string> candidates; // COMPREPLY
    std::string words;                   // the arguments the stub was called with
  };

  BashCompletion() {
    char pattern[] = "/tmp/umbriel-completion-XXXXXX";
    const char* created = mkdtemp(pattern);
    CHECK(created != nullptr);
    if (created == nullptr) {
      std::abort();
    }
    m_directory = created;
    const char* inherited = std::getenv("PATH");
    m_path = m_directory.string() + ':' + (inherited != nullptr ? inherited : "/usr/bin:/bin");
    writeFile("completion.bash", umbriel::completionScript(umbriel::CompletionShell::Bash));
  }

  ~BashCompletion() {
    std::error_code ignored;
    std::filesystem::remove_all(m_directory, ignored);
  }

  Result complete(const Case& request) {
    writeFile("reply", request.reply);
    writeFile(
        "umbriel",
        std::format(
            "#!/bin/sh\nprintf '%s' \"$*\" > '{0}/argv'\ncat '{0}/reply'\nexit {1}\n", m_directory.string(),
            request.status
        )
    );
    std::filesystem::permissions(m_directory / "umbriel", std::filesystem::perms::owner_all);
    std::string command = "source '" + (m_directory / "completion.bash").string() + "'";
    if (!request.cursorWord.empty()) {
      command += std::format("; COMP_WORDS=(umbriel {}); COMP_CWORD=1", request.cursorWord);
    }
    // The count first, so an empty `COMPREPLY` is distinct from one holding an empty candidate.
    command += R"(; _umbriel; printf '%s\n' "${#COMPREPLY[@]}"; printf '[%s]\n' "${COMPREPLY[@]}")";
    const std::vector<std::string> output = lines(shell(command, request));
    const std::size_t count =
        output.empty() ? 0 : static_cast<std::size_t>(std::strtoul(output.front().c_str(), nullptr, 10));
    std::vector<std::string> candidates;
    for (std::size_t index = 1; index < output.size() && candidates.size() < count; ++index) {
      const std::string& line = output[index];
      candidates.emplace_back(line.size() >= 2 ? line.substr(1, line.size() - 2) : std::string());
    }
    return {candidates, readFile("argv")};
  }

  // The registration readline dispatches through.
  std::string registered() const {
    return shell("source '" + (m_directory / "completion.bash").string() + "'; complete -p umbriel", {"", ""});
  }

private:
  std::string shell(const std::string& command, const Case& request) const {
    int output[2]{-1, -1};
    if (pipe2(output, O_CLOEXEC) < 0) {
      return {};
    }
    const pid_t child = fork();
    if (child == 0) {
      close(output[0]);
      if (dup2(output[1], STDOUT_FILENO) < 0) {
        _exit(1);
      }
      close(output[1]);
      setenv("COMP_LINE", request.line.c_str(), 1);
      setenv("COMP_POINT", std::to_string(request.line.size()).c_str(), 1);
      setenv("COMP_WORDBREAKS", " \t\n\"'><=;|&(:", 1);
      setenv("PATH", m_path.c_str(), 1);
      execlp("bash", "bash", "--norc", "--noprofile", "-c", command.c_str(), nullptr);
      _exit(1);
    }
    close(output[1]);
    output[1] = -1;
    if (child < 0) {
      close(output[0]);
      return {};
    }
    std::string text;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(output[0], buffer, sizeof buffer)) > 0) {
      text.append(buffer, static_cast<std::size_t>(count));
    }
    close(output[0]);
    int status = 0;
    CHECK_EQ(waitpid(child, &status, 0), child);
    CHECK(WIFEXITED(status));
    return text;
  }

  void writeFile(const std::string& name, const std::string& contents) {
    std::ofstream stream(m_directory / name, std::ios::binary | std::ios::trunc);
    stream << contents;
    CHECK(stream.good());
  }

  std::string readFile(const std::string& name) const {
    std::ifstream stream(m_directory / name);
    std::string text;
    std::getline(stream, text);
    return text;
  }

  std::filesystem::path m_directory;
  std::string m_path;
};

UMBRIEL_TEST(scriptsDeferToTheBinary) {
  // zsh and fish are absent from the image the tests run in, so their deferred positions and format contracts are
  // pinned by the call each one makes; bash is driven end to end below.
  const std::string zsh = umbriel::completionScript(umbriel::CompletionShell::Zsh);
  CHECK(!zsh.empty());
  CHECK(zsh.contains("umbriel complete"));
  CHECK(zsh.contains("#compdef umbriel"));
  // zsh refuses compadd outside a completion function, so an eval must register instead of running the function body.
  CHECK(zsh.contains("[[ $funcstack[1] == _umbriel ]]"));
  CHECK(zsh.contains("_path_files"));
  CHECK(zsh.contains("_command_names"));
  // `_describe` splits both of its arrays on an unescaped `:`, so a window id would otherwise be cut at its colon.
  CHECK(zsh.contains("values+=(\"${value//:/\\\\:}\")"));
  const std::string fish = umbriel::completionScript(umbriel::CompletionShell::Fish);
  CHECK(!fish.empty());
  CHECK(fish.contains("umbriel complete"));
  CHECK(fish.contains("complete -c umbriel"));
  CHECK(fish.contains("__fish_complete_path"));
  CHECK(fish.contains("__fish_complete_command"));
  // `-ct` leaves the escape the shell inserted in place, so the current token is unescaped before it is sent.
  CHECK(fish.contains("string unescape"));
  // Path delegation runs through compopt, which exists only inside a completion invocation.
  CHECK(umbriel::completionScript(umbriel::CompletionShell::Bash).contains("compopt -o default"));
}

UMBRIEL_TEST(bashScriptReplacesOnlyWhatReadlineDoes) {
  BashCompletion bash;
  CHECK_EQ(bash.registered(), "complete -F _umbriel umbriel\n");

  // readline replaces only the part after the last unquoted break character, which includes `:`, so a value supplies
  // the rest and the prefix the line already holds stays.
  const BashCompletion::Result plain = bash.complete({"umbriel msg window-focus kitty:h", "kitty:htop\thtop"});
  CHECK_EQ(joined(plain.candidates), "[htop]");
  CHECK_EQ(plain.words, "complete -- msg window-focus kitty:h");

  // A quote open at the cursor hides the break characters inside it, so the whole quoted value is replaced.
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus \"kitty:my", "kitty:my title\ttitle"}).candidates),
      "[kitty:my title]"
  );
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus 'kitty:my", "kitty:my title\ttitle"}).candidates),
      "[kitty:my title]"
  );

  const BashCompletion::Result escaped = bash.complete({"umbriel msg window-focus kitty\\:h", "kitty:htop\thtop"});
  // An escaped break character is quoted, so readline replaces the whole word and the value has to arrive complete.
  CHECK_EQ(joined(escaped.candidates), "[kitty:htop]");
  CHECK_EQ(escaped.words, "complete -- msg window-focus kitty:h");

  // A value the shell would re-read differently is quoted for the context it lands in.
  CHECK_EQ(joined(bash.complete({"umbriel msg window-focus Don", "Don't\ttitle"}).candidates), "[Don\\'t]");
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus foot:s", "foot:shell one\ttitle"}).candidates), "[shell\\ one]"
  );
  CHECK_EQ(joined(bash.complete({"umbriel msg window-focus \"a", "a\"b$c\ttitle"}).candidates), "[a\\\"b\\$c]");
  // Neither character can be carried by the quote that is open: `!` because it starts history expansion inside
  // double quotes, and an apostrophe because it would close a single-quoted word early.
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus \"kitty:hello", "kitty:hello!world\ttitle"}).candidates),
      "[kitty:hello\"'!'\"world]"
  );
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus 'kitty:Don", "kitty:Don't\ttitle"}).candidates),
      "[kitty:Don'\\''t]"
  );
  // The word has to end closed: readline closes only a match that lacks a closing quote, and then ends the word, so a
  // value whose last character leaves the quote open would take the space readline appends with it.
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus \"kitty:hel", "kitty:hello!\ttitle"}).candidates),
      "[kitty:hello\"'!'\"\"]"
  );
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus 'kitty:Don", "kitty:Don'\ttitle"}).candidates),
      "[kitty:Don'\\''']"
  );
  // A closed quote makes one word of everything inside and after it, so the whole value replaces the word.
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus \"kitty:Don't\"", "kitty:Don't\ttitle"}).candidates),
      "[kitty:Don\\'t]"
  );
  // A second TAB reads the quotes the first one inserted, so the value continues after them.
  CHECK_EQ(
      joined(bash.complete({"umbriel msg window-focus \"kitty:hello\"'!'\"",
                            "kitty:hello!world\ttitle\nkitty:hello!there\ttitle"})
                 .candidates),
      "[world][there]"
  );

  // Exit 2 leaves paths to readline, exit 3 takes the command names from the shell's own list.
  CHECK(bash.complete({"umbriel config-replace /e", "", 2}).candidates.empty());
  CHECK(contains(bash.complete({"umbriel spawn umb", "", 3, "umb"}).candidates, "umbriel"));
}

UMBRIEL_TEST(shellNamesRoundTrip) {
  CHECK(umbriel::completionShellNamed("bash") == umbriel::CompletionShell::Bash);
  CHECK(umbriel::completionShellNamed("zsh") == umbriel::CompletionShell::Zsh);
  CHECK(umbriel::completionShellNamed("fish") == umbriel::CompletionShell::Fish);
  CHECK(!umbriel::completionShellNamed("nushell"));
}

int main() { return RUN_TESTS(); }
