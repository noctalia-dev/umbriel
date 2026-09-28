#pragma once

#include <string>
#include <string_view>

namespace umbriel {

  // How a physical output identifies itself, as wlroots reports it on
  // wlr_output. Every field except the connector may be absent: make, model and
  // serial come from EDID, which not all displays fill in.
  struct OutputIdentity {
    std::string_view connector;
    std::string_view make;
    std::string_view model;
    std::string_view serial;
  };

  // "<make> <model> <serial>", with "Unknown" for any field the display does not report. Names an output by panel
  // rather than port; shown by `umbriel outputs`.
  [[nodiscard]] std::string outputDescriptor(const OutputIdentity& identity);

  enum class OutputNameMatch {
    None,
    Connector,
    Descriptor,
  };

  [[nodiscard]] bool outputNamesEqual(std::string_view left, std::string_view right);

  // Virtual output names use ASCII letters, digits, '-', '_' and '.', so they fit the "<output>:<n>" workspace IDs
  // and "<workspace>/<output>" action targets.
  [[nodiscard]] bool validVirtualOutputName(std::string_view name);

  // Return which configured output-name form refers to this identity.
  //
  // Connectors and descriptors are compared case-insensitively over ASCII. A
  // display reporting no make, model or serial can only be named by its
  // connector. The descriptor form is not attempted for it because every such
  // output would otherwise answer to "Unknown Unknown Unknown".
  [[nodiscard]] OutputNameMatch outputNameMatch(const OutputIdentity& identity, std::string_view configured);

} // namespace umbriel
