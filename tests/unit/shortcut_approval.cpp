#include "scene/shortcut_approval.h"

#include "check.h"

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <glib.h>
#include <limits>
#include <pango/pango.h>
#include <set>
#include <utility>

using namespace umbriel;

namespace {
  GlobalShortcutRegistration shortcut(std::string id = "talk", std::string description = "Push to talk") {
    GlobalShortcutRegistration result;
    result.id = std::move(id);
    result.description = std::move(description);
    result.candidate = parseGlobalShortcutTrigger("CTRL+m");
    result.conflict = GlobalShortcutConflict::None;
    return result;
  }

  bool verify(const ShortcutApprovalLayout& layout) {
    CHECK(!layout.pages.empty());
    if (layout.pages.empty()) {
      return false;
    }
    size_t next = 0;
    size_t finals = 0;
    std::string joined;
    for (size_t page = 0; page < layout.pages.size(); ++page) {
      const auto& range = layout.pages[page];
      CHECK_EQ(range.begin, next);
      CHECK(range.end > range.begin && range.end <= layout.content.size());
      if (range.begin != next || range.end <= range.begin || range.end > layout.content.size()) {
        return false;
      }
      const auto chunk = layout.content.substr(range.begin, range.end - range.begin);
      CHECK(g_utf8_validate(chunk.data(), static_cast<gssize>(chunk.size()), nullptr) != 0);
      joined += chunk;
      next = range.end;
      finals += range.final ? 1 : 0;
      CHECK_EQ(range.final, page + 1 == layout.pages.size());
      const auto params = layout.pageParams(page);
      CHECK(params.fixedWidth);
      const auto measured = measureTextBuffer(params);
      CHECK(measured.logicalWidth <= layout.width);
      CHECK(measured.logicalHeight <= layout.height);
      gchar* plain = nullptr;
      PangoAttrList* attributes = nullptr;
      CHECK(pango_parse_markup(params.markup.c_str(), -1, 0, &attributes, &plain, nullptr, nullptr) != 0);
      if (plain != nullptr) {
        CHECK_EQ(std::string(plain), layout.pageText(page));
        g_free(plain);
      }
      if (attributes != nullptr) {
        pango_attr_list_unref(attributes);
      }
    }
    CHECK_EQ(finals, size_t{1});
    CHECK_EQ(joined, layout.content);
    CHECK_EQ(next, layout.content.size());
    return true;
  }
} // namespace

UMBRIEL_TEST(onePageShowsApplicationMetadataAndTheExactCandidate) {
  const auto entry = shortcut();
  const auto layout = layoutShortcutApproval("org.test.App", std::array{entry}, 640, 480);
  CHECK(layout.has_value());
  if (!layout) {
    return;
  }
  CHECK(verify(*layout));
  CHECK_EQ(layout->pages.size(), size_t{1});
  CHECK(layout->header.contains("org.test.App"));
  CHECK(layout->content.contains(entry.id));
  CHECK(layout->content.contains(entry.description));
  CHECK(layout->content.contains(describeGlobalShortcutTrigger(*entry.candidate)));
  CHECK(layout->pages.front().final);
}

UMBRIEL_TEST(maximumBatchesCoverEveryEntryAndEveryWrappedDescription) {
  std::vector<GlobalShortcutRegistration> entries;
  entries.reserve(kMaxGlobalShortcuts);
  for (size_t index = 0; index < kMaxGlobalShortcuts; ++index) {
    entries.push_back(
        shortcut(std::format("action-{:03}", index), std::string(1024, static_cast<char>('a' + index % 26)))
    );
  }
  const auto layout = layoutShortcutApproval("org.test.App", entries, 640, 720);
  CHECK(layout.has_value());
  if (!layout) {
    return;
  }
  CHECK(verify(*layout));
  CHECK(layout->pages.size() > 1);
  for (const auto& entry : entries) {
    CHECK(layout->content.contains(entry.id));
    CHECK_EQ(layout->content.find(entry.id), layout->content.rfind(entry.id));
    CHECK(layout->content.contains(entry.description));
  }
}

UMBRIEL_TEST(graphemeClustersStayTogetherAcrossPageBreaks) {
  std::string description;
  for (int index = 0; index < 150; ++index) {
    description += "e\u0301\u0327";
  }
  const auto layout = layoutShortcutApproval("org.test.App", std::array{shortcut("unicode", description)}, 320, 400);
  CHECK(layout.has_value());
  if (!layout) {
    return;
  }
  CHECK(verify(*layout));
  CHECK(layout->pages.size() > 1);
  CHECK(layout->content.contains(description));
  for (const auto& page : layout->pages) {
    const auto first = g_utf8_get_char(layout->content.data() + page.begin);
    CHECK_EQ(g_unichar_combining_class(first), 0);
    if (page.end < layout->content.size()) {
      const auto following = g_utf8_get_char(layout->content.data() + page.end);
      CHECK_EQ(g_unichar_combining_class(following), 0);
    }
  }
}

UMBRIEL_TEST(untrustedTextCannotInjectMarkupOrInvisibleReviewControls) {
  const std::string application = "org.test.<b>App</b>\n\u202e";
  const std::string description = "<span size='99pt'>音声</span>\t\u2066safe\u2069";
  const auto layout = layoutShortcutApproval(application, std::array{shortcut("<&>", description)}, 640, 640);
  CHECK(layout.has_value());
  if (!layout) {
    return;
  }
  CHECK(verify(*layout));
  CHECK(layout->header.contains("<b>App</b>"));
  CHECK(!layout->header.contains("\u202e"));
  CHECK(!layout->content.contains('\t'));
  CHECK(!layout->content.contains("\u2066"));
  CHECK(!layout->content.contains("\u2069"));
  CHECK(layout->content.contains("音声"));
  for (size_t page = 0; page < layout->pages.size(); ++page) {
    const auto params = layout->pageParams(page);
    PangoAttrList* attributes = nullptr;
    CHECK(pango_parse_markup(params.markup.c_str(), -1, 0, &attributes, nullptr, nullptr, nullptr) != 0);
    if (attributes != nullptr) {
      GSList* parsed = pango_attr_list_get_attributes(attributes);
      CHECK(parsed == nullptr);
      g_slist_free_full(parsed, [](gpointer attribute) {
        pango_attribute_destroy(static_cast<PangoAttribute*>(attribute));
      });
      pango_attr_list_unref(attributes);
    }
  }
}

UMBRIEL_TEST(resizingReflowsTheCompleteContentWithinTheNewBounds) {
  std::vector<GlobalShortcutRegistration> entries;
  entries.reserve(8);
  for (int index = 0; index < 8; ++index) {
    entries.push_back(shortcut(std::format("action-{}", index), std::string(300, 'x')));
  }
  const auto wide = layoutShortcutApproval("org.test.App", entries, 1200, 1200);
  const auto narrow = layoutShortcutApproval("org.test.App", entries, 360, 420, 2.0);
  CHECK(wide && narrow);
  if (!wide || !narrow) {
    return;
  }
  CHECK(verify(*wide));
  CHECK(verify(*narrow));
  CHECK(wide->width <= 1200 && wide->height <= 1200);
  CHECK(narrow->width <= 360 && narrow->height <= 420);
  CHECK(narrow->pages.size() > wide->pages.size());
  CHECK_EQ(narrow->content, wide->content);
  CHECK_EQ(narrow->pages.front().begin, size_t{0});
}

UMBRIEL_TEST(insufficientGeometryAndInvalidInputFailWithoutPartialPages) {
  const std::array entries{shortcut()};
  CHECK(!layoutShortcutApproval("org.test.App", entries, 0, 720));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, -1));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, 80));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 80, 720));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, 480, 0.0));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, 480, std::numeric_limits<double>::infinity()));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, 480, std::numeric_limits<double>::quiet_NaN()));
  CHECK(!layoutShortcutApproval("org.test.App", entries, 640, 480, std::numeric_limits<double>::max()));
  CHECK(!layoutShortcutApproval("", entries, 640, 480));
  CHECK(!layoutShortcutApproval("org.test.App", {}, 640, 480));
  CHECK(!layoutShortcutApproval("org.test.App", std::array{shortcut("bad", std::string(1, '\xff'))}, 640, 480));
}

UMBRIEL_TEST(conflictReasonsAndUnassignedCandidatesRemainVisible) {
  std::set<std::string> assigned;
  for (const auto conflict :
       {GlobalShortcutConflict::None, GlobalShortcutConflict::Reserved, GlobalShortcutConflict::InUse}) {
    auto entry = shortcut();
    entry.conflict = conflict;
    const auto layout = layoutShortcutApproval("org.test.App", std::array{entry}, 640, 480);
    CHECK(layout.has_value());
    if (layout) {
      CHECK(layout->content.contains(describeGlobalShortcutTrigger(*entry.candidate)));
      assigned.insert(layout->content);
    }
  }
  CHECK_EQ(assigned.size(), size_t{3});
  std::set<std::string> unassigned;
  for (const auto conflict : {GlobalShortcutConflict::Unassigned, GlobalShortcutConflict::Disabled}) {
    auto entry = shortcut();
    entry.candidate.reset();
    entry.conflict = conflict;
    const auto layout = layoutShortcutApproval("org.test.App", std::array{entry}, 640, 480);
    CHECK(layout.has_value());
    if (layout) {
      CHECK(layout->content.contains(entry.id));
      unassigned.insert(layout->content);
    }
  }
  CHECK_EQ(unassigned.size(), size_t{2});
}

UMBRIEL_TEST(realTextBuffersMatchMeasuredApprovalBoundsAtEachScale) {
  std::string description(800, 'w');
  for (const double scale : {1.0, 1.25, 2.0}) {
    const auto layout =
        layoutShortcutApproval("org.test.App", std::array{shortcut("talk", description)}, 420, 420, scale);
    CHECK(layout.has_value());
    if (!layout) {
      continue;
    }
    for (size_t page = 0; page < layout->pages.size(); ++page) {
      const auto params = layout->pageParams(page);
      const auto measured = measureTextBuffer(params);
      const auto rendered = renderTextBuffer(params);
      CHECK(rendered.buffer != nullptr);
      CHECK_EQ(rendered.logicalWidth, measured.logicalWidth);
      CHECK_EQ(rendered.logicalHeight, measured.logicalHeight);
      CHECK(rendered.logicalWidth <= 420 && rendered.logicalHeight <= 420);
      if (rendered.buffer != nullptr) {
        CHECK_EQ(rendered.buffer->width, static_cast<int>(std::ceil(rendered.logicalWidth * scale)));
        CHECK_EQ(rendered.buffer->height, static_cast<int>(std::ceil(rendered.logicalHeight * scale)));
        wlr_buffer_drop(rendered.buffer);
      }
    }
  }
}

UMBRIEL_TEST(existingTextStillShrinksWrapsAndEllipsizesWhileFixedPagesKeepTheirWidth) {
  TextBufferParams params{.markup = "Short text", .maxWidth = 180, .padding = 8};
  const auto natural = renderTextBuffer(params);
  CHECK(natural.logicalWidth < params.maxWidth + 2 * params.padding);
  params.fixedWidth = true;
  const auto fixed = renderTextBuffer(params);
  CHECK_EQ(fixed.logicalWidth, params.maxWidth + 2 * params.padding);
  params.fixedWidth = false;
  params.markup = std::string(1000, 'w');
  const auto wrapped = renderTextBuffer(params);
  CHECK(wrapped.logicalHeight > natural.logicalHeight);
  CHECK(wrapped.logicalWidth <= params.maxWidth + 2 * params.padding);
  params.ellipsize = true;
  const auto ellipsized = renderTextBuffer(params);
  CHECK_EQ(ellipsized.logicalHeight, natural.logicalHeight);
  CHECK(ellipsized.logicalWidth <= params.maxWidth + 2 * params.padding);
  for (auto* buffer : {natural.buffer, fixed.buffer, wrapped.buffer, ellipsized.buffer}) {
    CHECK(buffer != nullptr);
    if (buffer != nullptr) {
      wlr_buffer_drop(buffer);
    }
  }
}

int main() { return RUN_TESTS(); }
