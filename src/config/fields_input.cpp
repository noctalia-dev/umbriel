// [input], its device classes, and [[input.device]] rules.

#include "config/fields.h"
#include "config/keybind_parse.h"
#include "config/store.h"

// clang-format off
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    // Well past any real layout: a value this large already means "no limit".
    constexpr double kMaxFollowsMouseScroll = 100.0;

    std::vector<std::string_view> splitWhitespace(std::string_view text) {
      std::vector<std::string_view> tokens;
      size_t offset = 0;
      while (offset < text.size()) {
        while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
          ++offset;
        }
        const size_t start = offset;
        while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
          ++offset;
        }
        if (start != offset) {
          tokens.push_back(text.substr(start, offset - start));
        }
      }
      return tokens;
    }

    std::optional<AccelProfile> parseAccelProfile(const toml::node& node, const std::string& path) {
      const auto* value = node.as_string();
      if (value == nullptr) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      const std::vector<std::string_view> tokens = splitWhitespace(value->get());
      if (tokens.empty()) {
        warnAt(node.source(), "{} cannot be empty", path);
        return std::nullopt;
      }
      const std::string profile = lowercase(tokens.front());
      if ((profile == "flat" || profile == "adaptive") && tokens.size() == 1) {
        return AccelProfile{
            .kind = profile == "flat" ? AccelProfile::Kind::Flat : AccelProfile::Kind::Adaptive,
            .step = 0.0,
            .points = {},
        };
      }
      if (profile != "custom" || tokens.size() < 4) {
        warnAt(
            node.source(), R"(invalid {} "{}" (expected "flat", "adaptive", or "custom <step> <points...>"))", path,
            value->get()
        );
        return std::nullopt;
      }

      std::vector<double> values;
      values.reserve(tokens.size() - 1);
      for (size_t index = 1; index < tokens.size(); ++index) {
        const std::string_view token = tokens[index];
        double number = 0.0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), number);
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(number)) {
          warnAt(node.source(), R"(invalid number "{}" in {})", token, path);
          return std::nullopt;
        }
        values.push_back(number);
      }
      if (values.front() <= 0.0) {
        warnAt(node.source(), "{} custom step must be greater than zero", path);
        return std::nullopt;
      }
      if (std::ranges::any_of(values.begin() + 1, values.end(), [](double point) { return point < 0.0; })) {
        warnAt(node.source(), "{} custom points must be non-negative", path);
        return std::nullopt;
      }
      return AccelProfile{
          .kind = AccelProfile::Kind::Custom,
          .step = values.front(),
          .points = std::vector<double>(values.begin() + 1, values.end()),
      };
    }

    // `accel_profile` on any device class whose struct has an `accelProfile` member.
    template <typename T> registry::Field<T> accelProfileField(std::optional<AccelProfile> T::* member) {
      return registry::custom<T>(
          "accel_profile",
          registry::KeyDescription("string").withValues({"flat", "adaptive"}).withFormat("accel_profile"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            if (auto profile = parseAccelProfile(node, path)) {
              target.*member = std::move(profile);
            }
          }
      );
    }

    const registry::Fields<Config::Input::Touchpad::ScrollFactor>& scrollFactorAxes() {
      using Axes = Config::Input::Touchpad::ScrollFactor;
      static const registry::Fields<Axes> fields{
          registry::real("horizontal", 0.1, 10.0, &Axes::horizontal),
          registry::real("vertical", 0.1, 10.0, &Axes::vertical),
      };
      return fields;
    }

    std::optional<Config::Input::Touchpad::ScrollFactor>
    parseScrollFactor(const toml::node& node, const std::string& path, registry::ReadContext& context) {
      Config::Input::Touchpad::ScrollFactor factor;
      if (const toml::table* table = node.as_table()) {
        Section axes(*table, path, configStore().mutableDiagnostics());
        registry::readFields(axes, scrollFactorAxes(), factor, context);
        return factor;
      }
      const auto value = node.value<double>();
      if (!value || std::isnan(*value)) {
        warnAt(node.source(), "ignoring {} (expected number or table)", path);
        return std::nullopt;
      }
      const double used = std::clamp(*value, 0.1, 10.0);
      if (used != *value) {
        warnAt(node.source(), "{} = {} out of range, clamped to {}", path, *value, used);
      }
      return Config::Input::Touchpad::ScrollFactor{.horizontal = used, .vertical = used};
    }

    std::optional<std::array<float, 6>> parseCalibrationMatrix(const toml::node& node, const std::string& path) {
      const auto* array = node.as_array();
      if (array == nullptr || array->size() != 6) {
        warnAt(node.source(), "{} must be an array of 6 finite numbers", path);
        return std::nullopt;
      }
      std::array<float, 6> matrix{};
      for (size_t index = 0; index < 6; ++index) {
        const auto value = (*array)[index].value<double>();
        if (!value || !std::isfinite(*value)) {
          warnAt(node.source(), "{} must be an array of 6 finite numbers", path);
          return std::nullopt;
        }
        matrix[index] = static_cast<float>(*value);
      }
      return matrix;
    }

    bool validateKeyboardInput(
        const Config::Input::Keyboard& keyboard, const toml::source_region& source, std::string_view context
    ) {
      if (keyboard.layout.empty() && keyboard.variant.empty() && keyboard.options.empty()) {
        return true;
      }
      xkb_context* xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (xkbContext == nullptr) {
        warnAt(source, "unable to validate {} XKB configuration", context);
        return false;
      }
      const xkb_rule_names names{
          .rules = nullptr,
          .model = nullptr,
          .layout = keyboard.layout.empty() ? nullptr : keyboard.layout.c_str(),
          .variant = keyboard.variant.empty() ? nullptr : keyboard.variant.c_str(),
          .options = keyboard.options.empty() ? nullptr : keyboard.options.c_str(),
      };
      xkb_keymap* keymap = xkb_keymap_new_from_names(xkbContext, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (keymap == nullptr) {
        warnAt(
            source, "ignoring {} layout='{}' variant='{}' options='{}' (invalid XKB configuration)", context,
            keyboard.layout, keyboard.variant, keyboard.options
        );
        xkb_context_unref(xkbContext);
        return false;
      }
      xkb_keymap_unref(keymap);
      xkb_context_unref(xkbContext);
      return true;
    }

    // A device rule is kept once it names a device no earlier rule names. Its XKB keys are checked merged over the
    // session keyboard they override, and dropped together when that fails.
    bool acceptDevice(
        Section& keys, const toml::node& entry, Config::Input::Device& device, Config::Input& input,
        registry::ReadContext&, bool fieldsAccepted
    ) {
      const std::string& context = keys.name();
      if (entry.as_table()->get("name") == nullptr) {
        errorAt(entry.source(), "{} must set name", context);
        return false;
      }
      if (!fieldsAccepted || device.name.empty()) {
        return false;
      }
      if (input.findDevice(device.name) != nullptr) {
        errorAt(entry.source(), "{} duplicates device '{}'", context, device.name);
        return false;
      }
      if (device.layout || device.variant || device.options) {
        Config::Input::Keyboard keyboard = input.keyboard;
        keyboard.layout = device.layout.value_or(keyboard.layout);
        keyboard.variant = device.variant.value_or(keyboard.variant);
        keyboard.options = device.options.value_or(keyboard.options);
        if (!validateKeyboardInput(keyboard, entry.source(), context)) {
          device.layout.reset();
          device.variant.reset();
          device.options.reset();
        }
      }
      return true;
    }

    const registry::Fields<Config::Input>& inputFields() {
      using registry::boolean;
      using registry::Case;
      using registry::choice;
      using registry::Choice;
      using registry::custom;
      using registry::Fields;
      using registry::integer;
      using registry::real;
      using registry::table;
      using registry::text;
      using In = Config::Input;

      static const std::vector<Choice<ClickMethod>> clickMethods{
          {.name = "button_areas", .value = ClickMethod::ButtonAreas},
          {.name = "clickfinger", .value = ClickMethod::ClickFinger},
      };
      static const std::vector<Choice<TapButtonMap>> tapButtonMaps{
          {.name = "left_right_middle", .value = TapButtonMap::LeftRightMiddle},
          {.name = "left_middle_right", .value = TapButtonMap::LeftMiddleRight},
      };
      static const std::vector<Choice<uint32_t>> scrollButtons{
          {.name = "MouseLeft", .value = BTN_LEFT},     {.name = "MouseRight", .value = BTN_RIGHT},
          {.name = "MouseMiddle", .value = BTN_MIDDLE}, {.name = "MouseBack", .value = BTN_SIDE},
          {.name = "MouseForward", .value = BTN_EXTRA},
      };

      static const Fields<In::Keyboard> keyboard{
          text("layout", &In::Keyboard::layout),
          text("variant", &In::Keyboard::variant),
          text("options", &In::Keyboard::options),
          integer("repeat_rate", 0, 1000, &In::Keyboard::repeatRate),
          integer("repeat_delay", 0, 10000, &In::Keyboard::repeatDelay),
          boolean("numlock_toggle", &In::Keyboard::numlockToggle),
          choice(
              "track_layout", &In::Keyboard::trackLayout,
              {{.name = "global", .value = TrackLayout::Global}, {.name = "window", .value = TrackLayout::Window}}
          ),
      };
      static const Fields<In::Touchpad> touchpad{
          boolean("tap", &In::Touchpad::tap),
          boolean("natural_scroll", &In::Touchpad::naturalScroll),
          boolean("left_handed", &In::Touchpad::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Touchpad::sensitivity),
          boolean("disable_while_typing", &In::Touchpad::disableWhileTyping),
          boolean("disable_on_external_mouse", &In::Touchpad::disableOnExternalMouse),
          custom<In::Touchpad>(
              "scroll_factor", registry::KeyDescription("float_or_table").withRange(0.1, 10.0),
              [](const toml::node& node, const std::string& path, In::Touchpad& target,
                 registry::ReadContext& context) {
                if (auto factor = parseScrollFactor(node, path, context)) {
                  target.scrollFactor = factor;
                }
              },
              nullptr,
              [] {
                registry::Descriptions axes;
                registry::describeFields(scrollFactorAxes(), {}, "", axes);
                return axes;
              }()
          ),
          accelProfileField(&In::Touchpad::accelProfile),
          choice("click_method", &In::Touchpad::clickMethod, clickMethods, Case::Fold),
          choice("tap_button_map", &In::Touchpad::tapButtonMap, tapButtonMaps, Case::Fold),
      };
      static const Fields<In::Mouse> mouse{
          boolean("natural_scroll", &In::Mouse::naturalScroll),
          boolean("left_handed", &In::Mouse::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Mouse::sensitivity),
          integer("scroll_wheel_step", 1, 1000, &In::Mouse::scrollWheelStep),
          boolean("scroll_button_lock", &In::Mouse::scrollButtonLock),
          accelProfileField(&In::Mouse::accelProfile),
          choice("scroll_button", &In::Mouse::scrollButton, scrollButtons, Case::Fold),
      };
      static const Fields<In::Tablet> tablet{
          boolean("enabled", &In::Tablet::enabled),
          text("map_to_output", &In::Tablet::mapToOutput),
          boolean("map_to_focused_output", &In::Tablet::mapToFocusedOutput),
          boolean("map_to_focused_window", &In::Tablet::mapToFocusedWindow),
          boolean("left_handed", &In::Tablet::leftHanded),
          custom<In::Tablet>(
              "calibration_matrix", registry::KeyDescription("float_array"),
              [](const toml::node& node, const std::string& path, In::Tablet& target, registry::ReadContext&) {
                if (auto matrix = parseCalibrationMatrix(node, path)) {
                  target.calibrationMatrix = matrix;
                }
              }
          ),
      };
      static const Fields<In::Touch> touch{
          boolean("enabled", &In::Touch::enabled),
          text("map_to_output", &In::Touch::mapToOutput),
      };
      static const Fields<In::Cursor> cursor{
          text("theme", &In::Cursor::theme),
          integer("size", 1, 512, &In::Cursor::size),
          boolean("hardware_cursor", &In::Cursor::hardwareCursor),
          boolean("follows_focus", &In::Cursor::followsFocus),
          boolean("hide_when_typing", &In::Cursor::hideWhenTyping),
          integer("hide_timeout_ms", 0, 3600000, &In::Cursor::hideTimeoutMs),
      };
      // The limit is measured in viewport widths and the quantity it is compared against is unbounded: revealing a
      // column three screens away is 3.0. The upper bound here is a nonsense-catcher, not a ceiling. Below zero would
      // refuse focus even for a window already fully visible, which disables hover focus rather than limiting it.
      static const Fields<In::Focus> focus{
          boolean("follows_mouse", &In::Focus::followsMouse),
          real("follows_mouse_max_scroll", 0.0, kMaxFollowsMouseScroll, &In::Focus::followsMouseMaxScroll),
      };
      static const Fields<In::Device> device{
          custom<In::Device>(
              "name", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, In::Device& target, registry::ReadContext&) {
                if (const auto name = node.value<std::string>(); name && !name->empty()) {
                  target.name = *name;
                } else {
                  errorAt(node.source(), "{} must be a non-empty string", path);
                }
              }
          ),
          text("layout", &In::Device::layout),
          text("variant", &In::Device::variant),
          text("options", &In::Device::options),
          integer("repeat_rate", 0, 1000, &In::Device::repeatRate),
          integer("repeat_delay", 0, 10000, &In::Device::repeatDelay),
          boolean("tap", &In::Device::tap),
          boolean("natural_scroll", &In::Device::naturalScroll),
          boolean("left_handed", &In::Device::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Device::sensitivity),
          boolean("disable_while_typing", &In::Device::disableWhileTyping),
          boolean("scroll_button_lock", &In::Device::scrollButtonLock),
          accelProfileField(&In::Device::accelProfile),
          choice("click_method", &In::Device::clickMethod, clickMethods, Case::Fold),
          choice("tap_button_map", &In::Device::tapButtonMap, tapButtonMaps, Case::Fold),
          choice("scroll_button", &In::Device::scrollButton, scrollButtons, Case::Fold),
      };
      static const Fields<In> fields{
          boolean("middle_click_paste", &In::middleClickPaste),
          boolean("client_window_drag", &In::clientWindowDrag),
          choice(
              "window_drag_toggle", &In::windowDragToggle,
              {{.name = "none", .value = WindowDragToggle::None},
               {.name = "floating", .value = WindowDragToggle::Floating},
               {.name = "pinned", .value = WindowDragToggle::Pinned}}
          ),
          table(
              "keyboard", &In::keyboard, keyboard,
              [](const toml::node& node, In::Keyboard& target, registry::ReadContext&) {
                if (!validateKeyboardInput(target, node.source(), "input.keyboard")) {
                  target.layout.clear();
                  target.variant.clear();
                  target.options.clear();
                }
                return true;
              }
          ),
          table("touchpad", &In::touchpad, touchpad),
          table("mouse", &In::mouse, mouse),
          table("tablet", &In::tablet, tablet),
          table("touch", &In::touch, touch),
          table("cursor", &In::cursor, cursor),
          table("focus", &In::focus, focus),
          registry::rules("device", &In::devices, device, registry::Shape::Error, acceptDevice),
      };
      return fields;
    }

  } // namespace

  registry::Field<Config> inputTable() { return registry::table("input", &Config::input, inputFields()); }

} // namespace umbriel
