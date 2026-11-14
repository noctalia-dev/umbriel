#include "cli/complete.h"

#include "cli/cli_words.h"
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
      std::string_view pendingFlag;
      unsigned seenFlags = 0;
      // A word the grammar refuses leaves no completable position, because the parser rejects the whole argument list.
      bool rejected = false;
    };

    // `words` are the complete argument words, so a flag at the end is the one whose value is being typed. Any word
    // starting with "--" is a flag, as it is for `splitActionWords`; one the action does not accept rejects the list.
    ArgState scanArgs(std::span<const std::string> words, unsigned allowed) {
      ArgState state;
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
      ArgState args;
    };

    bool isFlagWord(std::string_view word) { return word.starts_with('-'); }
    bool isJsonFlagWord(std::string_view word) { return word == kCliJsonFlag || word == kCliJsonShortFlag; }

    // `-j` and `--json` are one option under two spellings, so both are offered from here.
    void emitJsonFlags(std::string_view partial, std::string_view description, const CompletionSink& sink) {
      if (kCliJsonFlag.starts_with(partial)) {
        sink(kCliJsonFlag, description);
      }
      if (kCliJsonShortFlag.starts_with(partial)) {
        sink(kCliJsonShortFlag, description);
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
        const std::string_view sub{words[1]};
        if (sub == kCliConfigValidate) {
          analysis.form = position >= 2 && words[position - 1] == kCliConfigFlag ? Form::ConfigPath : Form::ConfigWord;
          return analysis;
        }
        if (sub == kCliConfigSchema) {
          analysis.form = Form::ConfigWord;
          return analysis;
        }
        // The Config group's requests are reached as `config <sub>`; the request name is `config-<sub>`.
        const std::string name = std::format("{}{}", kConfigCommandPrefix, sub);
        const IpcCommandSpec* spec = findIpcCommand(name);
        if (spec != nullptr && spec->group == IpcCommandGroup::Config) {
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
        analysis.form = Form::IpcCommand;
        analysis.command = spec;
        const IpcArgCompletion completion = ipcCommandArgCompletion(spec->name);
        if (completion.kind() == IpcArgCompletion::Kind::Path) {
          analysis.form = Form::ConfigPath;
        }
      }
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

      const unsigned flags = actionAllowedFlags(spec.argKind);
      if (args.rejected) {
        return CompletionOutcome::Candidates;
      }
      // `scanArgs` only records a flag the action accepts, so a pending flag always names the family to read.
      if (!args.pendingFlag.empty()) {
        const unsigned pending = actionFlagMask(args.pendingFlag);
        if (pending == kActionFlagOutput) {
          emitAll(values.outputs.items(), "output");
        } else if (pending == kActionFlagWindow) {
          emitAll(values.windows.items(), "window");
        } else {
          emitAll(values.workspaces.items(), "workspace");
        }
        return CompletionOutcome::Candidates;
      }
      if (isFlagWord(partial)) {
        if ((flags & kActionFlagOutput) != 0 && (args.seenFlags & kActionFlagOutput) == 0) {
          emit(kActionFlagOutputWord, "target output");
        }
        if ((flags & kActionFlagWindow) != 0 && (args.seenFlags & kActionFlagWindow) == 0) {
          emit(kActionFlagWindowWord, "target window");
        }
        if ((flags & kActionFlagName) != 0 && (args.seenFlags & kActionFlagName) == 0) {
          emit(kActionFlagNameWord, "workspace name");
        }
        return CompletionOutcome::Candidates;
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
        return CompletionOutcome::Delegate;
      case ActionArgKind::Submap:
        if (index == 0) {
          emitAll(values.submaps.items(), "submap");
          emit(kSubmapResetWord, "leave one submap level");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::Output:
      case ActionArgKind::OptionalOutput:
        if (index == 0) {
          emitAll(values.outputs.items(), "output");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::OptionalScratchpad:
        if (index == 0) {
          if (values.scratchpads.items().empty()) {
            emit(kImplicitScratchpad, "implicit scratchpad");
          } else {
            emitAll(values.scratchpads.items(), "scratchpad");
          }
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowId:
      case ActionArgKind::OptionalWindowId:
        if (index == 0) {
          emitAll(values.windows.items(), "window");
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
        if (index == 0) {
          emitAll(values.workspaces.items(), "workspace");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WorkspaceMoveToOutput:
        if (index == 0) {
          emitAll(values.outputs.items(), "output");
        } else if (index == 1) {
          emitAll(values.workspaces.items(), "workspace");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowEffectSet:
      case ActionArgKind::ScreenEffectSet:
      case ActionArgKind::CursorEffectSet:
        if (index == 0) {
          emitAll(values.effectPresets.items(), "preset");
          emitAll(values.effectPools.items(), "pool");
          emit(kEffectOff, "suppress the slot");
        }
        return CompletionOutcome::Candidates;
      case ActionArgKind::WindowEffectCycle:
      case ActionArgKind::ScreenEffectCycle:
      case ActionArgKind::CursorEffectCycle:
        if (index == 0) {
          emitAll(values.effectPresets.items(), "preset");
          emitAll(values.effectPools.items(), "pool");
        }
        return CompletionOutcome::Candidates;
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
        emitJsonFlags(partial, "print the reply as JSON", sink);
        return;
      }
      for (const ActionSpec& spec : actionSpecs()) {
        if (spec.name.starts_with(partial)) {
          sink(spec.name, spec.summary);
        }
      }
    }

    void emitEvents(std::string_view word, const CompletionSink& sink) {
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
        if (partial.empty() || kCliConfigValidate.starts_with(partial)) {
          sink(kCliConfigValidate, "check a configuration");
        }
        if (partial.empty() || kCliConfigSchema.starts_with(partial)) {
          sink(kCliConfigSchema, "list every configuration key");
        }
        // The Config group's commands, offered under the subcommand words the dispatch accepts.
        for (const IpcCommandSpec& spec : ipcCommands()) {
          if (spec.group != IpcCommandGroup::Config || !spec.name.starts_with(kConfigCommandPrefix)) {
            continue;
          }
          const std::string_view word = spec.name.substr(kConfigCommandPrefix.size());
          if (partial.empty() || word.starts_with(partial)) {
            sink(word, spec.description);
          }
        }
        return;
      }
      if (sub == kCliConfigValidate) {
        if (partial.empty() || kCliConfigFlag.starts_with(partial)) {
          sink(kCliConfigFlag, "path to the configuration file");
        }
        if (partial.empty() || kCliJsonFlag.starts_with(partial)) {
          sink(kCliJsonFlag, "print diagnostics as JSON");
        }
      } else if (sub == kCliConfigSchema && (partial.empty() || kCliJsonFlag.starts_with(partial))) {
        sink(kCliJsonFlag, "describe every key");
      }
    }

    void emitCommandWords(
        const IpcCommandSpec& spec, size_t position, std::string_view partial, const CompletionSink& sink
    ) {
      if (isFlagWord(partial)) {
        emitJsonFlags(partial, "print the reply as JSON", sink);
        return;
      }
      const IpcArgCompletion completion = ipcCommandArgCompletion(spec.name);
      if (position == 1 && completion.kind() == IpcArgCompletion::Kind::Choice) {
        for (const std::string_view choice : completion.choices()) {
          if (choice.starts_with(partial)) {
            sink(choice, "every remembered grant");
          }
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
      emitConfigWords(
          words.size() > 2 ? std::string_view{words[words.size() - 2]} : std::string_view{}, analysis.partial, sink
      );
      return CompletionOutcome::Candidates;
    case Form::ConfigPath:
      return CompletionOutcome::Delegate;
    case Form::CompletionsShell:
      for (const CompletionShellName& shell : completionShellNames()) {
        if (shell.name.starts_with(analysis.partial)) {
          sink(shell.name, "completion script");
        }
      }
      return CompletionOutcome::Candidates;
    case Form::IpcCommand:
      emitCommandWords(*analysis.command, words.size() - 1 - analysis.argBase, analysis.partial, sink);
      return CompletionOutcome::Candidates;
    case Form::Nothing:
      return CompletionOutcome::Candidates;
    }
    return CompletionOutcome::Candidates;
  }

  CompletionList::CompletionList(std::initializer_list<std::string> items) : m_items(items), m_filled(true) {}

  CompletionList::CompletionList(std::vector<std::string> items) : m_items(std::move(items)), m_filled(true) {}

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
