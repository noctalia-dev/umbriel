#include "scene/cheatsheet.h"

#include "config/config.h"
#include "scene/cheatsheet_rows.h"
#include "scene/color.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using umbriel::balancedColumnHeight;
  using umbriel::CheatsheetRow;
  using umbriel::escapeMarkup;
  using umbriel::Group;
  using umbriel::groupForAction;
  using umbriel::groupTitle;
  using umbriel::keycapBackgroundColor;
  using umbriel::renderTextBuffer;
  using umbriel::rgbaHex;
  using umbriel::TextBufferResult;

  struct CheatsheetPalette {
    std::string textPrimary;
    std::string textMuted;
    std::string accentPrimary;
    std::string accentSecondary;
    std::string warning;
    std::string keycapBackground;
  };

  CheatsheetPalette makeCheatsheetPalette(const umbriel::Config::Colors& colors) {
    const std::array<float, 4> keycapBackground = keycapBackgroundColor(colors.background, colors.textPrimary);
    return {
        .textPrimary = rgbaHex(colors.textPrimary),
        .textMuted = rgbaHex(colors.textMuted),
        .accentPrimary = rgbaHex(colors.accentPrimary),
        .accentSecondary = rgbaHex(colors.accentSecondary),
        .warning = rgbaHex(colors.warning),
        .keycapBackground = rgbaHex(keycapBackground),
    };
  }

  constexpr int kPad = 28;
  constexpr int kColumnGap = 32;
  constexpr int kTitleBodyGap = 12;
  constexpr int kBodyFooterGap = 12;
  constexpr int kColumnMaxWidth = 600;
  constexpr int kMaxColumns = 4;
  // Body font sizes, largest first. Dropping a point costs legibility on every line, so it is a last resort: only
  // reached once every column count has been tried and the panel still does not fit. 9 is the floor because below it
  // the chord pills stop being readable at arm's length, which is the whole job.
  constexpr int kFontSizes[] = {11, 10, 9};

  // Fixed groups in display order; submaps follow in first-seen order.
  constexpr Group kFixedGroups[] = {
      Group::Apps, Group::Focus, Group::MoveSize, Group::Windows, Group::Workspaces, Group::Overview, Group::System,
  };

  // A display line is either a group header or a bind row.
  struct DisplayLine {
    bool isHeader = false;
    bool isDitto = false;
    int group = 0;    // group index: lines with the same value are never split across columns
    std::string text; // Pango markup for the full line (header or row)
  };

  // A group header, preceded by a blank spacer unless it opens the list. The spacer carries the upcoming group's id
  // because a column break happens before it, never between it and its header.
  void
  pushHeader(std::vector<DisplayLine>& lines, int groupId, std::string_view title, const CheatsheetPalette& palette) {
    if (!lines.empty()) {
      lines.push_back({.isHeader = false, .group = groupId, .text = ""});
    }
    lines.push_back({
        .isHeader = true,
        .group = groupId,
        .text =
            std::format("<span foreground='{}' weight='bold'>{}</span>", palette.accentSecondary, escapeMarkup(title)),
    });
  }

  // One bind: the chord in a pill, then the label padded out to the group's
  // widest chord so the actions line up. A ditto repeat is dimmed.
  DisplayLine bindLine(
      const std::string& chord, const std::string& label, int groupId, size_t maxChordLen,
      const CheatsheetPalette& palette
  ) {
    const bool isDitto = label == "\xe2\x80\xb3";
    const size_t extraPad = maxChordLen > chord.size() ? maxChordLen - chord.size() : 0;
    return {
        .isHeader = false,
        .isDitto = isDitto,
        .group = groupId,
        .text = std::format(
            "<span background='{}' foreground='{}'> {} </span>{}  <span foreground='{}'>{}</span>",
            palette.keycapBackground, palette.accentPrimary, escapeMarkup(chord), std::string(extraPad, ' '),
            isDitto ? palette.textMuted : palette.textPrimary, escapeMarkup(label)
        ),
    };
  }

  // Apps: binaries bound once render as flat rows here; a binary bound to several distinct commands is promoted to its
  // own top-level section so its variants read as a set. Advances `groupId` for each section it opens.
  void pushAppsRows(
      std::vector<DisplayLine>& lines, int& groupId, const std::vector<const CheatsheetRow*>& groupRows,
      size_t maxChordLen, const CheatsheetPalette& palette
  ) {
    // Collect unique binaries in first-seen order.
    std::vector<std::string> binOrder;
    for (const auto* row : groupRows) {
      const std::string& bin = row->spawnBinary;
      if (std::ranges::find(binOrder, bin) == binOrder.end()) {
        binOrder.push_back(bin);
      }
    }

    // Partition: single-usage binaries render flat under "Apps",
    // multi-usage binaries are collected for their own groups.
    struct DeferredGroup {
      std::string title;
      std::vector<const CheatsheetRow*> rows;
    };
    std::vector<DeferredGroup> deferred;

    for (const auto& bin : binOrder) {
      std::vector<const CheatsheetRow*> binRows;
      for (const auto* row : groupRows) {
        if (row->spawnBinary == bin) {
          binRows.push_back(row);
        }
      }

      const auto realCount =
          std::ranges::count_if(binRows, [](const CheatsheetRow* r) { return r->action != "\xe2\x80\xb3"; });
      const bool promote =
          realCount >= 2 && std::ranges::any_of(binRows, [](const CheatsheetRow* r) { return !r->spawnArgs.empty(); });

      if (promote) {
        // Capitalize first letter for the group title.
        std::string groupTitle = bin;
        if (!groupTitle.empty()) {
          groupTitle[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(groupTitle[0])));
        }
        deferred.push_back({.title = std::move(groupTitle), .rows = std::move(binRows)});
      } else {
        // Flat under "Apps": combine binary + args.
        for (const auto* row : binRows) {
          std::string label = row->action;
          if (label != "\xe2\x80\xb3") {
            label = row->spawnArgs.empty() ? row->spawnBinary : row->spawnBinary + " " + row->spawnArgs;
            if (label.size() > 32) {
              label = label.substr(0, 32) + "\xe2\x80\xa6";
            }
            if (!row->submapAfter.empty()) {
              label += " \xe2\x86\x92 " + row->submapAfter;
            }
          }
          lines.push_back(bindLine(row->chord, label, groupId, maxChordLen, palette));
        }
      }
    }

    // Render deferred binary groups as top-level sections.
    for (auto& dg : deferred) {
      ++groupId;
      pushHeader(lines, groupId, dg.title, palette);
      for (const auto* row : dg.rows) {
        lines.push_back(bindLine(row->chord, row->action, groupId, maxChordLen, palette));
      }
    }
  }

  // Rows become Pango markup lines under their group headers. Lines sharing a group value are never split across
  // columns, so a header cannot be stranded at the foot of one.
  std::vector<DisplayLine> buildDisplayLines(
      const std::vector<umbriel::CheatsheetRow>& rows, const std::vector<std::string>& submapOrder,
      const CheatsheetPalette& palette
  ) {
    std::vector<DisplayLine> lines;
    int groupId = 0;

    for (Group grp : kFixedGroups) {
      std::vector<const CheatsheetRow*> groupRows;
      for (const auto& row : rows) {
        if (!row.submap.empty())
          continue;
        if (groupForAction(row.actionType) == grp) {
          groupRows.push_back(&row);
        }
      }
      if (groupRows.empty())
        continue;

      ++groupId;
      pushHeader(lines, groupId, groupTitle(grp), palette);

      // Find max chord width in this group (character count, for padding).
      size_t maxChordLen = 0;
      for (const auto* row : groupRows) {
        maxChordLen = std::max(maxChordLen, row->chord.size());
      }

      if (grp == Group::Apps) {
        pushAppsRows(lines, groupId, groupRows, maxChordLen, palette);
        continue;
      }

      // All other groups: flat row rendering.
      for (const auto* row : groupRows) {
        lines.push_back(bindLine(row->chord, row->action, groupId, maxChordLen, palette));
      }
    }

    // Submap groups.
    for (const auto& smName : submapOrder) {
      std::vector<const CheatsheetRow*> groupRows;
      for (const auto& row : rows) {
        if (row.submap == smName) {
          groupRows.push_back(&row);
        }
      }
      if (groupRows.empty())
        continue;

      ++groupId;
      pushHeader(lines, groupId, "Submap: " + smName, palette);

      size_t maxChordLen = 0;
      for (const auto* row : groupRows) {
        maxChordLen = std::max(maxChordLen, row->chord.size());
      }

      for (const auto* row : groupRows) {
        lines.push_back(bindLine(row->chord, row->action, groupId, maxChordLen, palette));
      }
    }

    return lines;
  }

  // Pack the lines into `numCols` columns of markup, breaking only between groups. A run of lines that has to stay
  // together: one group, plus the blank spacer that precedes it. A group broken across a column break reads as two
  // unrelated fragments, so these are the atoms and the only freedom in the layout is where the breaks between them go.
  struct Block {
    int begin = 0;
    int size = 0;
  };

  std::vector<Block> groupBlocks(const std::vector<DisplayLine>& lines) {
    std::vector<Block> blocks;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
      const auto index = static_cast<size_t>(i);
      if (blocks.empty() || lines[index].group != lines[index - 1].group) {
        blocks.push_back({.begin = i, .size = 0});
      }
      ++blocks.back().size;
    }
    return blocks;
  }

  std::vector<std::string> layoutColumns(const std::vector<DisplayLine>& allLines, int numCols) {
    const std::vector<Block> blocks = groupBlocks(allLines);
    if (blocks.empty()) {
      return {};
    }
    std::vector<int> blockSizes;
    blockSizes.reserve(blocks.size());
    for (const Block& block : blocks) {
      blockSizes.push_back(block.size);
    }
    const int limit = balancedColumnHeight(blockSizes, numCols);

    std::vector<std::string> columns;
    std::string markup;
    int used = 0;
    for (const Block& block : blocks) {
      if (used > 0 && used + block.size > limit) {
        columns.push_back(std::move(markup));
        markup.clear();
        used = 0;
      }
      for (int i = block.begin; i < block.begin + block.size; ++i) {
        const std::string& text = allLines[static_cast<size_t>(i)].text;
        // The spacer before a group is there to separate it from the group above. At the top of a column there is
        // nothing above, and keeping it would indent that column's first heading below its neighbours'.
        if (markup.empty() && text.empty()) {
          continue;
        }
        if (!markup.empty()) {
          markup += '\n';
        }
        markup += text;
      }
      used += block.size;
    }
    if (!markup.empty()) {
      columns.push_back(std::move(markup));
    }
    return columns;
  }

  // Lay the columns out and rasterise each, returning the buffers and the tallest.
  std::pair<std::vector<umbriel::TextBufferResult>, int>
  renderColumns(const std::vector<DisplayLine>& allLines, int cols, double scale, int fontSize) {
    auto colMarkups = layoutColumns(allLines, cols);
    const std::string font = std::format("monospace {}", fontSize);
    std::vector<TextBufferResult> buffers;
    int maxH = 0;
    for (auto& markup : colMarkups) {
      TextBufferResult buf = renderTextBuffer({
          .markup = std::move(markup),
          .font = font,
          .maxWidth = kColumnMaxWidth,
          .padding = 0,
          .scale = scale,
          .bgR = 0.0,
          .bgG = 0.0,
          .bgB = 0.0,
          .bgA = 0.0,
      });
      maxH = std::max(maxH, buf.logicalHeight);
      buffers.push_back(buf);
    }
    return {std::move(buffers), maxH};
  }

  struct Chrome {
    TextBufferResult title;
    TextBufferResult footer;
  };

  // The panel's fixed furniture: the title (which doubles as the "no config"
  // notice) and the footer naming the modifier key.
  Chrome renderChrome(bool configMissing, std::string_view modKeyName, double scale, const CheatsheetPalette& palette) {
    std::string titleMarkup =
        std::format("<span size='14pt' weight='bold' foreground='{}'>Umbriel keybinds</span>", palette.accentPrimary);
    if (configMissing) {
      titleMarkup += std::format(
          "\n<span foreground='{}'>no config found \xc2\xb7 showing built-in defaults</span>", palette.warning
      );
      titleMarkup += std::format(
          "\n<span foreground='{}'>copy examples/config.toml to ~/.config/umbriel/config.toml</span>", palette.textMuted
      );
    }
    const std::string footerMarkup = std::format(
        "<span foreground='{}'>Mod = {} \xc2\xb7 press any key to close</span>", palette.textMuted, modKeyName
    );

    const auto render = [scale](std::string markup) {
      return renderTextBuffer({
          .markup = std::move(markup),
          .font = "monospace 11",
          .maxWidth = 900,
          .padding = 0,
          .scale = scale,
          .bgR = 0.0,
          .bgG = 0.0,
          .bgB = 0.0,
          .bgA = 0.0,
      });
    };
    return {.title = render(std::move(titleMarkup)), .footer = render(footerMarkup)};
  }

  struct FittedBody {
    std::vector<TextBufferResult> columns;
    int maxColumnHeight = 0;
    int totalHeight = 0;
  };

  void dropBuffers(std::vector<umbriel::TextBufferResult>& buffers) {
    for (auto& buf : buffers) {
      if (buf.buffer != nullptr) {
        wlr_buffer_drop(buf.buffer);
        buf.buffer = nullptr;
      }
    }
  }

  // Rasterise the body, adding columns until the panel fits. The two bounds pull opposite ways: another column is
  // always shorter and always wider. The height bound is what makes the loop advance, the width bound is what stops it,
  // and the answer is the narrowest arrangement that clears both. Shrinking the font is the outer, later lever, because
  // it costs legibility everywhere while a column costs only width. Buffers from a rejected attempt are dropped rather
  // than leaked. Checking width at all is the point. This used to bound height only, and its remedy for an overflowing
  // panel was to add a column, which is the very thing that makes it too wide. On a 1080p screen at scale 1.25 that
  // walked out to four columns and 1862 logical pixels across an output 1536 wide, and the panel was then centred to a
  // negative x, putting the first column off the left edge. A loop rather than the single retry this replaces, which
  // gave up after one widening and returned the overflowing result anyway. It starts at one column instead of guessing
  // from a lines-per-column constant that knew nothing about the output: the guess cost a rasterisation when it was
  // wrong in either direction, and being wrong low was silently unrecoverable.
  int bodyWidth(const std::vector<umbriel::TextBufferResult>& columns) {
    int width = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
      width += columns[i].logicalWidth;
      if (i + 1 < columns.size()) {
        width += kColumnGap;
      }
    }
    return width;
  }

  FittedBody
  fitBody(const std::vector<DisplayLine>& allLines, double scale, int chromeHeight, int maxHeight, int maxBodyWidth) {
    FittedBody best;
    for (const int fontSize : kFontSizes) {
      for (int cols = 1; cols <= kMaxColumns; ++cols) {
        auto [buffers, maxColH] = renderColumns(allLines, cols, scale, fontSize);
        // Past the width bound, and every further column is wider still. The
        // only thing left to try is a smaller font.
        if (!best.columns.empty() && bodyWidth(buffers) > maxBodyWidth) {
          dropBuffers(buffers);
          break;
        }
        dropBuffers(best.columns);
        best = {.columns = std::move(buffers), .maxColumnHeight = maxColH, .totalHeight = chromeHeight + maxColH};
        if (best.totalHeight <= maxHeight) {
          return best;
        }
      }
    }
    // Nothing fit. `best` is the last thing tried: the smallest font at the
    // widest column count the output allows, which is the shortest of them.
    return best;
  }

} // namespace

namespace umbriel {

  Cheatsheet::Cheatsheet(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  Cheatsheet::~Cheatsheet() { hide(); }

  void Cheatsheet::show() {
    m_showWhenConfigReady = false;
    if (m_server.sessionLocked()) {
      return;
    }
    render();
  }

  void Cheatsheet::showOnStartup() {
    if (configHasMissingIncludes()) {
      m_showWhenConfigReady = true;
      return;
    }
    show();
  }

  void Cheatsheet::hide() {
    m_showWhenConfigReady = false;
    if (m_tree == nullptr) {
      return;
    }
    m_shadow.reset();
    wlr_scene_node_destroy(&m_tree->node);
    m_tree = nullptr;
  }

  void Cheatsheet::toggle() {
    if (m_tree != nullptr) {
      hide();
    } else {
      show();
    }
  }

  bool Cheatsheet::visible() const { return m_tree != nullptr; }

  void Cheatsheet::relayout() {
    if (m_showWhenConfigReady) {
      if (configHasMissingIncludes()) {
        return;
      }
      m_showWhenConfigReady = false;
      if (config().general.showCheatsheet && !m_server.sessionLocked()) {
        render();
      }
      return;
    }
    if (m_tree != nullptr) {
      render();
    }
  }

  void Cheatsheet::render() {
    // Destroy previous subtree.
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }

    m_tree = wlr_scene_tree_create(m_parent);

    // Read the preferred output's geometry.
    wlr_output* output = m_server.preferredOutput();
    double scale = 1.0;
    wlr_box outputBox{};
    bool haveOutput = false;
    if (output != nullptr) {
      scale = std::max(1.0, std::ceil(static_cast<double>(output->scale)));
      wlr_output_layout_get_box(m_server.outputLayout(), output, &outputBox);
      haveOutput = true;
    }

    std::vector<CheatsheetRow> rows = buildCheatsheetRows(config().keybinds);
    const CheatsheetPalette palette = makeCheatsheetPalette(config().colors);

    // Collect submaps in first-seen order.
    std::vector<std::string> submapOrder;
    for (const auto& row : rows) {
      if (!row.submap.empty()) {
        if (std::ranges::find(submapOrder, row.submap) == submapOrder.end()) {
          submapOrder.push_back(row.submap);
        }
      }
    }

    auto allLines = buildDisplayLines(rows, submapOrder, palette);

    // Provide a placeholder for an empty bind list.
    if (allLines.empty()) {
      allLines.push_back({
          .isHeader = false,
          .text = std::format("<span foreground='{}'>no keybinds configured</span>", palette.textMuted),
      });
    }

    Chrome chrome = renderChrome(configFileMissing(), m_server.modKeyName(), scale, palette);
    TextBufferResult& titleBuf = chrome.title;
    TextBufferResult& footerBuf = chrome.footer;

    // Everything the body has to share the panel with.
    const int chromeHeight =
        titleBuf.logicalHeight + kTitleBodyGap + kBodyFooterGap + footerBuf.logicalHeight + 2 * kPad;
    const int maxPanelHeight = haveOutput ? outputBox.height - 120 : std::numeric_limits<int>::max();
    // The body gets the output less the panel's own padding: what has to fit on
    // screen is the panel, not the text inside it.
    const int maxBodyWidth = haveOutput ? outputBox.width - 2 * kPad : std::numeric_limits<int>::max();
    FittedBody body = fitBody(allLines, scale, chromeHeight, maxPanelHeight, maxBodyWidth);
    auto& colBufs = body.columns;

    // Assemble the panel.
    int totalColW = 0;
    for (size_t i = 0; i < colBufs.size(); ++i) {
      totalColW += colBufs[i].logicalWidth;
      if (i + 1 < colBufs.size()) {
        totalColW += kColumnGap;
      }
    }

    int panelW = std::max({titleBuf.logicalWidth, footerBuf.logicalWidth, totalColW}) + 2 * kPad;
    int panelH = body.totalHeight;

    // Shadow and panel use the configured corner radius.
    const int cornerRadius = config().appearance.cornerRadius;
    m_shadow.update(m_tree, panelW, panelH, 0, cornerRadius);

    // Panel rect.
    float panelColor[4]{};
    premultiplied(panelColor, config().colors.background, 1.0F);
    wlr_scene_rect* panelRect = wlr_scene_rect_create(m_tree, panelW, panelH, panelColor);
    wlr_scene_rect_set_corner_radius(panelRect, cornerRadius);
    (void)panelRect;

    // Helper to add a text buffer to the scene tree.
    auto addBuffer = [this](TextBufferResult& result, int x, int y) {
      if (result.buffer == nullptr)
        return;
      wlr_scene_buffer* sceneBuf = wlr_scene_buffer_create(m_tree, result.buffer);
      wlr_buffer_drop(result.buffer);
      result.buffer = nullptr;
      if (sceneBuf == nullptr)
        return;
      wlr_scene_buffer_set_dest_size(sceneBuf, result.logicalWidth, result.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
      wlr_scene_node_set_position(&sceneBuf->node, x, y);
    };

    // Title.
    int curY = kPad;
    addBuffer(titleBuf, kPad, curY);
    curY += titleBuf.logicalHeight + kTitleBodyGap;

    // Columns.
    int colX = kPad;
    for (auto& buf : colBufs) {
      addBuffer(buf, colX, curY);
      colX += buf.logicalWidth + kColumnGap;
    }
    curY += body.maxColumnHeight + kBodyFooterGap;

    // Footer.
    addBuffer(footerBuf, kPad, curY);

    // Position: centered on preferred output.
    if (haveOutput) {
      // Clamped, not just centred. A panel taller or wider than the output centres to a negative offset, which pushes
      // the title off the top edge and the footer off the bottom with no way to reach either. Pinning the top-left
      // corner instead keeps the beginning of the list readable, which is the part worth keeping when something has to
      // be lost.
      const int x = outputBox.x + std::max(0, (outputBox.width - panelW) / 2);
      const int y = outputBox.y + std::max(0, (outputBox.height - panelH) / 2);
      wlr_scene_node_set_position(&m_tree->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_tree->node, 24, 24);
    }
  }

} // namespace umbriel
