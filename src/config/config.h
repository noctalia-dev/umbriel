#pragma once

#include "config/config_diag.h"
#include "config/keybind_parse.h"
#include "config/value_parse.h"
#include "core/animation.h"
#include "layout/layout.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  inline constexpr size_t kMaxWorkspaces = 64;
  struct ConfigReloadResult;

  enum class ModifierKey {
    Super,
    Alt,
    Control,
    Shift,
  };

  struct AccelProfile {
    enum class Kind {
      Flat,
      Adaptive,
      Custom,
    } kind = Kind::Flat;
    double step = 0.0;
    std::vector<double> points;
    bool operator==(const AccelProfile&) const = default;
  };

  // Per-workspace layout overrides (all optional → inherit Config::Layout).
  struct WorkspaceLayoutOverrides {
    std::optional<LayoutMode> mode;
    std::optional<int> gap;
    std::optional<std::vector<double>> widthPresets;
    struct Scrolling {
      std::optional<double> defaultWidthFraction;
      std::optional<bool> centerUnderfullStrip;
      std::optional<ScrollingDirection> direction;
      std::optional<bool> expandSingleColumn;
      bool operator==(const Scrolling&) const = default;
    } scrolling;
    struct Dwindle {
      std::optional<bool> preserveSplit;
      bool operator==(const Dwindle&) const = default;
    } dwindle;
    struct Master {
      std::optional<double> defaultWidthFraction;
      std::optional<MasterPosition> position;
      bool operator==(const Master&) const = default;
    } master;

    bool operator==(const WorkspaceLayoutOverrides&) const = default;
  };

  // Layout rule parsed from a [[workspace]] entry. Exactly one selector is set.
  struct WorkspaceConfig {
    std::string name;
    std::string output;       // optional output selector
    std::optional<int> index; // optional 1-based position selector
    WorkspaceLayoutOverrides layout;
    bool operator==(const WorkspaceConfig&) const = default;
  };

  // Fully resolved layout config. Owned by each Workspace.
  struct ResolvedLayoutConfig {
    LayoutMode mode = LayoutMode::Scrolling;
    int gap = 8;
    std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
    struct Scrolling {
      std::optional<double> defaultWidthFraction;
      bool centerUnderfullStrip = true;
      // Axis-agnostic layout state is preserved when config reload changes direction.
      ScrollingDirection direction = ScrollingDirection::Horizontal;
      bool expandSingleColumn = false;
      bool operator==(const Scrolling&) const = default;
    } scrolling;
    struct Dwindle {
      bool preserveSplit = false;
      bool operator==(const Dwindle&) const = default;
    } dwindle;
    struct Master {
      double defaultWidthFraction = 0.55;
      MasterPosition position = MasterPosition::Left;
      bool operator==(const Master&) const = default;
    } master;
    // Derived from gap + appearance border widths; set by resolve function.
    int totalGap = 0; // gap + 2 * totalBorderWidth
    int edgePad = 0;  // gap + totalBorderWidth
    bool operator==(const ResolvedLayoutConfig&) const = default;
  };

  // Resolved workspace entry for a specific output (name + layout config).
  struct ResolvedWorkspace {
    std::string name;
    ResolvedLayoutConfig layout;
    bool operator==(const ResolvedWorkspace&) const = default;
  };
  struct ResolvedWorkspaceSet {
    bool dynamic = false;
    std::vector<ResolvedWorkspace> workspaces;
    bool operator==(const ResolvedWorkspaceSet&) const = default;
  };
  enum class VrrMode {
    Disabled,
    Always,
    Fullscreen,
  };
  enum class HdrMode {
    Off,
    On,
    Auto,
    Fullscreen,
  };
  [[nodiscard]] constexpr std::string_view hdrModeName(HdrMode mode) {
    switch (mode) {
    case HdrMode::Off:
      return "off";
    case HdrMode::On:
      return "on";
    case HdrMode::Auto:
      return "auto";
    case HdrMode::Fullscreen:
      return "fullscreen";
    }
    return "off";
  }
  [[nodiscard]] constexpr bool vrrEnabled(VrrMode mode, bool fullscreen) {
    return mode == VrrMode::Always || (mode == VrrMode::Fullscreen && fullscreen);
  }
  [[nodiscard]] constexpr bool hdrEnabled(HdrMode mode, bool fullscreen, bool autoEligible) {
    return mode == HdrMode::On
        || (mode == HdrMode::Auto && autoEligible)
        || (mode == HdrMode::Fullscreen && fullscreen);
  }
  [[nodiscard]] constexpr bool effectiveVrrEnabled(
      VrrMode outputMode, bool outputFullscreen, std::optional<VrrMode> windowMode, bool windowFullscreen
  ) {
    return windowMode ? vrrEnabled(*windowMode, windowFullscreen) : vrrEnabled(outputMode, outputFullscreen);
  }
  [[nodiscard]] constexpr bool
  tearingEnabled(bool outputAllowed, std::optional<bool> windowOverride, bool clientHintAsync) {
    return outputAllowed && windowOverride.value_or(clientHintAsync);
  }
  struct OutputRule {
    std::string name;
    // False powers the monitor off, removes it from the layout, and hides its
    // workspaces from the desktop. Content is preserved while disabled.
    bool enabled = true;
    std::optional<OutputMode> mode;
    std::optional<std::array<int, 2>> position;
    std::optional<double> scale;
    std::optional<int> transform;
    VrrMode vrr = VrrMode::Disabled;
    // Global safety gate. Even a client async hint or a window-rule override
    // cannot request tearing unless the owning output enables it.
    bool allowTearing = false;
    // Allow eligible fullscreen buffers to bypass composition on this output.
    bool directScanout = true;
    HdrMode hdr = HdrMode::Off;
    float sdrWhite = 203.0F;
    // Explicit workspace inventory. Omitted means dynamic workspaces.
    std::optional<std::vector<std::string>> workspaces;
    bool operator==(const OutputRule&) const = default;
  };

  enum class WindowPositionAnchor {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Top,
    Bottom,
    Left,
    Right,
    Center,
  };

  struct WindowPosition {
    int x = 0;
    int y = 0;
    WindowPositionAnchor anchor = WindowPositionAnchor::Center;
    bool operator==(const WindowPosition&) const = default;
  };

  struct WindowRule {
    std::string appIdPattern;
    std::string titlePattern;
    std::regex appIdRegex;
    std::regex titleRegex;
    std::optional<bool> matchFocused;
    std::optional<std::string> defaultOutput;
    std::optional<bool> defaultFloating;
    std::optional<std::array<int, 2>> defaultSize; // [width, height]
    std::optional<WindowPosition> defaultPosition;
    std::optional<double> defaultWidth;  // column width fraction override
    std::optional<int> defaultWorkspace; // 1-64
    std::optional<bool> defaultFullscreen;
    std::optional<bool> defaultMaximizeToEdges;
    std::optional<bool> defaultMaximize;
    std::optional<bool> defaultFocused;
    std::optional<bool> defaultPinned;
    std::optional<bool> focusOnActivate;
    std::optional<VrrMode> vrr;
    // Overrides the client's tearing-control hint. Omitted follows the hint,
    // true forces async preference, and false vetoes it.
    std::optional<bool> allowTearing;
    std::optional<HdrMode> hdr;
    std::optional<double> opacity; // 0.0-1.0
    std::optional<bool> blur;
    std::optional<bool> blurPopups;
    std::optional<double> blurIgnoreAlpha;
    std::optional<bool> blurOptimized;

    // The compiled regexes are derived from the patterns and are not comparable,
    // so equality is decided by the patterns they came from.
    [[nodiscard]] bool operator==(const WindowRule& other) const {
      return appIdPattern == other.appIdPattern
          && titlePattern == other.titlePattern
          && matchFocused == other.matchFocused
          && defaultOutput == other.defaultOutput
          && defaultFloating == other.defaultFloating
          && defaultSize == other.defaultSize
          && defaultPosition == other.defaultPosition
          && defaultWidth == other.defaultWidth
          && defaultWorkspace == other.defaultWorkspace
          && defaultFullscreen == other.defaultFullscreen
          && defaultMaximizeToEdges == other.defaultMaximizeToEdges
          && defaultMaximize == other.defaultMaximize
          && defaultFocused == other.defaultFocused
          && defaultPinned == other.defaultPinned
          && focusOnActivate == other.focusOnActivate
          && vrr == other.vrr
          && allowTearing == other.allowTearing
          && hdr == other.hdr
          && opacity == other.opacity
          && blur == other.blur
          && blurPopups == other.blurPopups
          && blurIgnoreAlpha == other.blurIgnoreAlpha
          && blurOptimized == other.blurOptimized;
    }
  };

  // Resolved result: merge of all matching rules (last writer wins per field).
  struct ResolvedWindowRule {
    std::optional<std::string> defaultOutput;
    std::optional<bool> defaultFloating;
    std::optional<std::array<int, 2>> defaultSize;
    std::optional<WindowPosition> defaultPosition;
    std::optional<double> defaultWidth;
    std::optional<int> defaultWorkspace;
    std::optional<bool> defaultFullscreen;
    std::optional<bool> defaultMaximizeToEdges;
    std::optional<bool> defaultMaximize;
    std::optional<bool> defaultFocused;
    std::optional<bool> defaultPinned;
    std::optional<bool> focusOnActivate;
    std::optional<VrrMode> vrr;
    std::optional<bool> allowTearing;
    std::optional<HdrMode> hdr;
    std::optional<double> opacity;
    std::optional<bool> blur;
    std::optional<bool> blurPopups;
    std::optional<double> blurIgnoreAlpha;
    std::optional<bool> blurOptimized;
    bool operator==(const ResolvedWindowRule&) const = default;
  };

  struct LayerRule {
    std::string namespacePattern;
    std::regex namespaceRegex;
    std::optional<bool> blur;
    std::optional<bool> blurPopups;
    std::optional<double> ignoreAlpha;
    std::optional<bool> optimized;

    // See WindowRule: the regex is derived from the pattern.
    [[nodiscard]] bool operator==(const LayerRule& other) const {
      return namespacePattern == other.namespacePattern
          && blur == other.blur
          && blurPopups == other.blurPopups
          && ignoreAlpha == other.ignoreAlpha
          && optimized == other.optimized;
    }
  };

  struct ResolvedLayerRule {
    std::optional<bool> blur;
    std::optional<bool> blurPopups;
    std::optional<double> ignoreAlpha;
    std::optional<bool> optimized;
    bool operator==(const ResolvedLayerRule&) const = default;
  };

  struct Config {
    struct Colors {
      std::array<float, 4> background{0.0784314F, 0.0784314F, 0.0980392F, 0.9411765F};
      std::array<float, 4> textPrimary{0.9098039F, 0.9098039F, 0.9176471F, 1.0F};
      std::array<float, 4> textMuted{0.5411765F, 0.5411765F, 0.5725490F, 1.0F};
      std::array<float, 4> accentPrimary{0.4784314F, 0.6392157F, 1.0F, 1.0F};
      std::array<float, 4> accentSecondary{0.9607843F, 0.7882353F, 0.4196078F, 1.0F};
      std::array<float, 4> warning{0.9607843F, 0.7882353F, 0.4196078F, 1.0F};
      std::array<float, 4> error{1.0F, 0.4196078F, 0.4196078F, 1.0F};
      bool operator==(const Colors&) const = default;
    } colors;

    struct Appearance {
      int borderWidth = 2;
      int outerBorderWidth = 0;
      int cornerRadius = 10;
      std::array<float, 4> borderFocused{0.48F, 0.64F, 1.0F, 1.0F};
      std::array<float, 4> borderUnfocused{0.16F, 0.16F, 0.20F, 1.0F};
      std::array<float, 4> scratchpadBorderFocused{0.90F, 0.75F, 0.48F, 1.0F};
      std::array<float, 4> scratchpadBorderUnfocused{0.36F, 0.29F, 0.16F, 1.0F};
      std::array<float, 4> outerBorderColor{0.10F, 0.10F, 0.12F, 1.0F};
      std::array<float, 4> insertHintColor{0.50F, 0.78F, 1.0F, 0.50F};
      std::array<float, 4> backdropColor{0.0F, 0.0F, 0.0F, 1.0F};
      double dragOpacity = 0.75;
      struct Blur {
        bool enabled = true;
        bool optimized = true;
        int passes = 3;
        int radius = 5;
        double noise = 0.02;
        double brightness = 0.9;
        double contrast = 0.9;
        double saturation = 1.1;
        bool operator==(const Blur&) const = default;
      } blur;
      struct Shadow {
        bool enabled = true;
        int softness = 10;
        int offsetX = 2;
        int offsetY = 2;
        std::array<float, 4> color{0.0F, 0.0F, 0.0F, 0.50F};
        bool operator==(const Shadow&) const = default;
      } shadow;
      bool preferNoCsd = true;

      [[nodiscard]] int totalBorderWidth() const { return borderWidth + outerBorderWidth; }
      bool operator==(const Appearance&) const = default;
    } appearance;

    struct Animation {
      bool enabled = true;
      int durationMs = 250;
      AnimationCurve curve{.easing = Easing::EaseOutCubic};
      std::map<std::string, BezierCurve> beziers;
      std::map<std::string, SpringConfig> springs;

      struct WindowsIn {
        bool enabled = true;
        int durationMs = 150;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        std::string style = "popin";
        double scale = 0.85;
        bool operator==(const WindowsIn&) const = default;
      } windowsIn;

      struct WindowsOut {
        bool enabled = true;
        int durationMs = 150;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        std::string style = "fade";
        bool operator==(const WindowsOut&) const = default;
      } windowsOut;

      struct WindowsMove {
        bool enabled = true;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::Snappy};
        bool operator==(const WindowsMove&) const = default;
      } windowsMove;

      struct Workspaces {
        bool enabled = true;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        bool operator==(const Workspaces&) const = default;
      } workspaces;

      struct Overview {
        bool enabled = true;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        bool operator==(const Overview&) const = default;
      } overview;

      struct Scratchpad {
        bool enabled = false;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        double dim = 0.5;
        bool blur = false;
        double scale = 0.0;
        bool maximize = false;
        bool fullscreen = false;
        bool operator==(const Scratchpad&) const = default;
      } scratchpad;

      struct Border {
        bool enabled = false;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        bool operator==(const Border&) const = default;
      } border;

      struct DimUnfocused {
        bool enabled = false;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        double dim = 0.0;
        bool operator==(const DimUnfocused&) const = default;
      } dimUnfocused;

      struct Layers {
        bool enabled = false;
        int durationMs = 250;
        AnimationCurve curve{.easing = Easing::EaseOutCubic};
        bool operator==(const Layers&) const = default;
      } layers;

      bool operator==(const Animation&) const = default;
    } animation;

    struct Overview {
      // Workspace scale when fully zoomed out.
      double zoom = 0.5;
      // Blur the wallpaper behind the filmstrip while the overview is visible. Uses [appearance.blur] parameters;
      // inert when appearance blur is disabled.
      bool backgroundBlur = true;
      // Tint composited over the desktop background while overview is visible.
      std::array<float, 4> backgroundTint{0.0627451F, 0.0627451F, 0.0784314F, 0.1882353F};
      // Rounded background behind each workspace; alpha controls opacity.
      std::array<float, 4> workspaceBackground{0.0F, 0.0F, 0.0F, 0.2666667F};
      // Keyboard shortcut badges on overview cards. Pressing a badge key focuses
      // that window and closes the overview.
      bool shortcuts = true;
      // Favorite badge keys in preference order, one ASCII character each.
      std::string shortcutKeys = "1234567890";
      // Badge accent override. Unset follows colors.accent_primary.
      std::optional<std::array<float, 4>> badgeColor;
      bool operator==(const Overview&) const = default;
    } overview;

    struct HotCorner {
      bool enabled = false;
      int delayMs = 500;
      std::optional<Keybind> action;
      bool operator==(const HotCorner&) const = default;
    };

    struct HotCorners {
      // Corners are ordered top-left, top-right, bottom-left, bottom-right.
      std::array<HotCorner, 4> corners;
      bool operator==(const HotCorners&) const = default;
    } hotCorners;

    struct Layout {
      LayoutMode mode = LayoutMode::Scrolling;
      int gap = 8;
      std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
      struct Scrolling {
        std::optional<double> defaultWidthFraction;
        bool centerUnderfullStrip = true;
        ScrollingDirection direction = ScrollingDirection::Horizontal;
        bool expandSingleColumn = false;
        bool operator==(const Scrolling&) const = default;
      } scrolling;
      struct Dwindle {
        bool preserveSplit = false;
        bool operator==(const Dwindle&) const = default;
      } dwindle;
      struct Master {
        double defaultWidthFraction = 0.55;
        MasterPosition position = MasterPosition::Left;
        bool operator==(const Master&) const = default;
      } master;
      bool operator==(const Layout&) const = default;
    } layout;

    // Clear `layout.gap` outside decoration edges: borders are drawn outside the
    // surface, so tile spacing and usable-area insets include total border width.
    [[nodiscard]] int layoutGap() const { return layout.gap + 2 * appearance.totalBorderWidth(); }
    [[nodiscard]] int layoutEdgePad() const { return layout.gap + appearance.totalBorderWidth(); }

    struct Workspaces {
      // Re-selecting the active workspace jumps back to the previous one.
      bool backAndForth = false;
      bool emptyAbove = false;
      bool operator==(const Workspaces&) const = default;
    } workspaces;

    struct General {
      std::vector<std::string> autostart;
      // Symbolic `Mod` in keybinds. Unset preserves the runtime default:
      // Super on DRM, Alt when running nested.
      std::optional<ModifierKey> modKey;
      // Spawn and manage xwayland-satellite for X11 app support. Requires restart.
      bool xwayland = true;
      // Show the keybinds cheatsheet overlay on startup.
      bool showCheatsheet = true;
      // Honor activation requests by focusing and revealing the target window.
      bool focusOnActivate = false;
      // Honor maximized state restored by a client while its window opens.
      bool honorRestoredMaximize = false;
      bool operator==(const General&) const = default;
    } general;

    struct Environment {
      // Ordered NAME=value pairs exported to the compositor and the native session's systemd user manager.
      std::vector<std::pair<std::string, std::string>> variables;
      bool operator==(const Environment&) const = default;
    } environment;

    struct Input {
      // Advertise and accept the primary-selection clipboard used for
      // middle-click paste.
      bool middleClickPaste = true;

      struct Keyboard {
        // Comma-separated XKB layout list ("us,de"); the first entry is active at startup. `options` carries XKB option
        // names such as `grp:alt_shift_toggle`, which is what makes a second layout reachable from the keyboard itself
        // rather than only through the `keyboard-layout-next` action.
        std::string layout;
        std::string variant;
        std::string options;
        int repeatRate = 25;
        int repeatDelay = 600;
        bool numlockToggle = false;
        bool operator==(const Keyboard&) const = default;
      } keyboard;

      struct Touchpad {
        std::optional<bool> tap = true;
        std::optional<bool> naturalScroll;
        std::optional<AccelProfile> accelProfile;
        std::optional<double> sensitivity;
        std::optional<double> scrollFactor;
        std::optional<bool> disableWhileTyping;
        bool operator==(const Touchpad&) const = default;
      } touchpad;

      struct Mouse {
        std::optional<bool> naturalScroll;
        std::optional<AccelProfile> accelProfile;
        double sensitivity = 0.0;
        int scrollWheelStep = 60;
        bool operator==(const Mouse&) const = default;
      } mouse;

      struct Cursor {
        std::string theme;
        int size = 24;
        bool hardwareCursor = true;
        // Warp to the window selected by explicit focus-navigation actions.
        bool followsFocus = false;
        bool hideWhenTyping = false;
        // Milliseconds without pointer activity before hiding the cursor. Zero disables it.
        int hideTimeoutMs = 0;
        bool operator==(const Cursor&) const = default;
      } cursor;

      struct Focus {
        bool followsMouse = false;
        std::optional<double> followsMouseMaxScroll;
        bool operator==(const Focus&) const = default;
      } focus;

      struct Tablet {
        // false silences the tablet and its pads via libinput
        bool enabled = true;
        // empty = no static output mapping
        std::string mapToOutput;
        bool mapToFocusedOutput = false;
        bool mapToFocusedWindow = false;
        bool leftHanded = false;
        std::optional<std::array<float, 6>> calibrationMatrix;
        bool operator==(const Tablet&) const = default;
      } tablet;

      struct Device {
        std::string name;
        std::optional<std::string> layout;
        std::optional<std::string> variant;
        std::optional<std::string> options;
        std::optional<int> repeatRate;
        std::optional<int> repeatDelay;
        std::optional<bool> tap;
        std::optional<bool> naturalScroll;
        std::optional<AccelProfile> accelProfile;
        std::optional<double> sensitivity;
        std::optional<bool> disableWhileTyping;
        bool operator==(const Device&) const = default;
      };

      std::vector<Device> devices;
      [[nodiscard]] const Device* findDevice(std::string_view name) const;
      bool operator==(const Input&) const = default;
    } input;

    std::vector<Keybind> keybinds;
    std::vector<OutputRule> outputs;
    std::vector<WindowRule> windowRules;
    std::vector<LayerRule> layerRules;
    std::vector<WorkspaceConfig> workspaceRules; // [[workspace]] layout rules

    bool operator==(const Config&) const = default;
  };

  [[nodiscard]] const Config& config();
  void loadConfig(const char* explicitPath);
  [[nodiscard]] ConfigReloadResult reloadConfig();
  [[nodiscard]] const std::vector<std::filesystem::path>& configWatchPaths();
  [[nodiscard]] const std::vector<ConfigDiagnostic>& configDiagnostics();
  [[nodiscard]] const std::filesystem::path& configRootPath();
  [[nodiscard]] bool configFileMissing();
  [[nodiscard]] bool configHasMissingIncludes();
} // namespace umbriel
