#include "config/schema_recorder.h"

#include <algorithm>
#include <cassert>
#include <cctype>

namespace umbriel {

  namespace {

    SchemaRecorder*& activeRecorder() {
      static SchemaRecorder* recorder = nullptr;
      return recorder;
    }

    // `name[3]` names one entry of an array of tables; the schema describes them all at once.
    std::string stripIndices(std::string_view path) {
      std::string out;
      out.reserve(path.size());
      for (size_t index = 0; index < path.size(); ++index) {
        if (path[index] == '[') {
          size_t close = index + 1;
          while (close < path.size() && std::isdigit(static_cast<unsigned char>(path[close])) != 0) {
            ++close;
          }
          if (close < path.size() && path[close] == ']') {
            out += "[]";
            index = close;
            continue;
          }
        }
        out += path[index];
      }
      return out;
    }

    bool hasSegmentPrefix(std::string_view path, std::string_view prefix) {
      return path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '.');
    }

  } // namespace

  std::string_view schemaTypeName(SchemaType type) {
    switch (type) {
    case SchemaType::Unknown:
      return "unknown";
    case SchemaType::Bool:
      return "bool";
    case SchemaType::Int:
      return "int";
    case SchemaType::Float:
      return "float";
    case SchemaType::String:
      return "string";
    case SchemaType::Color:
      return "color";
    case SchemaType::Enum:
      return "enum";
    case SchemaType::StringArray:
      return "string_array";
    case SchemaType::FloatArray:
      return "float_array";
    case SchemaType::IntArray:
      return "int_array";
    case SchemaType::EnumOrArray:
      return "enum_or_array";
    case SchemaType::IntOrString:
      return "int_or_string";
    case SchemaType::IntOrStringArray:
      return "int_or_string_array";
    case SchemaType::FloatOrTable:
      return "float_or_table";
    case SchemaType::StringOrTable:
      return "string_or_table";
    case SchemaType::Table:
      return "table";
    case SchemaType::ArrayOfTables:
      return "array_of_tables";
    case SchemaType::Map:
      return "map";
    }
    return "unknown";
  }

  SchemaRecorder* SchemaRecorder::active() { return activeRecorder(); }

  std::string SchemaRecorder::normalize(std::string_view path) const {
    std::string out = stripIndices(path);
    // Rule arrays name each entry's Section after the array itself, without an index.
    for (const std::string& array : m_arrays) {
      if (hasSegmentPrefix(out, array) && out.size() > array.size()) {
        out = array + "[]" + out.substr(array.size());
      }
    }
    for (const auto& [map, sample] : m_maps) {
      const std::string prefix = map + "." + sample;
      if (hasSegmentPrefix(out, prefix)) {
        out = map + ".<name>" + out.substr(prefix.size());
      }
    }
    return out;
  }

  void SchemaRecorder::record(std::string_view path, KeySpec spec) {
    std::string normalized = normalize(path);
    // A key claimed twice keeps the more specific report: a typed reader's over a bare claim.
    const auto existing = m_entries.find(normalized);
    if (existing != m_entries.end() && spec.type == SchemaType::Unknown) {
      return;
    }
    m_entries.insert_or_assign(std::move(normalized), std::move(spec));
  }

  void SchemaRecorder::markArray(std::string path) {
    if (std::ranges::find(m_arrays, path) == m_arrays.end()) {
      m_arrays.push_back(std::move(path));
    }
  }

  void SchemaRecorder::markMap(std::string path, std::string sampleKey) {
    if (std::ranges::none_of(m_maps, [&](const auto& map) { return map.first == path; })) {
      m_maps.emplace_back(std::move(path), std::move(sampleKey));
    }
  }

  SchemaRecording::SchemaRecording(SchemaRecorder& recorder) {
    assert(activeRecorder() == nullptr && "schema recordings do not nest");
    activeRecorder() = &recorder;
  }

  SchemaRecording::~SchemaRecording() { activeRecorder() = nullptr; }

} // namespace umbriel
