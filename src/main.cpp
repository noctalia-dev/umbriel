#include "cli/ipc_client.h"
#include "cli/outputs.h"
#include "config/config.h"
#include "config/config_diag.h"
#include "core/fdlimit.h"
#include "core/log.h"
#include "server/ipc_commands.h"
#include "server/server.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

#ifdef __GLIBC__
#ifdef UMBRIEL_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#else
#include <malloc.h>
#endif
#endif

#ifndef UMBRIEL_VERSION
#define UMBRIEL_VERSION "unknown"
#endif

namespace {
  constexpr Logger kLog("main");

  int validateConfig(int argc, char** argv) {
    const char* configPath = nullptr;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
        configPath = argv[++i];
      } else {
        std::println(stderr, "error: unknown option '{}' for validate", argv[i]);
        return EXIT_FAILURE;
      }
    }

    umbriel::loadConfig(configPath);
    const auto& diags = umbriel::configDiagnostics();
    if (diags.empty()) {
      std::println("config: ok ({})", umbriel::configRootPath().string());
      return EXIT_SUCCESS;
    }
    bool hasError = false;
    for (const auto& d : diags) {
      const std::string loc = d.location();
      if (d.severity == umbriel::ConfigDiagnostic::Severity::Error) {
        hasError = true;
        std::println(stderr, "error: {}{}", loc.empty() ? "" : loc + ": ", d.message);
      } else {
        std::println(stderr, "warning: {}{}", loc.empty() ? "" : loc + ": ", d.message);
      }
    }
    return hasError ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  void printHelp(FILE* stream) {
    auto row = [stream](std::string_view lead, std::string_view cmd, std::string_view desc) {
      std::println(stream, "{}umbriel {:<30} {}", lead, cmd, desc);
    };
    std::println(stream, "umbriel {}: a wayland compositor\n", UMBRIEL_VERSION);
    row("Usage: ", "[-s <command>] [-c <config>]", "run the compositor");
    for (const auto& spec : umbriel::ipcCommands()) {
      std::string cmd{spec.name};
      if (!spec.argSpec.empty()) {
        cmd += ' ';
        cmd += spec.argSpec;
      }
      row("       ", cmd, spec.description);
    }
    row("       ", "outputs", "list outputs and modes");
    row("       ", "validate [-c <config>]", "check the config file");
    row("       ", "help | -h | --help", "show this help");
    row("       ", "--version", "print version");
    std::println(
        stream,
        "\nOptions:\n"
        "  -s <command>   spawn <command> once the compositor starts\n"
        "  -c <config>    use <config> instead of the default config path\n"
        "\n"
        "Run `umbriel msg --help` to list all available actions for `msg` and keybinds."
    );
  }
} // namespace

#ifdef UMBRIEL_USE_JEMALLOC
// Read by jemalloc before its first allocation. Few arenas and fast dirty/muzzy decay bound fragmentation and return
// freed pages to the OS promptly. The background thread runs decay even while the compositor is idle, otherwise pages
// freed by a burst would wait for the next allocation to be returned.
const char* malloc_conf = "background_thread:true,narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:5000,lg_tcache_max:12";

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
    if (std::strcmp(argv[1], "validate") == 0) {
      return validateConfig(argc, argv);
    }
    if (std::strcmp(argv[1], "outputs") == 0) {
      return umbriel::runOutputsCommand();
    }
    if (std::strcmp(argv[1], "help") == 0 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
      printHelp(stdout);
      return EXIT_SUCCESS;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0) {
      std::println("umbriel {}", UMBRIEL_VERSION);
      return EXIT_SUCCESS;
    }

    // IPC subcommands
    auto isJsonFlag = [](const char* arg) { return std::strcmp(arg, "--json") == 0 || std::strcmp(arg, "-j") == 0; };
    auto isHelpFlag = [](const char* arg) { return std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0; };

    if (const auto* spec = umbriel::findIpcCommand(argv[1])) {
      if (!spec->takesArg) {
        bool jf = false;
        for (int i = 2; i < argc; ++i) {
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
        return umbriel::runIpcCommand(*spec, {}, jf);
      }

      // Help only when -h/--help is the immediate argument (not buried in spawn args).
      if (argc == 3 && isHelpFlag(argv[2])) {
        std::println("Usage: umbriel {} {}", spec->name, spec->argSpec);
        if (spec->name != "msg") {
          std::println("");
          std::println("{}", spec->description);
          return EXIT_SUCCESS;
        }
        std::println("");
        std::println("Send an action to the running compositor.");
        std::println("Use `msg spawn <cmd...>` as shorthand for `msg spawn:<cmd...>`.");
        std::println("");
        std::println("Available actions:");
        std::vector<const umbriel::ActionSpec*> sortedActions;
        sortedActions.reserve(umbriel::actionSpecs().size());
        for (const auto& actionSpec : umbriel::actionSpecs()) {
          sortedActions.push_back(&actionSpec);
        }
        std::ranges::sort(sortedActions, {}, [](const auto* actionSpec) { return actionSpec->name; });
        for (const auto* actionSpec : sortedActions) {
          if (actionSpec->param.empty()) {
            std::println("  {}", actionSpec->name);
          } else {
            std::println("  {}:{}", actionSpec->name, actionSpec->param);
          }
        }
        return EXIT_SUCCESS;
      }

      bool jf = false;
      // Collect non-flag args; for spawn, everything after "spawn" is literal.
      std::vector<const char*> args;
      bool inSpawnTail = false;
      for (int i = 2; i < argc; ++i) {
        if (!inSpawnTail && isJsonFlag(argv[i])) {
          jf = true;
        } else {
          args.push_back(argv[i]);
          // Once we see "spawn" as the first non-flag arg and there are more,
          // everything after belongs to the command (no flag stripping).
          if (args.size() == 1 && std::strcmp(argv[i], "spawn") == 0) {
            inSpawnTail = true;
          }
        }
      }

      if (args.empty()) {
        printHelp(stderr);
        return EXIT_FAILURE;
      }

      std::string actionString;
      if (std::strcmp(args[0], "spawn") == 0 && args.size() > 1) {
        actionString = "spawn:";
        for (size_t i = 1; i < args.size(); ++i) {
          if (i > 1) {
            actionString += ' ';
          }
          actionString += args[i];
        }
      } else {
        for (size_t i = 0; i < args.size(); ++i) {
          if (i > 0) {
            actionString += ' ';
          }
          actionString += args[i];
        }
      }
      return umbriel::runIpcCommand(*spec, actionString, jf);
    }
  }

  // If argv[1] exists and doesn't start with '-', it's an unknown subcommand.
  if (argc >= 2 && argv[1][0] != '-') {
    printHelp(stderr);
    return EXIT_FAILURE;
  }

  initLogFile();
#ifdef NDEBUG
  wlr_log_init(WLR_INFO, wlrLogHandler);
#else
  wlr_log_init(WLR_DEBUG, wlrLogHandler);
#endif

  raiseFileDescriptorLimit();

  const char* startupCmd = nullptr;
  const char* configPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      startupCmd = argv[++i];
    } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      configPath = argv[++i];
    } else {
      printHelp(stderr);
      return EXIT_FAILURE;
    }
  }

  try {
    kLog.info("starting umbriel");
    umbriel::loadConfig(configPath);
    umbriel::Server server;

    // SIGINT and SIGTERM are handled on the event loop by the server itself. SIG_IGN for SIGCHLD reaps spawned children
    // without a handler; every fork in Server restores the default before exec.
    std::signal(SIGCHLD, SIG_IGN);

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
#elif defined(__GLIBC__)
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
