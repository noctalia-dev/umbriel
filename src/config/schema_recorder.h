#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace umbriel {

  // The shape of a key's value, as `umbriel config schema` reports it. The compound shapes name keys whose readers
  // accept more than one TOML type, such as `default_workspace` taking an index or a name.
  enum class SchemaType : std::uint8_t {
    Unknown,
    Bool,
    Int,
    Float,
    String,
    Color,
    Enum,
    StringArray,
    FloatArray,
    IntArray,
    EnumOrArray,
    IntOrString,
    IntOrStringArray,
    FloatOrTable,
    StringOrTable,
    Table,
    ArrayOfTables,
    Map,
  };

  [[nodiscard]] std::string_view schemaTypeName(SchemaType type);

  using SchemaValue =
      std::variant<bool, std::int64_t, double, std::string, std::vector<double>, std::vector<std::string>>;

  // What one key accepts.
  struct KeySpec {
    explicit KeySpec(SchemaType schemaType = SchemaType::Unknown) : type(schemaType) {}

    [[nodiscard]] KeySpec withValues(std::vector<std::string> accepted) && {
      values = std::move(accepted);
      return std::move(*this);
    }
    [[nodiscard]] KeySpec withRange(double minimum, double maximum) && {
      min = minimum;
      max = maximum;
      return std::move(*this);
    }
    [[nodiscard]] KeySpec withFormat(std::string meaning) && {
      format = std::move(meaning);
      return std::move(*this);
    }
    [[nodiscard]] KeySpec withDefault(std::optional<SchemaValue> value) && {
      defaultValue = std::move(value);
      return std::move(*this);
    }

    SchemaType type = SchemaType::Unknown;
    // Accepted spellings, for enums and for the string half of a compound type.
    std::vector<std::string> values;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<SchemaValue> defaultValue;
    // What a string means beyond being a string: "action", "curve", "regex", "path", ...
    std::string format;
  };

  // Collects every key the config readers claim while `umbriel config schema` runs. Section reports to it through a
  // scoped global rather than a pointer passed in like its diagnostics sink: every Section, including those bespoke
  // readers build for their own nested tables, is then covered without touching a single reader, and it is only ever
  // active inside the schema command. Config loading is single-threaded, and recordings do not nest.
  class SchemaRecorder {
  public:
    // The recorder of the running SchemaRecording, or nullptr when parsing normally.
    [[nodiscard]] static SchemaRecorder* active();

    // `path` is a Section-qualified key, such as `window_rule.match.app_id` or `input.device[0].tap`.
    void record(std::string_view path, KeySpec spec);

    // Shapes the schema run has discovered, which decide how paths are written: an array's entries as `name[]`, a
    // map's sample entry as `name.<name>`.
    void markArray(std::string path);
    void markMap(std::string path, std::string sampleKey);

    // Keyed by schema path.
    [[nodiscard]] const std::map<std::string, KeySpec>& entries() const { return m_entries; }

  private:
    [[nodiscard]] std::string normalize(std::string_view path) const;

    std::map<std::string, KeySpec> m_entries;
    std::vector<std::string> m_arrays;
    std::vector<std::pair<std::string, std::string>> m_maps;
  };

  // Activates a recorder for its lifetime.
  class SchemaRecording {
  public:
    explicit SchemaRecording(SchemaRecorder& recorder);
    ~SchemaRecording();

    SchemaRecording(const SchemaRecording&) = delete;
    SchemaRecording& operator=(const SchemaRecording&) = delete;
    SchemaRecording(SchemaRecording&&) = delete;
    SchemaRecording& operator=(SchemaRecording&&) = delete;
  };

} // namespace umbriel
