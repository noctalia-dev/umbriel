#include "output/identity.h"

#include <algorithm>

namespace umbriel {

  namespace {
    constexpr std::string_view kUnknown = "Unknown";

    char lowerAscii(char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; }
    std::string_view orUnknown(std::string_view value) { return value.empty() ? kUnknown : value; }

    bool descriptorEquals(const OutputIdentity& identity, std::string_view configured) {
      const std::string_view make = orUnknown(identity.make);
      const std::string_view model = orUnknown(identity.model);
      const std::string_view serial = orUnknown(identity.serial);
      if (configured.size() != make.size() + model.size() + serial.size() + 2) {
        return false;
      }
      const size_t modelOffset = make.size() + 1;
      const size_t serialOffset = modelOffset + model.size() + 1;
      return configured[make.size()] == ' '
          && configured[serialOffset - 1] == ' '
          && outputNamesEqual(configured.substr(0, make.size()), make)
          && outputNamesEqual(configured.substr(modelOffset, model.size()), model)
          && outputNamesEqual(configured.substr(serialOffset), serial);
    }

  } // namespace

  bool outputNamesEqual(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
      if (lowerAscii(left[index]) != lowerAscii(right[index])) {
        return false;
      }
    }
    return true;
  }

  bool validVirtualOutputName(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char value) {
      return (value >= 'a' && value <= 'z')
          || (value >= 'A' && value <= 'Z')
          || (value >= '0' && value <= '9')
          || value == '-'
          || value == '_'
          || value == '.';
    });
  }

  std::string outputDescriptor(const OutputIdentity& identity) {
    const std::string_view make = orUnknown(identity.make);
    const std::string_view model = orUnknown(identity.model);
    const std::string_view serial = orUnknown(identity.serial);
    std::string descriptor;
    descriptor.reserve(make.size() + model.size() + serial.size() + 2);
    descriptor.append(make);
    descriptor.push_back(' ');
    descriptor.append(model);
    descriptor.push_back(' ');
    descriptor.append(serial);
    return descriptor;
  }

  OutputNameMatch outputNameMatch(const OutputIdentity& identity, std::string_view configured) {
    if (!identity.connector.empty() && outputNamesEqual(configured, identity.connector)) {
      return OutputNameMatch::Connector;
    }
    if (identity.make.empty() && identity.model.empty() && identity.serial.empty()) {
      return OutputNameMatch::None;
    }
    return descriptorEquals(identity, configured) ? OutputNameMatch::Descriptor : OutputNameMatch::None;
  }

} // namespace umbriel
