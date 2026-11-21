#include "cli/complete.h"

#include "cli/cli_words.h"
#include "config/config.h"
#include "config/effects.h"
#include "config/keybind_parse.h"
#include "config/window_position.h"
#include "server/ipc.h"
#include "server/ipc_commands.h"
#include "view/window_region.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {
    struct ArgState {
      std::vector<std::string_view> positional;
      // Every complete argument word, including the flag words and values that `positional` drops.
      size_t words = 0;
      std::string_view pendingFlag;
      unsigned seenFlags = 0;
      // A word the grammar refuses leaves no completable position, because the parser rejects the whole argument list.
      bool rejected = false;
    };

    // `words` are the complete argument words, so a flag at the end is the one whose value is being typed. Any word
    // starting with "--" is a flag, as it is for `splitActionWords`; one the action does not accept rejects the list.
    ArgState scanArgs(std::span<const std::string> words, unsigned allowed) {
      ArgState state;
      state.words = words.size();
      for (size_t index = 0; index < words.size(); ++index) {
        const std::string_view word{words[index]};
        if (!word.starts_with("--")) {
          state.positional.push_back(word);
          continue;
        }
        const unsigned mask = actionFlagMask(word);
        if (mask == 0 || (allowed & mask) == 0) {
          state.rejected = true;
          return state;
        }
        // A flag twice is a line the parser refuses, so the position after it has no vocabulary.
        if ((state.seenFlags & mask) != 0) {
          state.rejected = true;
          return state;
        }
        if (index + 1 >= words.size()) {
          state.pendingFlag = word;
          return state;
        }
        state.seenFlags |= mask;
        ++index;
      }
      return state;
    }

    enum class Form : uint8_t {
      FirstWord,
      MsgAction,
      MsgArgument,
      SubscribeEvent,
      ConfigWord,
      ConfigPath,
      CommandLine,
      StartupFlag,
      CompletionsShell,
      IpcCommand,
      Nothing,
    };

    struct Analysis {
      Form form = Form::FirstWord;
      std::string_view partial;
      const ActionSpec* action = nullptr;
      const IpcCommandSpec* command = nullptr;
      // Words the command's own spelling consumes before its arguments: 0 for a one-word command, 1 for `config set`.
      size_t argBase = 0;
      // `config`'s subcommand, which its flags may follow in any order.
      std::string_view configSub;
      // The startup flags whose value is already on the line. A value may spell a flag, so the walk records them
      // rather than testing the words for membership.
      unsigned startupFlags = 0;
      ArgState args;
    };

    bool isFlagWord(std::string_view word) { return word.starts_with('-'); }

    constexpr unsigned kStartupConfigFlag = 1U << 0;
    constexpr unsigned kStartupSpawnFlag = 1U << 1;
    bool isJsonFlagWord(std::string_view word) { return word == kCliJsonFlag || word == kCliJsonShortFlag; }

    // `-j` and `--json` are one option under two spellings, so both are offered from here.
    void emitJsonFlags(std::string_view partial, const CompletionSink& sink) {
      constexpr std::string_view kDescription = "print the reply as JSON";
      if (kCliJsonFlag.starts_with(partial)) {
        sink(kCliJsonFlag, kDescription);
      }
      if (kCliJsonShortFlag.starts_with(partial)) {
        sink(kCliJsonShortFlag, kDescription);
      }
    }

    Analysis analyze(std::span<const std::string> words) {
      Analysis analysis;
      if (words.empty()) {
        return analysis;
      }
      analysis.partial = words.back();
      if (words.size() == 1) {
        return analysis;
      }

      const std::string_view first{words.front()};
      const size_t position = words.size() - 1;
      const auto completeArgs = [&words, position](size_t start) { return words.subspan(start, position - start); };

      if (first == kCliMsg) {
        if (position == 1) {
          analysis.form = Form::MsgAction;
          return analysis;
        }
        // `msg`'s own JSON flags come before the action; everything after it belongs to the action.
        size_t actionIndex = 1;
        while (actionIndex < position && isJsonFlagWord(words[actionIndex])) {
          ++actionIndex;
        }
        if (actionIndex >= position) {
          analysis.form = Form::MsgAction;
          return analysis;
        }
        if (const ActionSpec* spec = findActionSpec(words[actionIndex]); spec != nullptr) {
          analysis.form = Form::MsgArgument;
          analysis.action = spec;
          analysis.args = scanArgs(completeArgs(actionIndex + 1), actionAllowedFlags(spec->argKind));
        } else {
          analysis.form = Form::Nothing;
        }
        return analysis;
      }
      if (first == kCliSubscribe) {
        analysis.form = Form::SubscribeEvent;
        return analysis;
      }
      if (first == kCliConfig) {
        if (position == 1) {
          analysis.form = Form::ConfigWord;
          return analysis;
        }
        analysis.configSub = words[1];
        const std::string_view sub{words[1]};
        if (sub == kCliConfigValidate) {
          // `-c` takes the path that follows it; every other word leaves the flags and their order to complete.
          analysis.form = words[position - 1] == kCliConfigFlag ? Form::ConfigPath : Form::ConfigWord;
          return analysis;
        }
        if (sub == kCliConfigSchema) {
          analysis.form = Form::ConfigWord;
          return analysis;
        }
        // The Config group's requests are reached as `config <sub>`; the request name is `config-<sub>`.
        if (const IpcCommandSpec* spec = findConfigCommand(sub); spec != nullptr) {
          analysis.form = Form::IpcCommand;
          analysis.command = spec;
          analysis.argBase = 1;
          return analysis;
        }
        analysis.form = Form::Nothing;
        return analysis;
      }
      if (first == kCliCompletions) {
        analysis.form = position == 1 ? Form::CompletionsShell : Form::Nothing;
        return analysis;
      }
      if (const IpcCommandSpec* spec = findIpcCommand(first); spec != nullptr) {
        // The Config group is reached as `config <sub>`; its request name alone is a line the dispatch refuses.
        if (spec->group == IpcCommandGroup::Config) {
          analysis.form = Form::Nothing;
          return analysis;
        }
        analysis.form = Form::IpcCommand;
        analysis.command = spec;
        const IpcArgCompletion& completion = spec->completion;
        if (completion.kind() == IpcArgCompletion::Kind::Path) {
          analysis.form = Form::ConfigPath;
        }
        return analysis;
      }
      // The compositor's startup flags each take a value and come in either order; their values are the only
      // positions the line has. The walk consumes them in pairs, as the dispatch does, so a value that happens to
      // spell `-c` or `-s` stays a value.
      if (first == kCliConfigFlag || first == kCliSpawnFlag) {
        size_t index = 0;
        while (index < position) {
          const std::string_view word{words[index]};
          const bool isConfig = word == kCliConfigFlag;
          if (!isConfig && word != kCliSpawnFlag) {
            // A word the dispatch refuses leaves no position; an empty one is the value being typed.
            analysis.form = word.empty() ? Form::StartupFlag : Form::Nothing;
            return analysis;
          }
          if (index + 1 == position) {
            // The flag just typed has no value yet.
            analysis.form = isConfig ? Form::ConfigPath : Form::CommandLine;
            return analysis;
          }
          analysis.startupFlags |= isConfig ? kStartupConfigFlag : kStartupSpawnFlag;
          index += 2;
        }
        analysis.form = Form::StartupFlag;
        return analysis;
      }
      analysis.form = Form::Nothing;
      return analysis;
    }

    // Candidates are filtered by the partial word and never repeated, so a value two tables name once is offered once.
    auto uniqueSink(std::string_view partial, std::vector<std::string_view>& emitted, const CompletionSink& sink) {
      return [partial, &emitted, &sink](std::string_view value, std::string_view description) {
        if (value.empty() || !value.starts_with(partial) || std::ranges::contains(emitted, value)) {
          return;
        }
        emitted.push_back(value);
        sink(value, description);
      };
    }

    // Where a workspace kind reads its positional selector, or nullopt for a kind that takes none.
    constexpr std::optional<size_t> workspacePositionalIndex(ActionArgKind kind) {
      switch (kind) {
      case ActionArgKind::Workspace:
        return 0;
      case ActionArgKind::WorkspaceMoveToOutput:
        return 1;
      default:
        return std::nullopt;
      }
    }

    CompletionOutcome completeActionArguments(
        const ActionSpec& spec, const ArgState& args, std::string_view partial, const CompletionValues& values,
        const CompletionSink& sink
    ) {
      std::vector<std::string_view> emitted;
      auto emit = uniqueSink(partial, emitted, sink);
      auto emitAll = [&](const std::vector<std::string>& list, std::string_view description) {
        for (const std::string& value : list) {
          emit(value, description);
        }
      };
      // The word grammar reads a leading `--` as a flag, so such a value cannot stand where it expects a positional.
      // A flag's own value is taken verbatim, so `--name --x` stays completable.
      auto emitPositional = [&](const std::vector<std::string>& list, std::string_view description) {
        for (const std::string& value : list) {
          if (!value.starts_with("--")) {
            emit(value, description);
          }
        }
      };
      // A positional workspace word is an index when it is all digits, so a digit-only name is reachable through
      // `--name` alone.
      auto emitWorkspace = [&](const std::vector<std::string>& list) {
        for (const std::string& value : list) {
          if (!value.starts_with("--") && !allDigits(value)) {
            emit(value, "workspace");
          }
        }
      };

      // `parseWorkspaceSelector` takes exactly one of a positional workspace or `--name`, so each suppresses the
      // other's candidates.
      const std::optional<size_t> workspaceIndex = workspacePositionalIndex(spec.argKind);
      const bool workspaceByName = (args.seenFlags & kActionFlagName) != 0;
      const bool workspaceByPosition = workspaceIndex.has_value() && args.positional.size() > *workspaceIndex;
      // One word past what the kind reads leaves the grammar with nothing, so a surplus positional rejects the line
      // and no flag is offered behind it. `spawn` reads an unbounded tail and is exempt.
      const std::optional<size_t> arity = actionPositionalArity(spec.argKind);
      const bool tooManyPositionals = arity.has_value() && args.positional.size() > *arity;
      // Two workspace selectors are a line the parser refuses, so nothing after them completes.
      const bool bothSelectors = workspaceByPosition && workspaceByName;

      // `spawn` hands its whole tail to the shell, so a word of it that starts with "--" is the shell's, not a flag.
      if (spec.argKind != ActionArgKind::Command) {
        if (args.rejected || tooManyPositionals || bothSelectors) {
          return CompletionOutcome::Candidates;
        }
        // `scanArgs` only records a flag the action accepts, so a pending flag always names the family to read.
        if (!args.pendingFlag.empty()) {
          const unsigned pending = actionFlagMask(args.pendingFlag);
          if (pending == kActionFlagOutput) {
            emitAll(values.outputs.items(), "output");
          } else if (pending == kActionFlagWindow) {
            emitAll(values.windows.items(), "window");
          } else if (!workspaceByPosition) {
            emitAll(values.workspaces.items(), "workspace");
          }
          return CompletionOutcome::Candidates;
        }
        if (isFlagWord(partial)) {
          const unsigned flags = actionAllowedFlags(spec.argKind);
          // `parseActionPayload` only reaches a workspace selector through `--output` when one is already there, so the
          // transfer target completes after the destination and the selector rather than before them.
          const bool outputReachable =
              spec.argKind != ActionArgKind::WorkspaceMoveToOutput || workspaceByPosition || workspaceByName;
          if ((flags & kActionFlagOutput) != 0 && (args.seenFlags & kActionFlagOutput) == 0 && outputReachable) {
            emit(kActionFlagOutputWord, "target output");
          }
          if ((flags & kActionFlagWindow) != 0 && (args.seenFlags & kActionFlagWindow) == 0) {
            emit(kActionFlagWindowWord, "target window");
          }
          if ((flags & kActionFlagName) != 0 && (args.seenFlags & kActionFlagName) == 0 && !workspaceByPosition) {
            emit(kActionFlagNameWord, "workspace name");
          }
          return CompletionOutcome::Candidates;
        }
      }

      const size_t index = args.positional.size();
      switch (spec.argKind) {
      case ActionArgKind::None:
      case ActionArgKind::Fraction:
      case ActionArgKind::FractionDelta:
      case ActionArgKind::SdrWhite:
      case ActionArgKind::TabIndex:
        return CompletionOutcome::Candidates;
      case ActionArgKind::Command:
        // `spawn`'s tail is a shell command line: the first word is a command name, the rest are its arguments.
        // The count is every tail word, because a command name may begin with `--` as `positional` would not.
        return args.words == 0 ? CompletionOutcome::DelegateCommand : CompletionOutcome::Delegate;
      case ActionArgKind::Submap:
        if (index == 0) {
          // A submap name must pass the parser's own validator, and one beginning with `--` is a flag word instead.
          for (const std::string& name : values.submaps.items()) {
            if (!name.starts_with("--") && validSubmapName(name)) {
              emit(name, "submap");
            }
          }
          emit(kSubmapResetWord, "leave one submap level");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::Output:
      case ActionArgKind::OptionalOutput:
        if (index == 0) {
          emitPositional(values.outputs.items(), "output");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::OptionalScratchpad:
        if (index == 0) {
          if (values.scratchpads.items().empty()) {
            emit(kImplicitScratchpad, "implicit scratchpad");
          } else {
            emitPositional(values.scratchpads.items(), "scratchpad");
          }
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowId:
      case ActionArgKind::OptionalWindowId:
        if (index == 0) {
          emitPositional(values.windows.items(), "window");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowRegion:
        if (index == 0) {
          for (const std::string_view name : windowRegionNames()) {
            emit(name, "region");
          }
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::ColumnDisplay:
        if (index == 0) {
          for (const std::string_view name : columnDisplayNames()) {
            emit(name, "column display");
          }
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::SkipConfirmation:
        if (index == 0) {
          emit(kSkipConfirmationWord, "quit without the on-screen confirmation");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::LayoutMode:
        if (index == 0) {
          for (const std::string_view name : layoutModeNames()) {
            emit(name, "layout mode");
          }
          emit(kLayoutModeToggleWord, "cycle the listed modes");
          return CompletionOutcome::Candidates;
        }
        if (args.positional.front() != kLayoutModeToggleWord) {
          return CompletionOutcome::Candidates;
        }
        for (const std::string_view name : layoutModeNames()) {
          if (std::ranges::contains(std::span(args.positional).subspan(1), name)) {
            continue;
          }
          emit(name, "layout mode");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowMoveTo:
        if (index == 0) {
          for (const WindowPositionAnchorName& entry : kWindowPositionAnchorNames) {
            emit(entry.name, "anchor");
          }
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowMoveBy:
        return CompletionOutcome::Candidates;
      case ActionArgKind::Workspace:
        if (index == *workspaceIndex && !workspaceByName) {
          emitWorkspace(values.workspaces.items());
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WorkspaceMoveToOutput:
        if (index == 0) {
          emitPositional(values.outputs.items(), "output");
          emit(kCurrentOutputWord, "the output under the pointer");
        } else if (index == *workspaceIndex && !workspaceByName) {
          emitWorkspace(values.workspaces.items());
        }
        return CompletionOutcome::Candidates;
      // Both slots read names of their own kind only, and a cycle needs a pool: `effectReferenceError` refuses
      // everything else, so the lists the values carry are already filtered that way.
      case ActionArgKind::WindowEffectSet:
      case ActionArgKind::ScreenEffectSet:
      case ActionArgKind::CursorEffectSet: {
        if (index == 0) {
          const EffectKind kind = effectSlotKind(spec.action).value_or(EffectKind::Window);
          emitPositional(values.effects.presets(kind), "preset");
          emitPositional(values.effects.pools(kind), "pool");
          emit(kEffectOff, "suppress the slot");
        }
        return CompletionOutcome::Candidates;
      }
      case ActionArgKind::WindowEffectCycle:
      case ActionArgKind::ScreenEffectCycle:
      case ActionArgKind::CursorEffectCycle: {
        if (index == 0) {
          const EffectKind kind = effectSlotKind(spec.action).value_or(EffectKind::Window);
          emitPositional(values.effects.pools(kind), "pool");
        }
        return CompletionOutcome::Candidates;
      }
      }
      return CompletionOutcome::Candidates;
    }

    void emitFirstWords(std::string_view partial, const CompletionSink& sink) {
      std::vector<std::string_view> emitted;
      auto emit = uniqueSink(partial, emitted, sink);

      emit(kCliMsg, "send an action to the compositor");
      emit(kCliSubscribe, "stream events as JSON lines");
      emit(kCliConfig, "validate or describe the configuration");
      emit(kCliCompletions, "print a shell completion script");
      emit(kCliComplete, "answer a completion request (used by the scripts)");
      emit(kCliHelp, "show help");
      emit(kCliSpawnFlag, "spawn a command once the compositor starts");
      emit(kCliConfigFlag, "use <config> instead of the default configuration");
      emit(kCliHelpShortFlag, "show help");
      emit(kCliHelpFlag, "show help");
      emit(kCliVersionShortFlag, "print the version");
      emit(kCliVersionUpperFlag, "print the version");
      emit(kCliVersionFlag, "print the version");
      for (const IpcCommandSpec& spec : ipcCommands()) {
        // The Config group is spelled `config <sub>` on the command line, so `config-replace`'s neighbours must not be
        // offered as words no dispatch reaches.
        if (spec.group == IpcCommandGroup::Config) {
          continue;
        }
        emit(spec.name, spec.description);
      }
    }

    void emitActions(std::string_view partial, const CompletionSink& sink) {
      if (isFlagWord(partial)) {
        emitJsonFlags(partial, sink);
        return;
      }
      for (const ActionSpec& spec : actionSpecs()) {
        if (spec.name.starts_with(partial)) {
          sink(spec.name, spec.summary);
        }
      }
    }

    void emitEvents(std::string_view word, const CompletionSink& sink) {
      if (isFlagWord(word)) {
        emitJsonFlags(word, sink);
        return;
      }
      const size_t comma = word.rfind(',');
      const std::string_view prefix = comma == std::string_view::npos ? std::string_view{} : word.substr(0, comma + 1);
      const std::string_view partial = comma == std::string_view::npos ? word : word.substr(comma + 1);
      for (const std::string_view name : Ipc::kEventNames) {
        if (name.starts_with(partial)) {
          sink(std::string(prefix) + std::string(name), "event");
        }
      }
    }

    void emitConfigWords(std::string_view sub, std::string_view partial, const CompletionSink& sink) {
      if (sub.empty()) {
        if (kCliConfigValidate.starts_with(partial)) {
          sink(kCliConfigValidate, "check a configuration");
        }
        if (kCliConfigSchema.starts_with(partial)) {
          sink(kCliConfigSchema, "list every configuration key");
        }
        // The Config group's commands, offered under the subcommand words the dispatch accepts.
        for (const IpcCommandSpec& spec : ipcCommands()) {
          if (spec.group != IpcCommandGroup::Config || !spec.name.starts_with(kConfigCommandPrefix)) {
            continue;
          }
          const std::string_view word = spec.name.substr(kConfigCommandPrefix.size());
          if (word.starts_with(partial)) {
            sink(word, spec.description);
          }
        }
        return;
      }
      if (sub == kCliConfigValidate) {
        if (kCliConfigFlag.starts_with(partial)) {
          sink(kCliConfigFlag, "path to the configuration file");
        }
        emitJsonFlags(partial, sink);
      } else if (sub == kCliConfigSchema) {
        emitJsonFlags(partial, sink);
      }
    }

    // A startup line takes one `-c` and one `-s`, each with its value, so what completes is whatever the line still
    // lacks.
    void emitStartupFlags(unsigned seen, std::string_view partial, const CompletionSink& sink) {
      if ((seen & kStartupSpawnFlag) == 0 && kCliSpawnFlag.starts_with(partial)) {
        sink(kCliSpawnFlag, "command to run once the session is up");
      }
      if ((seen & kStartupConfigFlag) == 0 && kCliConfigFlag.starts_with(partial)) {
        sink(kCliConfigFlag, "path to the configuration file");
      }
    }

    // `arguments` is how many argument words already reach the command, after its own spelling and `--json`/`-j` are
    // taken out; the word being completed is not counted. A command that takes none refuses the line as soon as one
    // arrives, and one that takes an argument offers nothing past its first.
    void emitCommandWords(
        const IpcCommandSpec& spec, size_t arguments, std::string_view partial, const CompletionValues& values,
        const CompletionSink& sink
    ) {
      if (arguments > 0) {
        return;
      }
      const IpcArgCompletion& completion = spec.completion;
      // The JSON flags are options of every command that takes arguments, so they are offered beside them; a value
      // the command reads is offered whatever the word begins with, because the dispatch takes it verbatim.
      if (isFlagWord(partial)) {
        emitJsonFlags(partial, sink);
      }
      if (completion.kind() == IpcArgCompletion::Kind::Output) {
        for (const std::string& name : values.outputs.items()) {
          if (!name.starts_with("--") && name.starts_with(partial)) {
            sink(name, "output");
          }
        }
        return;
      }
      if (completion.kind() != IpcArgCompletion::Kind::Choice) {
        return;
      }
      // A dash-prefixed choice, such as `config unset --all`, is an argument the command takes rather than one of its
      // options, so it is offered on an empty word and not only once a dash is typed.
      for (const std::string_view choice : completion.choices()) {
        if (choice.starts_with(partial)) {
          sink(choice, completion.description());
        }
      }
    }
  } // namespace

  CompletionOutcome
  completeWords(std::span<const std::string> words, const CompletionValues& values, const CompletionSink& sink) {
    const Analysis analysis = analyze(words);
    switch (analysis.form) {
    case Form::FirstWord:
      emitFirstWords(analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::MsgAction:
      emitActions(analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::MsgArgument:
      return completeActionArguments(*analysis.action, analysis.args, analysis.partial, values, sink);
    case Form::SubscribeEvent:
      emitEvents(analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::ConfigWord:
      emitConfigWords(analysis.configSub, analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::ConfigPath:
      return CompletionOutcome::Delegate;
    case Form::CommandLine:
      return CompletionOutcome::DelegateCommand;
    case Form::StartupFlag:
      emitStartupFlags(analysis.startupFlags, analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::CompletionsShell:
      for (const CompletionShellName& shell : completionShellNames()) {
        if (shell.name.starts_with(analysis.partial)) {
          sink(shell.name, "completion script");
        }
      }
      return CompletionOutcome::Candidates;
    case Form::IpcCommand: {
      // `runSpecCommand` strips the JSON flags before reading the arguments, so the position counts the words that
      // actually reach it and not the command's own spelling.
      const std::span<const std::string> consumed = words.first(words.size() - 1);
      const auto reaching =
          std::ranges::count_if(consumed, [](const std::string& word) { return !isJsonFlagWord(word); });
      // The command's own spelling is one word, plus `config`'s extra one; everything else is an argument.
      const size_t arguments = static_cast<size_t>(reaching) - 1 - analysis.argBase;
      emitCommandWords(*analysis.command, arguments, analysis.partial, values, sink);
      return CompletionOutcome::Candidates;
    }
    case Form::Nothing:
      return CompletionOutcome::Candidates;
    }
    return CompletionOutcome::Candidates;
  }

  CompletionList::CompletionList(std::initializer_list<std::string> items) : m_items(items), m_filled(true) {}

  EffectCandidates::EffectCandidates(Filler filler) : m_filler(std::move(filler)) {}

  void EffectCandidates::fill(EffectKind kind) const {
    const auto slot = static_cast<size_t>(kind);
    if (m_filled[slot]) {
      return;
    }
    m_filled[slot] = true;
    if (m_filler) {
      m_filler(m_byKind[slot].presets, m_byKind[slot].pools, kind);
    }
    std::ranges::sort(m_byKind[slot].presets);
    std::ranges::sort(m_byKind[slot].pools);
  }

  const std::vector<std::string>& EffectCandidates::presets(EffectKind kind) const {
    fill(kind);
    return m_byKind[static_cast<size_t>(kind)].presets;
  }

  const std::vector<std::string>& EffectCandidates::pools(EffectKind kind) const {
    fill(kind);
    return m_byKind[static_cast<size_t>(kind)].pools;
  }

  CompletionList::CompletionList(Filler filler) : m_filler(std::move(filler)) {}

  const std::vector<std::string>& CompletionList::items() const {
    if (!m_filled) {
      m_filled = true;
      if (m_filler) {
        m_filler(m_items);
      }
      std::ranges::sort(m_items);
    }
    return m_items;
  }

  std::span<const CompletionShellName> completionShellNames() {
    static constexpr std::array<CompletionShellName, 3> kNames{
        CompletionShellName{CompletionShell::Bash, "bash"},
        CompletionShellName{CompletionShell::Zsh, "zsh"},
        CompletionShellName{CompletionShell::Fish, "fish"},
    };
    return kNames;
  }

  std::optional<CompletionShell> completionShellNamed(std::string_view name) {
    for (const CompletionShellName& shell : completionShellNames()) {
      if (shell.name == name) {
        return shell.shell;
      }
    }
    return std::nullopt;
  }

} // namespace umbriel
