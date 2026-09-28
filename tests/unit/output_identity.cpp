#include "check.h"
#include "output/identity.h"

#include <string>

using umbriel::outputDescriptor;
using umbriel::OutputIdentity;
using umbriel::OutputNameMatch;
using umbriel::outputNameMatch;
using umbriel::outputNamesEqual;
using umbriel::validVirtualOutputName;

namespace {
  // A display that fills in every EDID string.
  constexpr OutputIdentity kMonitor{
      .connector = "HDMI-A-1", .make = "Microstep", .model = "MSI G2712F", .serial = "CD6T084401192"
  };
  // A laptop panel: make and model, no serial. Common.
  constexpr OutputIdentity kPanel{
      .connector = "eDP-1", .make = "Najing CEC Panda FPD Technology CO. ltd", .model = "0x0036", .serial = ""
  };
  // Nothing but a connector: headless and nested outputs look like this.
  constexpr OutputIdentity kBare{.connector = "HEADLESS-1", .make = "", .model = "", .serial = ""};
  bool matches(const OutputIdentity& identity, std::string_view configured) {
    return outputNameMatch(identity, configured) != OutputNameMatch::None;
  }
} // namespace

UMBRIEL_TEST(connectorStillMatches) {
  CHECK(matches(kMonitor, "HDMI-A-1"));
  CHECK(matches(kPanel, "eDP-1"));
  CHECK(matches(kBare, "HEADLESS-1"));
  CHECK(!matches(kMonitor, "HDMI-A-2"));
}

UMBRIEL_TEST(descriptorMatches) {
  CHECK_EQ(outputDescriptor(kMonitor), std::string("Microstep MSI G2712F CD6T084401192"));
  CHECK(matches(kMonitor, "Microstep MSI G2712F CD6T084401192"));
}
UMBRIEL_TEST(matchKindDistinguishesConnectorAndDescriptor) {
  CHECK(outputNameMatch(kMonitor, "hdmi-a-1") == OutputNameMatch::Connector);
  CHECK(outputNameMatch(kMonitor, "microstep msi g2712f cd6t084401192") == OutputNameMatch::Descriptor);
  CHECK(outputNameMatch(kMonitor, "HDMI-A-2") == OutputNameMatch::None);
}

// A missing field becomes "Unknown" rather than collapsing the separators, so
// the descriptor stays a stable three-part string.
UMBRIEL_TEST(missingFieldsBecomeUnknown) {
  CHECK_EQ(outputDescriptor(kPanel), std::string("Najing CEC Panda FPD Technology CO. ltd 0x0036 Unknown"));
  CHECK(matches(kPanel, "Najing CEC Panda FPD Technology CO. ltd 0x0036 Unknown"));
}

UMBRIEL_TEST(matchingIsCaseInsensitive) {
  CHECK(matches(kMonitor, "hdmi-a-1"));
  CHECK(matches(kMonitor, "microstep msi g2712f cd6t084401192"));
  CHECK(matches(kMonitor, "MICROSTEP MSI G2712F CD6T084401192"));
}
UMBRIEL_TEST(outputNameEqualityUsesTheSameAsciiCaseFolding) {
  CHECK(outputNamesEqual("DP-1", "dp-1"));
  CHECK(!outputNamesEqual("DP-1", "DP-2"));
  CHECK(!outputNamesEqual("DP-1", "DP-10"));
}

UMBRIEL_TEST(virtualOutputNamesExcludeTargetSeparators) {
  CHECK(validVirtualOutputName("sunshine"));
  CHECK(validVirtualOutputName("Stream_2.4k-60"));
  CHECK(!validVirtualOutputName(""));
  CHECK(!validVirtualOutputName("a/b"));
  CHECK(!validVirtualOutputName("x:1"));
  CHECK(!validVirtualOutputName("my stream"));
  CHECK(!validVirtualOutputName("\"stream\""));
  CHECK(!validVirtualOutputName("str\u00e9am"));
}

// The guard that stops every EDID-less output answering to the same string.
UMBRIEL_TEST(outputWithoutEdidMatchesOnlyItsConnector) {
  CHECK(!matches(kBare, "Unknown Unknown Unknown"));
  CHECK(matches(kBare, "HEADLESS-1"));
}

// Two panels on the same connector are the whole point: a rule written for one
// desk must not apply at the other.
UMBRIEL_TEST(sameConnectorDifferentPanelsAreDistinguishable) {
  constexpr OutputIdentity home{
      .connector = "HDMI-A-1", .make = "Microstep", .model = "MSI G2712F", .serial = "CD6T084401192"
  };
  constexpr OutputIdentity office{
      .connector = "HDMI-A-1", .make = "Microstep", .model = "MSI MP275Q", .serial = "PC3M805701148"
  };
  CHECK(matches(home, "Microstep MSI G2712F CD6T084401192"));
  CHECK(!matches(office, "Microstep MSI G2712F CD6T084401192"));
  CHECK(matches(office, "Microstep MSI MP275Q PC3M805701148"));
  CHECK(!matches(home, "Microstep MSI MP275Q PC3M805701148"));
  // Both still answer to the connector, which is why connector-keyed rules
  // cannot tell them apart.
  CHECK(matches(home, "HDMI-A-1"));
  CHECK(matches(office, "HDMI-A-1"));
}

UMBRIEL_TEST(partialDescriptorsDoNotMatch) {
  CHECK(!matches(kMonitor, "Microstep"));
  CHECK(!matches(kMonitor, "MSI G2712F"));
  CHECK(!matches(kMonitor, "Microstep MSI G2712F"));
  CHECK(!matches(kMonitor, ""));
}

int main() { return RUN_TESTS(); }
