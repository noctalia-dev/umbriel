// docs/user/actions.md is the action reference. Its tables are the same strings `umbriel msg --help` prints, so a new
// action, a renamed one, or a reworded summary that never reaches the doc is a failure here rather than a silently
// stale page. The doc is also required to group actions exactly as the cheatsheet and the CLI do: one taxonomy, three
// renderings.

#include "check.h"
#include "config/keybind_parse.h"
#include "scene/cheatsheet_rows.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef UMBRIEL_DOCS_ACTIONS_MD
#error "UMBRIEL_DOCS_ACTIONS_MD must point at docs/user/actions.md"
#endif

using umbriel::ActionSpec;
using umbriel::fixedGroupOrder;
using umbriel::Group;
using umbriel::groupForAction;
using umbriel::groupTitle;

namespace {

  std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
      text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
    return text;
  }

  // A markdown row's cells, without the leading and trailing pipe. `\|` is a literal pipe inside a cell, not a cell
  // boundary: `workspace-set-layout:<scrolling|dwindle|master|toggle>` has to survive as one cell.
  std::vector<std::string_view> cells(std::string_view row) {
    std::vector<std::string_view> result;
    row = trim(row);
    if (!row.starts_with('|')) {
      return result;
    }
    row.remove_prefix(1);
    if (row.ends_with('|')) {
      row.remove_suffix(1);
    }
    size_t start = 0;
    size_t cursor = 0;
    while (true) {
      const size_t separator = row.find('|', cursor);
      if (separator != std::string_view::npos && separator > 0 && row[separator - 1] == '\\') {
        cursor = separator + 1;
        continue;
      }
      result.push_back(trim(row.substr(start, separator == std::string_view::npos ? separator : separator - start)));
      if (separator == std::string_view::npos) {
        break;
      }
      start = separator + 1;
      cursor = start;
    }
    return result;
  }

  bool isSeparatorCell(std::string_view cell) {
    return !cell.empty() && cell.find_first_not_of("-:") == std::string_view::npos;
  }

  // `window-focus-left` or `window-set-primary-extent:<fraction>`: the exact spelling `msg --help` prints, wrapped in a
  // code span, with any `|` escaped for the markdown table. Anything else in a two-column table (argument forms, config
  // keys, prose) is not an action row.
  bool splitActionCell(std::string_view cell, std::string& name, std::string& param) {
    if (cell.size() < 3 || cell.front() != '`' || cell.back() != '`') {
      return false;
    }
    const std::string_view token = cell.substr(1, cell.size() - 2);
    const size_t colon = token.find(':');
    const std::string_view namePart = token.substr(0, colon);
    if (namePart.empty() || namePart.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string_view::npos) {
      return false;
    }
    name = std::string(namePart);
    param.clear();
    if (colon == std::string_view::npos) {
      return true;
    }
    for (const std::string_view rest = token.substr(colon + 1); const char character : rest) {
      if (character != '\\') {
        param.push_back(character);
      }
    }
    return true;
  }

  struct DocRow {
    std::string param;
    std::string summary;
    std::string section;
  };

  struct DocTables {
    std::map<std::string, DocRow> rows;
    std::vector<std::string> duplicates;   // an action documented twice would hide one of the two rows
    std::vector<std::string> sectionOrder; // group sections, in the order the doc presents them
    bool read = false;
  };

  const DocTables& docTables() {
    static const DocTables tables = [] {
      DocTables parsed;
      std::ifstream file(UMBRIEL_DOCS_ACTIONS_MD);
      if (!file) {
        return parsed;
      }
      parsed.read = true;

      std::vector<std::string> groupSections;
      for (const Group group : fixedGroupOrder()) {
        groupSections.emplace_back(groupTitle(group));
      }

      std::string line;
      std::string section;
      bool inGroupSection = false;
      while (std::getline(file, line)) {
        if (line.starts_with("## ")) {
          section = std::string(trim(std::string_view(line).substr(3)));
          inGroupSection = std::ranges::find(groupSections, section) != groupSections.end();
          if (inGroupSection) {
            parsed.sectionOrder.push_back(section);
          }
          continue;
        }
        if (!inGroupSection || !trim(line).starts_with('|')) {
          continue;
        }
        const auto row = cells(line);
        if (row.size() != 2 || isSeparatorCell(row[0])) {
          continue;
        }
        std::string name;
        std::string param;
        if (!splitActionCell(row[0], name, param)) {
          continue;
        }
        if (!parsed.rows.emplace(name, DocRow{.param = param, .summary = std::string(row[1]), .section = section})
                 .second) {
          parsed.duplicates.push_back(name);
        }
      }
      return parsed;
    }();
    return tables;
  }

} // namespace

UMBRIEL_TEST(actionsDocIsReadable) {
  CHECK(docTables().read);
  CHECK(!docTables().rows.empty());
}

UMBRIEL_TEST(everyActionSpecHasADocRowWithTheSameTextAndGroup) {
  const DocTables& doc = docTables();
  for (const ActionSpec& spec : umbriel::actionSpecs()) {
    const auto row = doc.rows.find(std::string(spec.name));
    if (row == doc.rows.end()) {
      ::umbriel::test::reportFailure(__FILE__, __LINE__, std::format("{} is missing from actions.md", spec.name));
      continue;
    }
    CHECK_EQ(row->second.param, std::string(spec.param));
    CHECK_EQ(row->second.summary, std::string(spec.summary));
    CHECK_EQ(row->second.section, std::string(groupTitle(groupForAction(spec.action))));
  }
}

UMBRIEL_TEST(actionsDocListsEveryActionOnceAndNothingElse) {
  for (const auto& name : docTables().duplicates) {
    ::umbriel::test::reportFailure(__FILE__, __LINE__, std::format("actions.md documents `{}` more than once", name));
  }
  for (const auto& [name, row] : docTables().rows) {
    const bool known =
        std::ranges::any_of(umbriel::actionSpecs(), [&name](const ActionSpec& spec) { return spec.name == name; });
    if (!known) {
      ::umbriel::test::reportFailure(
          __FILE__, __LINE__, std::format("actions.md documents `{}`, which is not an action", name)
      );
    }
  }
}

UMBRIEL_TEST(actionsDocSectionsFollowTheCheatsheetOrder) {
  std::vector<std::string> expected;
  for (const Group group : fixedGroupOrder()) {
    expected.emplace_back(groupTitle(group));
  }
  CHECK_EQ(docTables().sectionOrder, expected);
}

int main() { return RUN_TESTS(); }
