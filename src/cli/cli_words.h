#pragma once

#include <string_view>

namespace umbriel {

  // The words the CLI spells for itself, shared by the dispatch in `main.cpp` and the completion engine, so a rename
  // cannot leave the two disagreeing about what the command line accepts.
  inline constexpr std::string_view kCliMsg = "msg";
  inline constexpr std::string_view kCliSubscribe = "subscribe";
  inline constexpr std::string_view kCliConfig = "config";
  inline constexpr std::string_view kCliCompletions = "completions";
  inline constexpr std::string_view kCliComplete = "complete";
  inline constexpr std::string_view kCliHelp = "help";
  inline constexpr std::string_view kCliConfigValidate = "validate";
  inline constexpr std::string_view kCliConfigSchema = "schema";
  inline constexpr std::string_view kCliJsonFlag = "--json";
  inline constexpr std::string_view kCliJsonShortFlag = "-j";
  inline constexpr std::string_view kCliSpawnFlag = "-s";
  inline constexpr std::string_view kCliConfigFlag = "-c";
  inline constexpr std::string_view kCliHelpShortFlag = "-h";
  inline constexpr std::string_view kCliHelpFlag = "--help";
  inline constexpr std::string_view kCliVersionFlag = "--version";
  inline constexpr std::string_view kCliVersionShortFlag = "-v";
  inline constexpr std::string_view kCliVersionUpperFlag = "-V";

} // namespace umbriel
