#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace umbriel {

  // How a column presents its views: stacked rows that share the column, or tabs that each get the whole column while
  // only one of them shows.
  enum class ColumnDisplay : uint8_t {
    Normal,
    Tabbed,
  };

  // Where a window joining a tabbed column lands among its tabs.
  enum class NewTabPosition : uint8_t {
    End,
    AfterActive,
  };

  // Which edge of a tabbed column carries its tab bar.
  enum class TabBarPosition : uint8_t {
    Top,
    Bottom,
    Left,
    Right,
  };

  // Whether a bar on that edge runs across the column, its slots side by side, rather than down it, stacked.
  constexpr bool tabBarAcross(TabBarPosition position) {
    return position == TabBarPosition::Top || position == TabBarPosition::Bottom;
  }

  // A column's display and the row it shows as its active tab. The selection follows its view through every row
  // change, tabbed or not, so tabbing a column later shows the row that was selected last. Every operation takes the
  // column's row count after the change and keeps the selection inside it.
  class TabState {
  public:
    constexpr bool tabbed() const { return m_tabbed; }
    constexpr size_t active() const { return m_active; }
    // Whether the bar is drawn, when the user said so; unset follows the configuration.
    constexpr std::optional<bool> bar() const { return m_bar; }
    constexpr void setBar(std::optional<bool> bar) { m_bar = bar; }

    // True when the display changed.
    constexpr bool setTabbed(bool tabbed, size_t rows) {
      clamp(rows);
      if (m_tabbed == tabbed) {
        return false;
      }
      m_tabbed = tabbed;
      return true;
    }

    // True when the selection moved to a different row.
    constexpr bool select(size_t row, size_t rows) {
      const size_t previous = m_active;
      m_active = row;
      clamp(rows);
      return m_active != previous;
    }

    // A row inserted at or before the selection pushes it forward, so the view on show stays the same. The first row
    // of an empty column becomes the selection.
    constexpr void rowInserted(size_t row, size_t rows) {
      if (rows > 1 && row <= m_active) {
        ++m_active;
      }
      clamp(rows);
    }

    // A row erased before the selection pulls it back. Erasing the selected row selects its predecessor, the row focus
    // falls back to when that window leaves, so the shown tab and the focus agree.
    constexpr void rowErased(size_t row, size_t rows) {
      if (row < m_active || (row == m_active && m_active > 0)) {
        --m_active;
      }
      clamp(rows);
    }

    // Two rows traded places: the selection stays with its view.
    constexpr void rowsSwapped(size_t first, size_t second) {
      if (m_active == first) {
        m_active = second;
      } else if (m_active == second) {
        m_active = first;
      }
    }

    constexpr void clamp(size_t rows) { m_active = rows == 0 ? 0 : std::min(m_active, rows - 1); }

    constexpr bool operator==(const TabState&) const = default;

  private:
    bool m_tabbed = false;
    size_t m_active = 0;
    std::optional<bool> m_bar;
  };

  // A run of a column's rows shown as tabs. The run is one unit of the column's stack, sized like a single window, and
  // only its active row shows in it. Rows are counted in the column.
  struct TabGroup {
    size_t first = 0;
    size_t count = 0;
    size_t active = 0;
    // Whether the group draws its bar, when the user said so; unset follows the configuration.
    std::optional<bool> bar;

    constexpr size_t end() const { return first + count; }
    constexpr bool contains(size_t row) const { return row >= first && row < end(); }
    constexpr bool operator==(const TabGroup&) const = default;
  };

  // The tab groups of one column, in row order and never overlapping; every other row stands alone as its own unit.
  // Membership is by position: a row belongs to the group whose span covers it, so the groups follow every row change
  // the column reports, and each group's active row follows its view while it stays in the group.
  class ColumnTabs {
  public:
    bool empty() const { return m_groups.empty(); }
    const std::vector<TabGroup>& groups() const { return m_groups; }

    std::optional<size_t> groupIndexAt(size_t row) const {
      for (size_t index = 0; index < m_groups.size(); ++index) {
        if (m_groups[index].contains(row)) {
          return index;
        }
      }
      return std::nullopt;
    }

    const TabGroup* groupAt(size_t row) const {
      const std::optional<size_t> index = groupIndexAt(row);
      return index ? &m_groups[*index] : nullptr;
    }

    // The rows of the unit holding `row`: its group's, or the row's alone.
    size_t unitStart(size_t row) const {
      const TabGroup* group = groupAt(row);
      return group != nullptr ? group->first : row;
    }
    size_t unitEnd(size_t row) const {
      const TabGroup* group = groupAt(row);
      return group != nullptr ? group->end() : row + 1;
    }
    // The row the unit holding `row` shows.
    size_t unitShown(size_t row) const {
      const TabGroup* group = groupAt(row);
      return group != nullptr ? group->active : row;
    }
    // Units in a column of `rows` rows.
    size_t unitCount(size_t rows) const {
      size_t units = rows;
      for (const TabGroup& group : m_groups) {
        units -= std::min(units, group.count - 1);
      }
      return units;
    }
    // True for a tab its group is not showing.
    bool hidden(size_t row) const {
      const TabGroup* group = groupAt(row);
      return group != nullptr && group->active != row;
    }

    // A row inserted at `row` joins the group whose span is strictly around it, or group `into`, whose span must reach
    // it from either end. A row inserted at or before a group's active row pushes it forward, so the group keeps
    // showing the same view.
    void rowInserted(size_t row, std::optional<size_t> into = std::nullopt) {
      for (size_t index = 0; index < m_groups.size(); ++index) {
        TabGroup& group = m_groups[index];
        const bool joins =
            into == index ? row >= group.first && row <= group.end() : row > group.first && row < group.end();
        if (joins) {
          ++group.count;
          if (row <= group.active) {
            ++group.active;
          }
        } else if (row <= group.first) {
          ++group.first;
          ++group.active;
        }
      }
    }

    // Erasing a group's active row shows its predecessor, the row focus falls back to when that window leaves. A group
    // losing its last row is gone.
    void rowErased(size_t row) {
      for (TabGroup& group : m_groups) {
        if (group.contains(row)) {
          --group.count;
          if (row < group.active || (row == group.active && group.active > group.first)) {
            --group.active;
          }
        } else if (row < group.first) {
          --group.first;
          --group.active;
        }
      }
      std::erase_if(m_groups, [](const TabGroup& group) { return group.count == 0; });
    }

    // Two rows traded places. Within one group the active row stays with its view; across units each group keeps the
    // place it shows.
    void rowsSwapped(size_t first, size_t second) {
      for (TabGroup& group : m_groups) {
        if (!group.contains(first) || !group.contains(second)) {
          continue;
        }
        if (group.active == first) {
          group.active = second;
        } else if (group.active == second) {
          group.active = first;
        }
      }
    }

    // The neighbouring units at rows [first, middle) and [middle, last) traded places, each keeping its order: a group
    // in either moves with it.
    void unitsSwapped(size_t first, size_t middle, size_t last) {
      for (TabGroup& group : m_groups) {
        if (group.first >= first && group.first < middle) {
          group.first += last - middle;
          group.active += last - middle;
        } else if (group.first >= middle && group.first < last) {
          group.first -= middle - first;
          group.active -= middle - first;
        }
      }
      std::ranges::sort(m_groups, {}, &TabGroup::first);
    }

    // Group rows [first, first + count), showing `active`. False when the span is empty, `active` lies outside it, or
    // any of its rows is in a group already.
    bool form(size_t first, size_t count, size_t active, std::optional<bool> bar = std::nullopt) {
      if (count == 0 || active < first || active >= first + count) {
        return false;
      }
      for (const TabGroup& group : m_groups) {
        if (group.first < first + count && first < group.end()) {
          return false;
        }
      }
      const auto at = std::ranges::find_if(m_groups, [first](const TabGroup& group) { return group.first > first; });
      m_groups.insert(at, TabGroup{.first = first, .count = count, .active = active, .bar = bar});
      return true;
    }

    // Stack the rows of the group holding `row` again. False when there is none.
    bool dissolve(size_t row) {
      const std::optional<size_t> index = groupIndexAt(row);
      if (!index) {
        return false;
      }
      m_groups.erase(m_groups.begin() + static_cast<std::ptrdiff_t>(*index));
      return true;
    }

    // Show `row` in its group. True only when the group now shows a different row.
    bool select(size_t row) {
      const std::optional<size_t> index = groupIndexAt(row);
      if (!index || m_groups[*index].active == row) {
        return false;
      }
      m_groups[*index].active = row;
      return true;
    }

    // Take `row`, the first or last row of its group, out of the group to stand beside it. A group of one is gone.
    bool leave(size_t row) {
      const std::optional<size_t> index = groupIndexAt(row);
      if (!index) {
        return false;
      }
      TabGroup& group = m_groups[*index];
      if (group.count == 1) {
        m_groups.erase(m_groups.begin() + static_cast<std::ptrdiff_t>(*index));
        return true;
      }
      if (row == group.first) {
        ++group.first;
      } else if (row + 1 != group.end()) {
        return false;
      }
      --group.count;
      group.active = std::clamp(group.active, group.first, group.end() - 1);
      return true;
    }

    // Take `row`, standing alone just before or after group `into`, into that group as its first or last row.
    bool join(size_t row, size_t into) {
      if (into >= m_groups.size() || groupIndexAt(row)) {
        return false;
      }
      TabGroup& group = m_groups[into];
      if (row + 1 == group.first) {
        --group.first;
      } else if (row != group.end()) {
        return false;
      }
      ++group.count;
      return true;
    }

    // Draw or hide the bar of the group holding `row`; unset follows the configuration. False without a group.
    bool setBar(size_t row, std::optional<bool> bar) {
      const std::optional<size_t> index = groupIndexAt(row);
      if (!index) {
        return false;
      }
      m_groups[*index].bar = bar;
      return true;
    }

    bool operator==(const ColumnTabs&) const = default;

  private:
    std::vector<TabGroup> m_groups;
  };

} // namespace umbriel
