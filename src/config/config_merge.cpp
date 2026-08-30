#include "config/config_merge.h"

#include "config/config_diag.h"
#include "config/section.h"
#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace umbriel::configmerge {

  namespace {

    constexpr Logger kLog("config");

    void
    emit(MergeResult& result, ConfigDiagnostic::Severity severity, const toml::source_region* src, std::string msg) {
      ConfigDiagnostic diag;
      diag.severity = severity;
      diag.message = msg;
      if (src != nullptr) {
        diag.line = src->begin.line;
        diag.column = src->begin.column;
        if (src->path != nullptr) {
          diag.file = *src->path;
        }
      }
      const std::string loc = diag.location();
      if (severity == ConfigDiagnostic::Severity::Error) {
        kLog.error("{}{}", loc.empty() ? "" : loc + ": ", msg);
      } else {
        kLog.warn("{}{}", loc.empty() ? "" : loc + ": ", msg);
      }
      result.diagnostics.push_back(std::move(diag));
    }

    std::filesystem::path canonicalKey(const std::filesystem::path& path) {
      std::error_code error;
      auto key = std::filesystem::weakly_canonical(path, error);
      return error ? path.lexically_normal() : key;
    }

    // toml++ 3.4 corrupts quoted duplicate keys in parse-error descriptions.
    // Reconstruct the key from source until the minimum supported version fixes it.
    std::string keyAtLine(const std::filesystem::path& path, toml::source_index lineNumber) {
      if (lineNumber == 0) {
        return {};
      }

      std::ifstream input(path);
      std::string line;
      for (toml::source_index current = 1; current <= lineNumber; ++current) {
        if (!std::getline(input, line)) {
          return {};
        }
      }

      char quote = '\0';
      bool escaped = false;
      for (size_t i = 0; i < line.size(); ++i) {
        const char character = line[i];
        if (quote != '\0') {
          if (quote == '"' && character == '\\' && !escaped) {
            escaped = true;
            continue;
          }
          if (character == quote && !escaped) {
            quote = '\0';
          }
          escaped = false;
          continue;
        }
        if (character == '"' || character == '\'') {
          quote = character;
          continue;
        }
        if (character != '=') {
          continue;
        }

        size_t begin = 0;
        while (begin < i && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
          ++begin;
        }
        size_t end = i;
        while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
          --end;
        }
        return line.substr(begin, end - begin);
      }
      return {};
    }

    std::string parseErrorMessage(const toml::parse_error& error, const std::filesystem::path& path) {
      std::string message(error.description());
      constexpr std::string_view duplicatePrefix = "Error while parsing key-value pair: cannot redefine existing ";
      if (!message.starts_with(duplicatePrefix)) {
        return message;
      }

      const std::string key = keyAtLine(path, error.source().begin.line);
      const size_t recordedKey = message.find(" '", duplicatePrefix.size());
      if (key.empty() || recordedKey == std::string::npos) {
        return message;
      }

      // Keep the parser's context and value type, but use the source spelling.
      return message.substr(0, recordedKey + 1) + "'" + key + "'";
    }

    enum class MultilineString {
      None,
      Basic,
      Literal,
    };

    std::string structuralTomlLine(std::string_view line, MultilineString& multiline) {
      std::string code;
      for (size_t index = 0; index < line.size();) {
        if (multiline == MultilineString::Basic) {
          if (line.substr(index).starts_with(R"(""")")) {
            multiline = MultilineString::None;
            index += 3;
          } else if (line[index] == '\\' && index + 1 < line.size()) {
            index += 2;
          } else {
            ++index;
          }
          continue;
        }
        if (multiline == MultilineString::Literal) {
          if (line.substr(index).starts_with("'''")) {
            multiline = MultilineString::None;
            index += 3;
          } else {
            ++index;
          }
          continue;
        }

        if (line[index] == '#') {
          break;
        }
        if (line.substr(index).starts_with(R"(""")")) {
          multiline = MultilineString::Basic;
          index += 3;
          continue;
        }
        if (line.substr(index).starts_with("'''")) {
          multiline = MultilineString::Literal;
          index += 3;
          continue;
        }
        if (line[index] == '"' || line[index] == '\'') {
          const char quote = line[index];
          code.push_back(line[index++]);
          bool escaped = false;
          while (index < line.size()) {
            const char character = line[index++];
            code.push_back(character);
            if (quote == '"' && character == '\\' && !escaped) {
              escaped = true;
              continue;
            }
            if (character == quote && !escaped) {
              break;
            }
            escaped = false;
          }
          continue;
        }
        code.push_back(line[index++]);
      }
      return code;
    }

    // toml++ does not return a partial table on syntax errors. Inspect only
    // structural source tokens so a malformed policy still fails closed, while
    // comments and multiline string contents cannot look like a real [drm]
    // section.
    bool parseErrorMayDiscardDrmPolicy(const std::filesystem::path& path) {
      std::ifstream input(path);
      std::string line;
      MultilineString multiline = MultilineString::None;
      bool inDrmTable = false;
      while (std::getline(input, line)) {
        std::string code = structuralTomlLine(line, multiline);
        std::erase_if(code, [](unsigned char character) { return std::isspace(character) != 0; });
        if (code == "[drm]") {
          inDrmTable = true;
          continue;
        }
        const bool drmTable = code.starts_with("[drm") && (code.size() == 4 || code[4] == '.' || code[4] == ']');
        const bool drmArray = code.starts_with("[[drm") && (code.size() == 5 || code[5] == '.' || code[5] == ']');
        if (drmTable || drmArray) {
          return true;
        }
        if (code.starts_with('[')) {
          inDrmTable = false;
          continue;
        }
        if (code.empty()) {
          continue;
        }
        if (inDrmTable || code.starts_with("drm.") || (code.starts_with("drm=") && code != "drm={}")) {
          return true;
        }
      }
      return false;
    }

    std::string expandEnvironment(std::string_view input) {
      std::string result;
      result.reserve(input.size());
      const auto isNameStart = [](char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::isalpha(value) != 0 || c == '_';
      };
      const auto isNameCharacter = [](char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::isalnum(value) != 0 || c == '_';
      };

      for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '$') {
          result.push_back(input[i++]);
          continue;
        }
        if (i + 1 < input.size() && input[i + 1] == '{') {
          const std::size_t close = input.find('}', i + 2);
          if (close != std::string_view::npos) {
            const std::string name(input.substr(i + 2, close - i - 2));
            if (const char* value = std::getenv(name.c_str()); value != nullptr) {
              result.append(value);
            }
            i = close + 1;
            continue;
          }
        } else if (i + 1 < input.size() && isNameStart(input[i + 1])) {
          std::size_t end = i + 2;
          while (end < input.size() && isNameCharacter(input[end])) {
            ++end;
          }
          const std::string name(input.substr(i + 1, end - i - 1));
          if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            result.append(value);
          }
          i = end;
          continue;
        }
        result.push_back(input[i++]);
      }
      return result;
    }

    std::filesystem::path expandPath(std::string_view raw, const std::filesystem::path& baseDir) {
      std::string expanded = expandEnvironment(raw);
      std::filesystem::path path;
      if (expanded == "~") {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          path = home;
        } else {
          path = expanded;
        }
      } else if (expanded.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          path = std::filesystem::path(home) / expanded.substr(2);
        } else {
          path = expanded;
        }
      } else {
        path = expanded;
      }
      if (!path.is_absolute()) {
        path = baseDir / path;
      }
      return path.lexically_normal();
    }

    struct IncludeDirective {
      struct Entry {
        std::string path;
        toml::source_region source;
      };
      std::vector<Entry> entries;
    };

    IncludeDirective readInclude(const toml::table& table, MergeResult& result) {
      IncludeDirective directive;
      const toml::node* node = table.get("include");
      if (node == nullptr) {
        return directive;
      }
      const auto* include = node->as_table();
      if (include == nullptr) {
        emit(result, ConfigDiagnostic::Severity::Warning, &node->source(), "ignoring include (expected table)");
        return directive;
      }
      // `include` is erased from the table before the config readers run, so the root section never sees it and never
      // reports its unknown keys. This reader owns that report for the section's fixed vocabulary.
      Section keys(*include, "include", result.diagnostics);
      const toml::node* filesNode = keys.take("files");
      if (filesNode == nullptr) {
        return directive;
      }
      const auto* files = filesNode->as_array();
      if (files == nullptr) {
        emit(
            result, ConfigDiagnostic::Severity::Warning, &filesNode->source(),
            "ignoring include.files (expected array of strings)"
        );
        return directive;
      }
      for (const auto& entry : *files) {
        if (!entry.is_string()) {
          emit(
              result, ConfigDiagnostic::Severity::Warning, &entry.source(),
              "ignoring include.files (expected array of strings)"
          );
          directive.entries.clear();
          return directive;
        }
        directive.entries.push_back({
            .path = *entry.value<std::string>(),
            .source = entry.source(),
        });
      }
      return directive;
    }

    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& result);

    toml::table expandFile(
        const std::filesystem::path& path, toml::table parsed, std::set<std::filesystem::path>& visited,
        MergeResult& result
    ) {
      const auto key = canonicalKey(path);
      if (visited.contains(key)) {
        emit(
            result, ConfigDiagnostic::Severity::Warning, nullptr,
            std::format("include cycle or duplicate skipped: {}", key.string())
        );
        return {};
      }
      visited.insert(key);
      result.loadedFiles.push_back(key);

      IncludeDirective directive = readInclude(parsed, result);
      parsed.erase("include");

      if (directive.entries.empty()) {
        // No includes: return parsed directly, preserving toml++ source regions
        // (copies lose them; only moves keep line/column/path).
        return parsed;
      }

      toml::table base;
      for (const auto& entry : directive.entries) {
        const auto target = expandPath(entry.path, path.parent_path());
        std::error_code error;
        if (std::filesystem::is_regular_file(target, error) && !error) {
          deepMerge(base, loadAndExpand(target, visited, result));
          continue;
        }
        result.missingIncludes = true;
        emit(
            result, ConfigDiagnostic::Severity::Warning, &entry.source,
            std::format("include not found: {} (from {})", target.string(), path.string())
        );
        result.loadedFiles.push_back(canonicalKey(target));
      }
      deepMerge(base, std::move(parsed));
      return base;
    }

    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& result) {
      toml::table parsed;
      try {
        parsed = toml::parse_file(path.string());
      } catch (const toml::parse_error& error) {
        result.hadParseError = true;
        result.parseErrorMayDiscardDrmPolicy =
            result.parseErrorMayDiscardDrmPolicy || parseErrorMayDiscardDrmPolicy(path);
        const auto key = canonicalKey(path);
        if (std::ranges::find(result.loadedFiles, key) == result.loadedFiles.end()) {
          result.loadedFiles.push_back(key);
        }
        auto source = error.source();
        if (source.path == nullptr) {
          source.path = std::make_shared<const std::string>(path.string());
        }
        emit(result, ConfigDiagnostic::Severity::Error, &source, parseErrorMessage(error, path));
        return {};
      }
      return expandFile(path, std::move(parsed), visited, result);
    }

  } // namespace

  void deepMerge(toml::table& base, const toml::table& overlay) {
    for (const auto& [key, value] : overlay) {
      if (const auto* overlayTable = value.as_table()) {
        if (auto* baseNode = base.get(key)) {
          if (auto* baseTable = baseNode->as_table()) {
            deepMerge(*baseTable, *overlayTable);
            continue;
          }
        }
      }
      base.insert_or_assign(key, value);
    }
  }

  void deepMerge(toml::table& base, toml::table&& overlay) {
    for (auto&& [key, value] : overlay) {
      if (auto* overlayTable = value.as_table()) {
        if (auto* baseNode = base.get(key)) {
          if (auto* baseTable = baseNode->as_table()) {
            deepMerge(*baseTable, std::move(*overlayTable));
            continue;
          }
        }
      }
      // Move via visit to preserve toml++ source regions (copies reset them).
      value.visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        base.insert_or_assign(std::string(key.str()), T(std::move(val)));
      });
    }
  }

  MergeResult mergeWithIncludes(const std::filesystem::path& rootFile) {
    MergeResult result;
    std::set<std::filesystem::path> visited;
    result.merged = loadAndExpand(rootFile, visited, result);
    return result;
  }

} // namespace umbriel::configmerge
