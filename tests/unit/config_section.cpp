#include "check.h"
#include "config/section.h"

#include <string>

using umbriel::ConfigDiagnostic;
using umbriel::readSection;
using umbriel::Section;

namespace {

  // Everything a reader reported, joined so a test can assert on it in one go.
  std::string messages(const std::vector<ConfigDiagnostic>& diagnostics) {
    std::string out;
    for (const auto& diag : diagnostics) {
      out += diag.message;
      out += '\n';
    }
    return out;
  }

  bool contains(const std::vector<ConfigDiagnostic>& diagnostics, std::string_view needle) {
    return messages(diagnostics).contains(needle);
  }

} // namespace

UMBRIEL_TEST(readsEveryScalarKind) {
  const auto table = toml::parse(R"(
    count = 7
    ratio = 0.5
    name = "hello"
    flag = true
    tint = "#ff0000"
  )");

  std::vector<ConfigDiagnostic> diagnostics;
  int count = 0;
  double ratio = 0.0;
  std::string name;
  bool flag = false;
  std::array<float, 4> tint{};
  {
    Section s(table, "demo", diagnostics);
    s.integer("count", 0, 100, count)
        .real("ratio", 0.0, 1.0, ratio)
        .text("name", name)
        .boolean("flag", flag)
        .color("tint", tint);
  }

  CHECK_EQ(count, 7);
  CHECK(ratio == 0.5);
  CHECK(name == "hello");
  CHECK(flag);
  CHECK(tint[0] == 1.0F);
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(absentKeysLeaveTargetsAlone) {
  const auto table = toml::parse("other = 1");
  std::vector<ConfigDiagnostic> diagnostics;
  int count = 42;
  {
    Section s(table, "demo", diagnostics);
    s.integer("count", 0, 100, count).custom("other");
  }
  // A key the user did not write must not reset the default.
  CHECK_EQ(count, 42);
}

UMBRIEL_TEST(unknownKeysAreReportedWithoutAParallelList) {
  // The whole point: nothing here declares the vocabulary. Asking for "count" is
  // what makes "count" known, so the two cannot drift apart.
  const auto table = toml::parse(R"(
    count = 1
    typo = 2
  )");
  std::vector<ConfigDiagnostic> diagnostics;
  int count = 0;
  {
    Section s(table, "demo", diagnostics);
    s.integer("count", 0, 100, count);
  }
  CHECK(contains(diagnostics, "unknown key demo.typo"));
  CHECK(!contains(diagnostics, "demo.count"));
}

UMBRIEL_TEST(emptySectionNameReportsTopLevelKeysWithoutLeadingDot) {
  const auto table = toml::parse("typo = 2");
  std::vector<ConfigDiagnostic> diagnostics;
  {
    Section root(table, "", diagnostics);
  }
  CHECK(contains(diagnostics, "unknown key typo"));
  CHECK(!contains(diagnostics, "unknown key .typo"));
}

UMBRIEL_TEST(unknownKeysAreReportedEvenWhenTheReaderStopsEarly) {
  // Emitted from the destructor, so a reader that returns before asking for
  // everything still reports what it never claimed.
  const auto table = toml::parse("typo = 1");
  std::vector<ConfigDiagnostic> diagnostics;
  {
    Section s(table, "demo", diagnostics);
    // Reader bails immediately.
  }
  CHECK(contains(diagnostics, "unknown key demo.typo"));
}

UMBRIEL_TEST(customClaimsAKeyWithoutReadingIt) {
  const auto table = toml::parse("binds = [1, 2]");
  std::vector<ConfigDiagnostic> diagnostics;
  {
    Section s(table, "demo", diagnostics);
    s.custom("binds");
  }
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(freeformSuppressesTheWholeReport) {
  // Sections whose keys are user-chosen names, not a fixed vocabulary.
  const auto table = toml::parse(R"(
    ANYTHING = "1"
    else_entirely = "2"
  )");
  std::vector<ConfigDiagnostic> diagnostics;
  {
    Section s(table, "env", diagnostics);
    s.freeform();
  }
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(wrongTypesAreReportedAndIgnored) {
  const auto table = toml::parse(R"(
    count = "not a number"
    ratio = "nope"
    name = 5
    flag = 1
    tint = 7
  )");
  std::vector<ConfigDiagnostic> diagnostics;
  int count = 3;
  double ratio = 0.25;
  std::string name = "keep";
  bool flag = true;
  std::array<float, 4> tint{0.5F, 0.5F, 0.5F, 0.5F};
  {
    Section s(table, "demo", diagnostics);
    s.integer("count", 0, 100, count)
        .real("ratio", 0.0, 1.0, ratio)
        .text("name", name)
        .boolean("flag", flag)
        .color("tint", tint);
  }

  // Every target keeps its previous value.
  CHECK_EQ(count, 3);
  CHECK(ratio == 0.25);
  CHECK(name == "keep");
  CHECK(flag);
  CHECK(tint[0] == 0.5F);

  CHECK(contains(diagnostics, "ignoring demo.count (expected integer)"));
  CHECK(contains(diagnostics, "ignoring demo.ratio (expected number)"));
  CHECK(contains(diagnostics, "ignoring demo.name (expected string)"));
  CHECK(contains(diagnostics, "ignoring demo.flag (expected boolean)"));
  CHECK(contains(diagnostics, "ignoring demo.tint (expected color string)"));
}

UMBRIEL_TEST(anIntegerIsNotABoolean) {
  // TOML has a real boolean type; `flag = 1` is a mistake worth reporting rather
  // than silently coercing.
  const auto table = toml::parse("flag = 1");
  std::vector<ConfigDiagnostic> diagnostics;
  bool flag = false;
  {
    Section s(table, "demo", diagnostics);
    s.boolean("flag", flag);
  }
  CHECK(!flag);
  CHECK(contains(diagnostics, "expected boolean"));
}

UMBRIEL_TEST(outOfRangeValuesClampAndSaySo) {
  const auto table = toml::parse(R"(
    high = 500
    low = -10
    ratio = 9.5
  )");
  std::vector<ConfigDiagnostic> diagnostics;
  int high = 0;
  int low = 0;
  double ratio = 0.0;
  {
    Section s(table, "demo", diagnostics);
    s.integer("high", 0, 100, high).integer("low", 0, 100, low).real("ratio", 0.0, 1.0, ratio);
  }
  CHECK_EQ(high, 100);
  CHECK_EQ(low, 0);
  CHECK(ratio == 1.0);
  // Silently clamping is how a user concludes a setting does nothing.
  CHECK(contains(diagnostics, "demo.high = 500 out of range, clamped to 100"));
  CHECK(contains(diagnostics, "demo.low = -10 out of range, clamped to 0"));
}

UMBRIEL_TEST(inRangeValuesDoNotWarn) {
  const auto table = toml::parse("edge = 100");
  std::vector<ConfigDiagnostic> diagnostics;
  int edge = 0;
  {
    Section s(table, "demo", diagnostics);
    s.integer("edge", 0, 100, edge);
  }
  // Inclusive bounds: exactly the maximum is fine.
  CHECK_EQ(edge, 100);
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(notANumberIsRejectedRatherThanClamped) {
  // std::clamp on a NaN would propagate it into the config.
  const auto table = toml::parse("ratio = nan");
  std::vector<ConfigDiagnostic> diagnostics;
  double ratio = 0.25;
  {
    Section s(table, "demo", diagnostics);
    s.real("ratio", 0.0, 1.0, ratio);
  }
  CHECK(ratio == 0.25);
  CHECK(contains(diagnostics, "expected number"));
}

UMBRIEL_TEST(invalidColorsAreReportedWithTheOffendingText) {
  const auto table = toml::parse(R"(tint = "octarine")");
  std::vector<ConfigDiagnostic> diagnostics;
  std::array<float, 4> tint{};
  {
    Section s(table, "demo", diagnostics);
    s.color("tint", tint);
  }
  CHECK(contains(diagnostics, "invalid color 'octarine'"));
}

UMBRIEL_TEST(optionalTargetsStayUnsetWhenTheKeyIsAbsent) {
  const auto table = toml::parse("present = 5");
  std::vector<ConfigDiagnostic> diagnostics;
  std::optional<int> present;
  std::optional<int> absent;
  std::optional<double> absentReal;
  std::optional<bool> absentFlag;
  {
    Section s(table, "demo", diagnostics);
    s.integer("present", 0, 10, present)
        .integer("absent", 0, 10, absent)
        .real("absent_real", 0.0, 1.0, absentReal)
        .boolean("absent_flag", absentFlag);
  }
  CHECK(present.has_value());
  CHECK_EQ(*present, 5);
  // "unset" and "set to the default" are different states for a rule override.
  CHECK(!absent.has_value());
  CHECK(!absentReal.has_value());
  CHECK(!absentFlag.has_value());
}

UMBRIEL_TEST(nestedTablesReportUnderTheirFullPath) {
  const auto table = toml::parse(R"(
    [inner]
    depth = 2
    typo = 3
  )");
  std::vector<ConfigDiagnostic> diagnostics;
  int depth = 0;
  {
    Section s(table, "outer", diagnostics);
    s.sub("inner", [&](Section& inner) { inner.integer("depth", 0, 10, depth); });
  }
  CHECK_EQ(depth, 2);
  CHECK(contains(diagnostics, "unknown key outer.inner.typo"));
}

UMBRIEL_TEST(aNestedKeyThatIsNotATableIsReportedNotDescendedInto) {
  const auto table = toml::parse("inner = 5");
  std::vector<ConfigDiagnostic> diagnostics;
  bool entered = false;
  {
    Section s(table, "outer", diagnostics);
    s.sub("inner", [&](Section&) { entered = true; });
  }
  CHECK(!entered);
  CHECK(contains(diagnostics, "ignoring outer.inner (expected table)"));
}

UMBRIEL_TEST(anAbsentNestedTableIsNotAnError) {
  const auto table = toml::parse("");
  std::vector<ConfigDiagnostic> diagnostics;
  bool entered = false;
  {
    Section s(table, "outer", diagnostics);
    s.sub("inner", [&](Section&) { entered = true; });
  }
  CHECK(!entered);
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(readSectionSkipsAnAbsentSection) {
  const auto table = toml::parse("other = 1");
  std::vector<ConfigDiagnostic> diagnostics;
  CHECK(!readSection(table, "demo", diagnostics, [](Section&) {}));
  CHECK_EQ(static_cast<int>(diagnostics.size()), 0);
}

UMBRIEL_TEST(readSectionReportsANonTableSection) {
  const auto table = toml::parse(R"(demo = "oops")");
  std::vector<ConfigDiagnostic> diagnostics;
  CHECK(!readSection(table, "demo", diagnostics, [](Section&) {}));
  CHECK(contains(diagnostics, "ignoring demo (expected table)"));
}

UMBRIEL_TEST(diagnosticsCarryTheSourcePositionOfTheOffendingKey) {
  const auto table = toml::parse("\n\ntypo = 1\n");
  std::vector<ConfigDiagnostic> diagnostics;
  {
    Section s(table, "demo", diagnostics);
  }
  CHECK_EQ(static_cast<int>(diagnostics.size()), 1);
  // Without a position the user has to hunt for the key themselves.
  CHECK_EQ(static_cast<int>(diagnostics[0].line), 3);
}

// The schema recorder only observes: a table read with and without one yields the same values and the same reports.
UMBRIEL_TEST(recordingLeavesParsingUnchanged) {
  const auto table = toml::parse(R"(
    count = 700
    ratio = "half"
    tint = "#00ff0080"
    names = ["a", ""]
    stray = 1
    [nested]
    flag = true
  )");

  struct Result {
    int count = 3;
    double ratio = 0.25;
    std::array<float, 4> tint{};
    std::vector<std::string> names;
    bool flag = false;
    std::string messages;
  };
  const auto read = [&table] {
    Result result;
    std::vector<ConfigDiagnostic> diagnostics;
    {
      Section s(table, "demo", diagnostics);
      s.integer("count", 0, 100, result.count)
          .real("ratio", 0.0, 1.0, result.ratio)
          .color("tint", result.tint)
          .strings("names", result.names)
          .sub("nested", [&](Section& nested) { nested.boolean("flag", result.flag); });
    }
    result.messages = messages(diagnostics);
    return result;
  };

  const Result plain = read();
  umbriel::SchemaRecorder recorder;
  Result recorded;
  {
    const umbriel::SchemaRecording recording(recorder);
    recorded = read();
  }

  CHECK_EQ(recorded.count, plain.count);
  CHECK(recorded.ratio == plain.ratio);
  CHECK(recorded.tint == plain.tint);
  CHECK(recorded.names == plain.names);
  CHECK(recorded.flag == plain.flag);
  CHECK(recorded.messages == plain.messages);
  CHECK(plain.messages.contains("unknown key demo.stray"));
  // A default is the target as the reader found it, before the table changed it.
  const auto& count = recorder.entries().at("demo.count");
  CHECK(count.type == umbriel::SchemaType::Int);
  CHECK(count.defaultValue == umbriel::SchemaValue{std::int64_t{3}});
  CHECK(recorder.entries().contains("demo.nested.flag"));
  CHECK(umbriel::SchemaRecorder::active() == nullptr);
}

int main() { return RUN_TESTS(); }
