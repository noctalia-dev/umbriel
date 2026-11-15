#include "cli/cli_words.h"
#include "cli/complete.h"
#include "cli/ipc_client.h"
#include "config/config.h"
#include "config/config_diag.h"
#include "config/schema.h"
#include "core/build_info.h"
#include "core/fdlimit.h"
#include "core/log.h"
#include "core/process.h"
#include "scene/cheatsheet_rows.h"
#include "server/ipc.h"
#include "server/ipc_commands.h"
#include "server/server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#ifdef __GLIBC__
#ifdef UMBRIEL_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#else
#include <malloc.h>
#endif
#endif

namespace {
  constexpr Logger kLog("main");

  bool isJsonFlag(const char* arg) { return arg == umbriel::kCliJsonFlag || arg == umbriel::kCliJsonShortFlag; }

  bool isHelpFlag(const char* arg) { return arg == umbriel::kCliHelpFlag || arg == umbriel::kCliHelpShortFlag; }

  int validateConfig(int argc, char** argv) {
    const char* configPath = nullptr;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
      if (argv[i] == umbriel::kCliConfigFlag && i + 1 < argc) {
        configPath = argv[++i];
      } else if (isJsonFlag(argv[i])) {
        json = true;
      } else {
        std::println(stderr, "error: unknown option '{}' for config validate", argv[i]);
        return EXIT_FAILURE;
      }
    }

    umbriel::loadConfig(configPath);
    const auto& diags = umbriel::configDiagnostics();
    if (json) {
      nlohmann::json diagnostics = nlohmann::json::array();
      for (const auto& d : diags) {
        const bool located = !d.file.empty();
        diagnostics.push_back({
            {"severity", d.severity == umbriel::ConfigDiagnostic::Severity::Error ? "error" : "warning"},
            {"message", d.message},
            {"file", located ? nlohmann::json(d.file) : nlohmann::json(nullptr)},
            {"line", located && d.line > 0 ? nlohmann::json(d.line) : nlohmann::json(nullptr)},
            {"column", located && d.column > 0 ? nlohmann::json(d.column) : nlohmann::json(nullptr)},
        });
      }
      std::println(
          "{}",
          nlohmann::json{
              {"valid", diags.empty()},
              {"path", umbriel::configRootPath().string()},
              {"diagnostics", std::move(diagnostics)},
          }
              .dump()
      );
      return diags.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (diags.empty()) {
      std::println("config: ok ({})", umbriel::configRootPath().string());
      return EXIT_SUCCESS;
    }
    for (const auto& d : diags) {
      const char* severity = d.severity == umbriel::ConfigDiagnostic::Severity::Error ? "error" : "warning";
      std::println(stderr, "{}: {}", severity, d.display());
    }
    std::println(stderr, "configuration invalid");
    return EXIT_FAILURE;
  }

  int printConfigSchema(bool json) {
    const umbriel::registry::Descriptions keys = umbriel::registry::describeConfig(umbriel::Config{});
    if (!json) {
      std::print("{}", umbriel::configSchemaSummary(keys));
      return EXIT_SUCCESS;
    }
    // Same rule as --version: a build outside git reports "unknown", which is no revision at all.
    const std::string_view revision = umbriel::build_info::revision();
    const bool knownRevision = !revision.empty() && revision != "unknown";
    std::print(
        "{}",
        umbriel::configSchemaJson(
            keys, umbriel::build_info::version(), knownRevision ? std::optional(revision) : std::nullopt
        )
    );
    return EXIT_SUCCESS;
  }

  void printHelp(FILE* stream) {
    auto heading = [stream](std::string_view title) { std::println(stream, "\n{}:", title); };
    auto row = [stream](std::string_view cmd, std::string_view desc) {
      std::println(stream, "  {:<30} {}", cmd, desc);
    };
    auto specRows = [&row](umbriel::IpcCommandGroup group) {
      for (const auto& spec : umbriel::ipcCommands()) {
        if (spec.group != group) {
          continue;
        }
        std::string cmd = umbriel::cliName(spec);
        if (!spec.argSpec.empty()) {
          cmd += ' ';
          cmd += spec.argSpec;
        }
        row(cmd, spec.description);
      }
    };

    std::println(stream, "umbriel {}: a wayland compositor\n", umbriel::build_info::version());
    std::println(stream, "Usage: umbriel [-s <command>] [-c <config>]   run the compositor");
    std::println(stream, "       umbriel <command> [args...]            run a command");

    heading("Compositor options");
    row("-s <command>", "spawn <command> once the compositor starts");
    row("-c <config>", "use <config> instead of the default config path");

    heading("Control the running compositor");
    row("msg <action> [args...]", "send an action to the compositor");
    specRows(umbriel::IpcCommandGroup::Control);
    specRows(umbriel::IpcCommandGroup::Config);

    heading("Inspect the running compositor");
    specRows(umbriel::IpcCommandGroup::Inspect);
    {
      std::string names;
      for (const auto& name : umbriel::Ipc::kEventNames) {
        if (!names.empty()) {
          names += ", ";
        }
        names += name;
      }
      row("subscribe <event>[,<event>…]", "stream events as JSON lines");
      std::println(stream, "{:>33}{}", "", "events: " + names);
    }

    heading("Configuration, without a running compositor");
    row("config validate [-c <config>]", "check the config file; --json lists diagnostics");
    row("config schema [--json]", "count or describe every config key");

    heading("Shell completion");
    row("completions <bash|zsh|fish>", "print a completion script to install or eval");
    row("complete -- <word>...", "answer a completion request (used by the scripts)");

    if (std::ranges::any_of(umbriel::ipcCommands(), [](const auto& spec) {
          return spec.group == umbriel::IpcCommandGroup::Harness;
        })) {
      heading("Test harness");
      specRows(umbriel::IpcCommandGroup::Harness);
    }

    heading("Other");
    row("help | -h | --help", "show this help");
    row("-v | -V | --version", "print version");

    std::println(
        stream,
        "\nRun `umbriel msg --help` to list all available actions for `msg` and keybinds, and `umbriel completions "
        "<shell>` to complete them."
    );
  }

  // `umbriel msg --help`: every action with its arguments, grouped as the cheatsheet groups them.
  void printActionHelp() {
    std::println("Usage: umbriel msg <action> [args...]");
    std::println("");
    std::println("Send an action to the running compositor.");
    std::println(
        "Arguments follow the action as separate words; `spawn` runs the remaining words as one shell command."
    );
    std::println("Available actions, grouped as the cheatsheet groups them:");
    for (const umbriel::Group group : umbriel::fixedGroupOrder()) {
      std::vector<std::string> names;
      std::vector<std::string_view> summaries;
      for (const auto& actionSpec : umbriel::actionSpecs()) {
        if (umbriel::groupForAction(actionSpec.action) != group) {
          continue;
        }
        names.emplace_back(
            actionSpec.param.empty() ? std::string(actionSpec.name)
                                     : std::format("{} {}", actionSpec.name, actionSpec.param)
        );
        summaries.push_back(actionSpec.summary);
      }
      if (names.empty()) {
        continue;
      }
      // Pad per group: a single global column would push every summary past the width of the widest name in the
      // table, which only one group actually needs.
      size_t width = 0;
      for (const auto& name : names) {
        width = std::max(width, name.size());
      }
      std::println("");
      std::println("{}", umbriel::groupTitle(group));
      for (size_t i = 0; i < names.size(); ++i) {
        std::println("  {:<{}}  {}", names[i], width, summaries[i]);
      }
    }
    std::println("");
    std::println("These actions also complete in bash, zsh, and fish: load `umbriel completions <shell>`.");
  }

  // Runs an IpcCommandSpec whose words start at argv[first].
  int runSpecCommand(const umbriel::IpcCommandSpec& spec, int argc, char** argv, int first) {

    if (!spec.takesArgs) {
      bool jf = false;
      for (int i = first; i < argc; ++i) {
        if (isHelpFlag(argv[i])) {
          printHelp(stdout);
          return EXIT_SUCCESS;
        }
        if (isJsonFlag(argv[i])) {
          jf = true;
        } else {
          printHelp(stderr);
          return EXIT_FAILURE;
        }
      }
      return umbriel::runIpcCommand(spec, {}, jf);
    }

    if (argc == first + 1 && isHelpFlag(argv[first])) {
      std::println("Usage: umbriel {} {}", umbriel::cliName(spec), spec.argSpec);
      std::println("");
      std::println("{}", spec.description);
      return EXIT_SUCCESS;
    }

    bool jf = false;
    std::vector<std::string> args;
    for (int i = first; i < argc; ++i) {
      if (isJsonFlag(argv[i])) {
        jf = true;
      } else {
        args.emplace_back(argv[i]);
      }
    }

    if (args.empty()) {
      printHelp(stderr);
      return EXIT_FAILURE;
    }
    return umbriel::runIpcCommand(spec, args, jf);
  }

  // Long enough for one inspect reply on a healthy compositor, short enough that a stopped one cannot stall a
  // keystroke. Completion prints nothing when it expires.
  constexpr int kCompletionLiveBudgetMs = 120;

  void addUnique(std::vector<std::string>& values, std::string value) {
    if (!value.empty() && std::ranges::find(values, value) == values.end()) {
      values.push_back(std::move(value));
    }
  }

  bool completionLiveEnabled() {
    const char* value = std::getenv("UMBRIEL_COMPLETE_LIVE");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }

  // The shells `umbriel completions` accepts, spelled from the one table the engine and the scripts read.
  std::string completionShellList(std::string_view separator) {
    std::string list;
    for (const umbriel::CompletionShellName& shell : umbriel::completionShellNames()) {
      if (!list.empty()) {
        list += separator;
      }
      list += shell.name;
    }
    return list;
  }

  // One deadline for the whole request: every live query spends what is left of it, so a stalled compositor cannot
  // delay the keystroke by more than `kCompletionLiveBudgetMs` however many families the position needs.
  class LiveBudget {
  public:
    std::chrono::steady_clock::time_point deadline() const { return m_deadline; }

  private:
    std::chrono::steady_clock::time_point m_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kCompletionLiveBudgetMs);
  };

  // The configuration is parsed once per request and only when a position the engine reaches asks for a configured
  // name.
  const umbriel::Config& completionConfig(bool& loaded) {
    if (!loaded) {
      umbriel::loadConfig(nullptr);
      loaded = true;
    }
    return umbriel::config();
  }

  // A field of the expected type, or a default for a reply that carries a different one. `value` throws on a
  // mismatched type, and a completion must degrade to the compiled-in vocabulary rather than abort a keystroke.
  std::string stringField(const nlohmann::json& entry, std::string_view key) {
    const auto it = entry.find(std::string(key));
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

  bool boolField(const nlohmann::json& entry, std::string_view key) {
    const auto it = entry.find(std::string(key));
    return it != entry.end() && it->is_boolean() && it->get<bool>();
  }

  // `section` names the array to read when the reply is an object, as `effects` wraps its presets and pools in keys;
  // `requiresTrue` names a boolean field an entry must carry to be collected.
  void collectLiveNames(
      std::vector<std::string>& target, std::string_view command, std::string_view key, const LiveBudget& budget,
      std::string_view section = {}, std::string_view requiresTrue = {}
  ) {
    // The query measures what the shared deadline has left for each system call, and issues none once it is spent.
    const std::optional<nlohmann::json> reply = umbriel::queryIpcJson(command, budget.deadline());
    if (!reply) {
      return;
    }
    const nlohmann::json* json = &*reply;
    if (!section.empty()) {
      if (!json->is_object()) {
        return;
      }
      const auto it = json->find(std::string(section));
      if (it == json->end()) {
        return;
      }
      json = &*it;
    }
    if (!json->is_array()) {
      return;
    }
    for (const auto& entry : *json) {
      if (!entry.is_object()) {
        continue;
      }
      if (!requiresTrue.empty() && !boolField(entry, requiresTrue)) {
        continue;
      }
      const std::string name = stringField(entry, key);
      if (!name.empty()) {
        addUnique(target, name);
      }
    }
  }

  // Every family loads on first use, so a position that reads none of them parses no configuration and contacts no
  // compositor.
  umbriel::CompletionValues completionValues() {
    const auto loaded = std::make_shared<bool>(false);
    const auto budget = std::make_shared<LiveBudget>();
    const bool live = completionLiveEnabled();

    const auto configNames = [loaded](std::vector<std::string>& target, const auto& take) {
      for (const auto& entry : take(completionConfig(*loaded))) {
        addUnique(target, entry.name);
      }
    };

    umbriel::CompletionValues values;
    values.outputs = umbriel::CompletionList([=](std::vector<std::string>& target) {
      configNames(target, [](const umbriel::Config& config) -> const auto& { return config.outputs; });
      if (live) {
        collectLiveNames(target, "outputs", "name", *budget);
      }
    });
    values.windows = umbriel::CompletionList([=](std::vector<std::string>& target) {
      if (live) {
        collectLiveNames(target, "windows", "id", *budget);
      }
    });
    // A workspace is named either by a `[[workspace]]` rule or by a string inventory on an output; a numeric
    // inventory creates anonymous members, which only a positional index reaches — completion never offers them.
    values.workspaces = umbriel::CompletionList([=](std::vector<std::string>& target) {
      const umbriel::Config& config = completionConfig(*loaded);
      configNames(target, [](const umbriel::Config& loadedConfig) -> const auto& {
        return loadedConfig.workspaceRules;
      });
      for (const umbriel::OutputRule& output : config.outputs) {
        if (!output.workspaces) {
          continue;
        }
        if (const auto* names = std::get_if<std::vector<std::string>>(&*output.workspaces)) {
          for (const std::string& name : *names) {
            addUnique(target, name);
          }
        }
      }
      if (live) {
        // A live reply lists every member, and an anonymous one carries its index as a name, which `--name` refuses.
        collectLiveNames(target, "workspaces", "name", *budget, {}, "named");
      }
    });
    // A slot accepts only names of its own kind, and only a cycle demands a pool, so the kind is asked for here and
    // the filter is the validator's own predicate rather than a second copy of the rule.
    values.effects = umbriel::EffectCandidates([=](std::vector<std::string>& presets, std::vector<std::string>& pools,
                                                   umbriel::EffectKind kind) {
      const umbriel::Effects& effects = completionConfig(*loaded).effects;
      for (const umbriel::EffectPreset& preset : effects.presets) {
        if (umbriel::effectReferenceAccepted(
                effects, preset.name, kind, false, umbriel::EffectReferenceConstraint::PresetOrPool
            )) {
          addUnique(presets, preset.name);
        }
      }
      for (const umbriel::EffectPool& pool : effects.pools) {
        if (umbriel::effectReferenceAccepted(
                effects, pool.name, kind, false, umbriel::EffectReferenceConstraint::PoolRequired
            )) {
          addUnique(pools, pool.name);
        }
      }
      if (!live) {
        return;
      }
      const std::optional<nlohmann::json> reply = umbriel::queryIpcJson("effects", budget->deadline());
      if (!reply) {
        return;
      }
      // The running compositor answers from its own configuration, which need not be the file this process discovers
      // (it may have started with `-c`, or hold a last-good config a broken file replaced). Its entry's own `kind` and
      // the section it sits in are the only claims that hold, so they decide rather than this process's tables.
      if (!reply->is_object()) {
        return;
      }
      for (const char* section : {"pools", "presets"}) {
        const auto it = reply->find(section);
        if (it == reply->end() || !it->is_array()) {
          continue;
        }
        for (const auto& entry : *it) {
          if (!entry.is_object() || stringField(entry, "kind") != umbriel::effectKindName(kind)) {
            continue;
          }
          addUnique(std::string_view(section) == "pools" ? pools : presets, stringField(entry, "name"));
        }
      }
    });
    values.scratchpads = umbriel::CompletionList([=](std::vector<std::string>& target) {
      configNames(target, [](const umbriel::Config& config) -> const auto& { return config.scratchpads; });
    });
    values.submaps = umbriel::CompletionList([loaded](std::vector<std::string>& target) {
      for (const auto& bind : completionConfig(*loaded).keybinds) {
        addUnique(target, bind.submap);
      }
    });
    return values;
  }
} // namespace

#ifdef UMBRIEL_USE_JEMALLOC
// Read by jemalloc before its first allocation. Few arenas and fast dirty/muzzy decay bound fragmentation and return
// freed pages to the OS promptly. The background thread runs decay even while the compositor is idle, otherwise pages
// freed by a burst would wait for the next allocation to be returned. thp:never keeps the kernel from backing the
// sparse heap with 2 MiB pages when transparent huge pages are set to `always`, which grew anonymous RSS by a quarter.
const char* malloc_conf =
    "background_thread:true,narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:5000,lg_tcache_max:12,thp:never";

#define UMBRIEL_STRINGIFY_HELPER(x) #x
#define UMBRIEL_STRINGIFY(x) UMBRIEL_STRINGIFY_HELPER(x)
#endif

int main(int argc, char** argv) {
#if defined(__GLIBC__) && !defined(UMBRIEL_USE_JEMALLOC)
  // glibc fallback when jemalloc is unavailable: a bounded arena count keeps a
  // long-running compositor from fragmenting across per-thread arenas.
  mallopt(M_ARENA_MAX, 2);
#endif
  if (argc >= 2) {

    // `config validate` and `config schema` read the config without a running compositor; the other verbs are runtime
    // overrides in the running one.
    if (argv[1] == umbriel::kCliConfig) {
      if (argc >= 3 && argv[2] == umbriel::kCliConfigValidate) {
        return validateConfig(argc, argv);
      }
      if (argc >= 3 && argv[2] == umbriel::kCliConfigSchema) {
        bool json = false;
        for (int i = 3; i < argc; ++i) {
          if (isHelpFlag(argv[i])) {
            printHelp(stdout);
            return EXIT_SUCCESS;
          }
          if (!isJsonFlag(argv[i])) {
            printHelp(stderr);
            return EXIT_FAILURE;
          }
          json = true;
        }
        return printConfigSchema(json);
      }
      if (argc >= 3) {
        if (const auto* spec = umbriel::findConfigCommand(argv[2]); spec != nullptr) {
          return runSpecCommand(*spec, argc, argv, 3);
        }
      }
      const bool help = argc >= 3 && isHelpFlag(argv[2]);
      printHelp(help ? stdout : stderr);
      return help ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argv[1] == umbriel::kCliHelp || argv[1] == umbriel::kCliHelpShortFlag || argv[1] == umbriel::kCliHelpFlag) {
      printHelp(stdout);
      return EXIT_SUCCESS;
    }
    if (argv[1] == umbriel::kCliVersionFlag
        || argv[1] == umbriel::kCliVersionShortFlag
        || argv[1] == umbriel::kCliVersionUpperFlag) {
      constexpr std::string_view unknownRevision = "unknown";
      const std::string_view revision = umbriel::build_info::revision();
      if (!revision.empty() && revision != unknownRevision) {
        std::println("umbriel {} ({})", umbriel::build_info::version(), revision);
      } else {
        std::println("umbriel {}", umbriel::build_info::version());
      }
      return EXIT_SUCCESS;
    }

    // Completion reads the compiled tables, and the live state only when a session answers within the budget.
    if (argv[1] == umbriel::kCliCompletions) {
      if (argc == 3 && isHelpFlag(argv[2])) {
        std::println("Usage: umbriel completions <{}>", completionShellList("|"));
        return EXIT_SUCCESS;
      }
      if (argc != 3) {
        // Both a missing and an unknown shell are usage errors on 1, clear of the 2 and 3 the scripts delegate on.
        std::println(stderr, "error: completions needs a shell: {}", completionShellList(", "));
        return EXIT_FAILURE;
      }
      const auto shell = umbriel::completionShellNamed(argv[2]);
      if (!shell) {
        std::println(stderr, "error: unknown shell '{}': expected {}", argv[2], completionShellList(", "));
        return EXIT_FAILURE;
      }
      std::print("{}", umbriel::completionScript(*shell));
      return EXIT_SUCCESS;
    }

    if (argv[1] == umbriel::kCliComplete) {
      std::vector<std::string> words;
      for (int i = 2; i < argc; ++i) {
        if (i == 2 && std::strcmp(argv[i], "--") == 0) {
          continue;
        }
        words.emplace_back(argv[i]);
      }
      // Completing the first word passes no argument at all; the word under the cursor is then empty.
      if (words.empty()) {
        words.emplace_back();
      }
      // stdout is machine-readable and stderr silent here, so nothing the configuration load finds may be narrated.
      setConsoleLogging(false);
      const umbriel::CompletionValues values = completionValues();
      const umbriel::CompletionOutcome outcome =
          umbriel::completeWords(words, values, [](std::string_view value, std::string_view description) {
            // The protocol is one `value<TAB>description` line, so a value carrying a separator, as a window title
            // may, would forge or split a candidate.
            if (value.find_first_of("\t\n") != std::string_view::npos) {
              return;
            }
            if (description.empty()) {
              std::println("{}", value);
            } else {
              std::println("{}\t{}", value, description);
            }
          });
      switch (outcome) {
      case umbriel::CompletionOutcome::Candidates:
        return EXIT_SUCCESS;
      // The shell owns the position: 2 asks for a path, 3 for a command line.
      case umbriel::CompletionOutcome::Delegate:
        return 2;
      case umbriel::CompletionOutcome::DelegateCommand:
        return 3;
      }
      return EXIT_SUCCESS;
    }

    // IPC subcommands

    // Not an IpcCommandSpec: the reply is a stream, not one response, so it has its own client path.
    if (argv[1] == umbriel::kCliSubscribe) {
      std::vector<std::string> events;
      for (int i = 2; i < argc; ++i) {
        if (isHelpFlag(argv[i])) {
          printHelp(stdout);
          return EXIT_SUCCESS;
        }
        // Every event is already a JSON line; the flag is accepted so scripts can pass it uniformly.
        if (isJsonFlag(argv[i])) {
          continue;
        }
        // Comma-separated or repeated arguments, so `subscribe workspaces,windows` and `subscribe workspaces windows`
        // both work.
        std::string_view arg{argv[i]};
        while (!arg.empty()) {
          const size_t comma = arg.find(',');
          std::string_view name = arg.substr(0, comma);
          if (!name.empty()) {
            events.emplace_back(name);
          }
          if (comma == std::string_view::npos) {
            break;
          }
          arg.remove_prefix(comma + 1);
        }
      }
      if (events.empty()) {
        std::println(stderr, "error: subscribe requires at least one event");
        printHelp(stderr);
        return EXIT_FAILURE;
      }
      return umbriel::runIpcSubscribe(events);
    }

    // Not an IpcCommandSpec: the words are parsed here and sent as a structured action request.
    if (argv[1] == umbriel::kCliMsg) {
      if (argc == 3 && isHelpFlag(argv[2])) {
        printActionHelp();
        return EXIT_SUCCESS;
      }
      // Only flags before the action name are msg's own; everything after it belongs to the action.
      bool jf = false;
      std::vector<std::string> words;
      for (int i = 2; i < argc; ++i) {
        if (words.empty() && isJsonFlag(argv[i])) {
          jf = true;
        } else {
          words.emplace_back(argv[i]);
        }
      }
      if (words.empty()) {
        printHelp(stderr);
        return EXIT_FAILURE;
      }
      return umbriel::runIpcAction(words, jf);
    }

    if (const auto* spec = umbriel::findIpcCommand(argv[1]);
        spec != nullptr && spec->group != umbriel::IpcCommandGroup::Config) {
      return runSpecCommand(*spec, argc, argv, 2);
    }
  }

  // If argv[1] exists and doesn't start with '-', it's an unknown subcommand.
  if (argc >= 2 && argv[1][0] != '-') {
    printHelp(stderr);
    return EXIT_FAILURE;
  }

  initLogFile();
  umbriel::setConfigDiagnosticLogging(true);
  // wlroots debug output is mostly per-frame scan-out and buffer chatter, so it stays opt-in.
  const char* wlrDebug = std::getenv("UMBRIEL_WLR_DEBUG");
  const bool wlrDebugEnabled = wlrDebug != nullptr && std::strcmp(wlrDebug, "1") == 0;
  wlr_log_init(wlrDebugEnabled ? WLR_DEBUG : WLR_INFO, wlrLogHandler);

  raiseFileDescriptorLimit();

  const char* startupCmd = nullptr;
  const char* configPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == umbriel::kCliSpawnFlag && i + 1 < argc) {
      startupCmd = argv[++i];
    } else if (argv[i] == umbriel::kCliConfigFlag && i + 1 < argc) {
      configPath = argv[++i];
    } else {
      printHelp(stderr);
      return EXIT_FAILURE;
    }
  }

  try {
    kLog.info("starting umbriel version={} commit={}", umbriel::build_info::version(), umbriel::build_info::revision());
    umbriel::loadConfig(configPath);
    umbriel::Server server;

    // SIGINT and SIGTERM are handled on the event loop by the server itself.
    // Exited children are reaped by a SIGCHLD handler. Every fork in Server
    // restores the default before exec.
    umbriel::installChildReaper();

    if (!server.start(startupCmd)) {
      kLog.error("failed to start server");
      return EXIT_FAILURE;
    }

    // Startup allocates heavily (config, scene tree, xwayland, banners) and none of that peak is needed once the
    // session is up. Purge the excess pages so steady-state RSS tracks what the session actually uses.
#ifdef UMBRIEL_USE_JEMALLOC
    // Log the effective configuration once so a wrong malloc_conf shows up in
    // the startup log instead of silently degrading allocator behavior.
    unsigned narenas = 0;
    size_t size = sizeof(narenas);
    mallctl("opt.narenas", &narenas, &size, nullptr, 0);
    size_t dirtyDecayMs = 0;
    size = sizeof(dirtyDecayMs);
    mallctl("opt.dirty_decay_ms", &dirtyDecayMs, &size, nullptr, 0);
    kLog.info("jemalloc: narenas={} dirty_decay_ms={}ms", narenas, dirtyDecayMs);

    const int purgeResult =
        mallctl("arena." UMBRIEL_STRINGIFY(MALLCTL_ARENAS_ALL) ".purge", nullptr, nullptr, nullptr, 0);
    if (purgeResult != 0) {
      kLog.warn("failed to purge jemalloc arenas: {}", std::strerror(purgeResult));
    }
#elifdef __GLIBC__
    malloc_trim(0);
#endif

    server.run();
    kLog.info("shutting down");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    kLog.error("{}", ex.what());
    return EXIT_FAILURE;
  }
}
