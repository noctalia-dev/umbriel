// [output.<name>] rules.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/store.h"
#include "config/value_parse.h"
#include "output/identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    OutputRule* findOutputRuleMutable(Config& loaded, const std::string& name) {
      const auto it = std::ranges::find_if(loaded.outputs, [&](const OutputRule& rule) {
        return outputNamesEqual(rule.name, name);
      });
      return it != loaded.outputs.end() ? &*it : nullptr;
    }

    // A count, a list of names, or "dynamic".
    void parseOutputWorkspaces(const toml::node& node, const std::string& path, OutputRule& rule) {
      if (const auto count = node.value<std::int64_t>()) {
        if (*count < 1 || *count > static_cast<std::int64_t>(kMaxWorkspaces)) {
          errorAt(node.source(), "{} must be an integer from 1 to {}", path, kMaxWorkspaces);
        } else {
          rule.workspaces = static_cast<size_t>(*count);
        }
        return;
      }
      if (const auto* names = node.as_array()) {
        bool valid = true;
        if (names->empty() || names->size() > kMaxWorkspaces) {
          errorAt(node.source(), "{} must contain 1 to {} names", path, kMaxWorkspaces);
          valid = false;
        }
        std::vector<std::string> parsed;
        parsed.reserve(names->size());
        for (const auto& item : *names) {
          const auto value = item.value<std::string>();
          if (!value || value->empty()) {
            errorAt(item.source(), "{} entries must be non-empty strings", path);
            valid = false;
            continue;
          }
          if (std::ranges::find(parsed, *value) != parsed.end()) {
            errorAt(item.source(), "{} contains duplicate name '{}'", path, *value);
            valid = false;
            continue;
          }
          parsed.push_back(*value);
        }
        if (valid) {
          rule.workspaces = std::move(parsed);
        }
        return;
      }
      if (node.value<std::string>() != "dynamic") {
        errorAt(node.source(), R"({} must be a count, a name array, or "dynamic")", path);
      }
    }

    // Same "~" semantics as include paths; other placeholders are not
    // expanded in profile paths.
    std::string expandProfileHome(std::string_view raw) {
      if (raw == "~" || raw.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          return raw == "~" ? std::string(home) : std::string(home).append(raw.substr(1));
        }
      }
      return std::string(raw);
    }

    // "none", "edid", or a path to an ICC profile. Config stays pure data: the
    // file is only checked for readability here and parsed at apply time. The
    // rule records the file's mtime so a reload of an edited profile reads as
    // a changed rule and re-opens the file; apply-time failures fall back to
    // EDID, so a path that cannot resolve to a regular file does too.
    void parseColorProfile(const toml::node& node, const std::string& path, OutputRule& target) {
      const auto value = node.value<std::string>();
      if (!value || value->empty() || value->contains('\0')) {
        warnAt(node.source(), R"(ignoring {} (expected "none", "edid", or a profile path))", path);
        return;
      }
      if (*value == "none" || *value == "edid") {
        target.colorProfile = *value;
        return;
      }
      std::filesystem::path file(expandProfileHome(*value));
      if (file.is_relative()) {
        if (node.source().path == nullptr || node.source().path->empty()) {
          warnAt(
              node.source(), R"({}: relative profile path requires a config source path; falling back to "edid")", path
          );
          target.colorProfile = "edid";
          return;
        }
        file = std::filesystem::path(*node.source().path).parent_path() / file;
      }
      file = file.lexically_normal();
      configStore().addWatchPath(file);
      // O_NONBLOCK avoids hanging on a FIFO before fstat can reject it. The
      // bytes are never read here: ICC parsing happens at apply time.
      const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      if (fd < 0) {
        warnAt(
            node.source(), R"({}: cannot read profile '{}' ({}); falling back to "edid")", path, file.string(),
            std::strerror(errno)
        );
        target.colorProfile = "edid";
        return;
      }
      struct stat metadata{};
      const bool regular = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode);
      const long long stamp =
          regular ? static_cast<long long>(metadata.st_mtim.tv_sec) * 1000000000LL + metadata.st_mtim.tv_nsec : 0;
      close(fd);
      if (!regular) {
        warnAt(
            node.source(), R"({}: profile '{}' must be a readable regular file; falling back to "edid")", path,
            file.string()
        );
        target.colorProfile = "edid";
        return;
      }
      target.colorProfile = file.string();
      target.colorProfileMtime = stamp;
    }

    const registry::Fields<OutputRule>& outputFields() {
      using registry::boolean;
      using registry::custom;
      using registry::KeyDescription;
      using O = OutputRule;
      static const registry::Fields<O::Layout::Scrolling> scrolling{
          registry::real(
              "default_extent_fraction", 0.1, kMaxWideFraction, &O::Layout::Scrolling::defaultExtentFraction
          ),
      };
      static const registry::Fields<O::Layout> layout{
          registry::table("scrolling", &O::Layout::scrolling, scrolling),
      };
      static const registry::Fields<O> fields{
          boolean("enabled", &O::enabled),
          boolean("focus_at_startup", &O::focusAtStartup),
          boolean("tearing", &O::allowTearing),
          boolean("direct_scanout", &O::directScanout),
          optionalEffectField("screen_effect", &O::screenEffect),
          registry::table("layout", &O::layout, layout),
          registry::integer("min_workspaces", 1, static_cast<int>(kMaxWorkspaces), &O::minWorkspaces),
          boolean("cyclic_workspaces", &O::cyclicWorkspaces),
          registry::choice(
              "workspace_axis", &O::workspaceAxis,
              {{.name = "vertical", .value = WorkspaceAxis::Vertical},
               {.name = "horizontal", .value = WorkspaceAxis::Horizontal}}
          ),
          custom<O>(
              "workspaces", KeyDescription("int_or_string_array").withValues({"dynamic"}).withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                parseOutputWorkspaces(node, path, target);
              },
              [](const O&) { return nlohmann::ordered_json("dynamic"); }
          ),
          custom<O>(
              "mode", KeyDescription("string").withFormat("output_mode"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::string>();
                OutputMode mode;
                if (!value || !parseOutputMode(*value, mode)) {
                  warnAt(node.source(), R"(ignoring {} (expected "WIDTHxHEIGHT" or "WIDTHxHEIGHT@HZ"))", path);
                } else {
                  target.mode = mode;
                }
              }
          ),
          custom<O>(
              "position", KeyDescription("int_array").withRange(-100000, 100000),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto* position = node.as_array();
                bool valid = position != nullptr && position->size() == 2;
                std::array<int, 2> parsed{};
                for (size_t index = 0; valid && index < parsed.size(); ++index) {
                  const auto value = (*position)[index].value<std::int64_t>();
                  valid = value.has_value();
                  if (valid) {
                    parsed[index] = static_cast<int>(
                        std::clamp(*value, static_cast<std::int64_t>(-100000), static_cast<std::int64_t>(100000))
                    );
                  }
                }
                if (!valid) {
                  warnAt(node.source(), "ignoring {} (expected [x, y] integers)", path);
                } else {
                  target.position = parsed;
                }
              }
          ),
          registry::real("scale", 0.25, 4.0, &O::scale),
          registry::choice("vrr", &O::vrr, vrrModes()),
          registry::choice("hdr", &O::hdr, hdrModes()),
          registry::real("sdr_white", 80.0, 1000.0, &O::sdrWhite),
          registry::choice(
              "sdr_transfer", &O::sdrTransfer,
              {{.name = "gamma22", .value = SdrTransfer::Gamma22}, {.name = "srgb", .value = SdrTransfer::Srgb}}
          ),
          custom<O>(
              "color_profile", KeyDescription("string").withValues({"none", "edid"}).withFormat("path"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                parseColorProfile(node, path, target);
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.colorProfile); }
          ),
          registry::choice(
              "sdr_gamut", &O::sdrGamut,
              {{.name = "srgb", .value = SdrGamut::Srgb}, {.name = "native", .value = SdrGamut::Native}}
          ),
          custom<O>(
              "bit_depth", KeyDescription("int").withRange(8, 10),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::int64_t>();
                if (value && (*value == 8 || *value == 10)) {
                  target.bitDepth = static_cast<int>(*value);
                } else {
                  warnAt(node.source(), "ignoring {} (expected 8 or 10)", path);
                }
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.bitDepth); }
          ),
          registry::choice(
              "transform", &O::transform,
              {{.name = "normal", .value = 0},
               {.name = "90", .value = 1},
               {.name = "180", .value = 2},
               {.name = "270", .value = 3},
               {.name = "flipped", .value = 4},
               {.name = "flipped-90", .value = 5},
               {.name = "flipped-180", .value = 6},
               {.name = "flipped-270", .value = 7}}
          ),
      };
      return fields;
    }

    // A later section for the same output replaces an earlier one.
    void acceptOutput(
        const toml::key& key, Section& keys, const toml::node&, OutputRule& rule, Config& loaded,
        registry::ReadContext& context
    ) {
      const std::string name(key.str());
      rule.name = name;
      if (std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name);
          })) {
        warnAt(key.source(), "duplicate output section '{}'", name);
        // Drop the discarded section's references so they cannot clear the surviving rule's value by name.
        std::erase_if(context.effectReferences, [&](const EffectReference& reference) {
          return std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name)
                && reference.context == "output." + existing.name + ".screen_effect";
          });
        });
        std::erase_if(loaded.outputs, [&](const OutputRule& existing) {
          return outputNamesEqual(existing.name, name);
        });
      }
      if (const toml::node* minNode = keys.node("min_workspaces"); minNode != nullptr && rule.workspaces) {
        errorAt(minNode->source(), "{} requires dynamic workspaces", keys.qualified("min_workspaces"));
      }
      loaded.outputs.push_back(std::move(rule));
      recordRuleEffect(context, keys, "screen_effect", EffectKind::Screen, [&loaded, name] {
        if (OutputRule* output = findOutputRuleMutable(loaded, name)) {
          output->screenEffect.reset();
        }
      });
    }

  } // namespace

  registry::Field<Config> outputTable() {
    return registry::namedTables<Config>("output", outputFields(), acceptOutput);
  }

} // namespace umbriel
