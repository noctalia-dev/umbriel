#include "server/action_request.h"

#include "config/window_position.h"
#include "view/window_region.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    constexpr std::string_view kCmdField = "cmd";
    constexpr std::string_view kActionField = "action";

    // A JSON integer within [min, max]. Unsigned JSON integers are bounded before conversion so a huge value cannot
    // wrap into range.
    std::optional<long long> boundedInteger(const nlohmann::json& value, long long min, long long max) {
      if (value.is_number_unsigned()) {
        const auto unsignedValue = value.get<unsigned long long>();
        if (std::cmp_less(unsignedValue, min) || std::cmp_greater(unsignedValue, max)) {
          return std::nullopt;
        }
        return static_cast<long long>(unsignedValue);
      }
      const auto signedValue = value.get<long long>();
      if (signedValue < min || signedValue > max) {
        return std::nullopt;
      }
      return signedValue;
    }

    // Reads typed fields from one request, recording the first problem in `error`.
    class Fields {
    public:
      Fields(const nlohmann::json& request, std::string& error) : m_request(request), m_error(error) {}

      // Every field other than cmd and action must be one the action's argument kind declares.
      bool only(std::initializer_list<std::string_view> allowed) {
        for (const auto& [key, value] : m_request.items()) {
          if (key != kCmdField && key != kActionField && std::ranges::find(allowed, key) == allowed.end()) {
            return fail(std::format("unknown field \"{}\"", key));
          }
        }
        return true;
      }

      [[nodiscard]] bool has(std::string_view key) const { return m_request.contains(key); }

      // A present string field must be non-empty. An absent optional field leaves `output` empty.
      bool string(std::string_view key, bool required, std::string& output) {
        const auto field = m_request.find(key);
        if (field == m_request.end()) {
          return required ? fail(std::format("missing \"{}\"", key)) : true;
        }
        if (!field->is_string() || field->get_ref<const std::string&>().empty()) {
          return fail(std::format("\"{}\" must be a non-empty string", key));
        }
        output = field->get<std::string>();
        return true;
      }

      bool number(std::string_view key, double& output) {
        const auto field = m_request.find(key);
        if (field == m_request.end()) {
          return fail(std::format("missing \"{}\"", key));
        }
        if (!field->is_number()) {
          return fail(std::format("\"{}\" must be a number", key));
        }
        output = field->get<double>();
        return true;
      }

      bool integer(std::string_view key, int& output) {
        const auto field = m_request.find(key);
        if (field == m_request.end()) {
          return fail(std::format("missing \"{}\"", key));
        }
        if (!field->is_number_integer()) {
          return fail(std::format("\"{}\" must be an integer", key));
        }
        const auto value = boundedInteger(*field, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        if (!value) {
          return fail(std::format("\"{}\" is out of range", key));
        }
        output = static_cast<int>(*value);
        return true;
      }

      bool boolean(std::string_view key, bool& output) {
        const auto field = m_request.find(key);
        if (field == m_request.end()) {
          return true;
        }
        if (!field->is_boolean()) {
          return fail(std::format("\"{}\" must be a boolean", key));
        }
        output = field->get<bool>();
        return true;
      }

      [[nodiscard]] const nlohmann::json* raw(std::string_view key) const {
        const auto field = m_request.find(key);
        return field == m_request.end() ? nullptr : &*field;
      }

      bool fail(std::string message) {
        m_error = std::move(message);
        return false;
      }

    private:
      const nlohmann::json& m_request;
      std::string& m_error;
    };

    std::optional<WorkspaceArg> workspacePayload(Fields& fields) {
      WorkspaceArg workspace;
      if (!fields.string("output", false, workspace.output)) {
        return std::nullopt;
      }
      const nlohmann::json* selector = fields.raw("workspace");
      if (selector == nullptr) {
        fields.fail("missing \"workspace\"");
        return std::nullopt;
      }
      if (selector->is_number_integer()) {
        const auto index = boundedInteger(*selector, 1, static_cast<long long>(kMaxWorkspaces));
        if (!index) {
          fields.fail(std::format("\"workspace\" position must be 1 to {}", kMaxWorkspaces));
          return std::nullopt;
        }
        workspace.reference = WorkspaceIndex{static_cast<size_t>(*index)};
        return workspace;
      }
      if (!selector->is_string() || selector->get_ref<const std::string&>().empty()) {
        fields.fail("\"workspace\" must be a position number or a non-empty name");
        return std::nullopt;
      }
      workspace.reference = WorkspaceName{selector->get<std::string>()};
      return workspace;
    }

    std::optional<KeybindPayload> effectPayload(ActionArgKind kind, Fields& fields) {
      const bool cycle = kind == ActionArgKind::WindowEffectCycle
          || kind == ActionArgKind::ScreenEffectCycle
          || kind == ActionArgKind::CursorEffectCycle;
      const bool cursor = kind == ActionArgKind::CursorEffectSet || kind == ActionArgKind::CursorEffectCycle;
      const bool screen = kind == ActionArgKind::ScreenEffectSet || kind == ActionArgKind::ScreenEffectCycle;
      const std::string_view nameField = cycle ? "pool" : "effect";
      const std::string_view targetField = screen ? "output" : "window";
      const bool allowed = cursor ? fields.only({nameField}) : fields.only({nameField, targetField});
      std::string name;
      std::string target;
      if (!allowed || !fields.string(nameField, !cycle, name) || !fields.string(targetField, false, target)) {
        return std::nullopt;
      }
      if (cycle && name == kEffectOff) {
        fields.fail("\"pool\" cannot be off");
        return std::nullopt;
      }
      if (cursor) {
        return EffectCursorArg{.name = std::move(name)};
      }
      if (screen) {
        return EffectScreenArg{.name = std::move(name), .output = std::move(target)};
      }
      return EffectWindowArg{.name = std::move(name), .id = std::move(target)};
    }

    std::optional<KeybindPayload> layoutPayload(Fields& fields) {
      std::string layout;
      if (!fields.only({"layout", "cycle"}) || !fields.string("layout", true, layout)) {
        return std::nullopt;
      }
      if (const auto mode = layoutModeNamed(layout)) {
        if (fields.has("cycle")) {
          fields.fail(R"("cycle" needs "layout": "toggle")");
          return std::nullopt;
        }
        return LayoutModeArg{.mode = mode, .toggleSequence = {}};
      }
      if (layout != "toggle") {
        fields.fail(std::format("unknown layout \"{}\"", layout));
        return std::nullopt;
      }
      const nlohmann::json* cycle = fields.raw("cycle");
      if (cycle == nullptr) {
        return LayoutModeArg{};
      }
      std::vector<LayoutMode> sequence;
      if (cycle->is_array()) {
        for (const auto& entry : *cycle) {
          const auto mode = entry.is_string() ? layoutModeNamed(entry.get_ref<const std::string&>()) : std::nullopt;
          if (!mode || std::ranges::contains(sequence, *mode)) {
            sequence.clear();
            break;
          }
          sequence.push_back(*mode);
        }
      }
      if (sequence.size() < 2) {
        fields.fail("\"cycle\" must list two or more distinct layouts");
        return std::nullopt;
      }
      return LayoutModeArg{.mode = std::nullopt, .toggleSequence = std::move(sequence)};
    }

    std::optional<KeybindPayload> payloadFor(const ActionSpec& spec, Fields& fields) {
      switch (spec.argKind) {
      case ActionArgKind::None:
        if (!fields.only({})) {
          return std::nullopt;
        }
        return std::monostate{};
      case ActionArgKind::Command: {
        SpawnArg arg;
        if (!fields.only({"command"}) || !fields.string("command", true, arg.command)) {
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::Submap: {
        SubmapArg arg;
        if (!fields.only({"submap"}) || !fields.string("submap", true, arg.name)) {
          return std::nullopt;
        }
        if (!validSubmapName(arg.name)) {
          fields.fail("\"submap\" may not contain ']' or be 'disable'");
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::Fraction: {
        FractionArg arg;
        if (!fields.only({"fraction"}) || !fields.number("fraction", arg.fraction)) {
          return std::nullopt;
        }
        if (!validActionFraction(arg.fraction)) {
          fields.fail(std::format("\"fraction\" must be 0.1 to {}", kMaxWideFraction));
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::FractionDelta: {
        FractionArg arg;
        if (!fields.only({"delta"}) || !fields.number("delta", arg.fraction)) {
          return std::nullopt;
        }
        if (!validActionFractionDelta(arg.fraction)) {
          fields.fail("\"delta\" must be non-zero and within -0.9 to 0.9");
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::Workspace: {
        auto workspace = fields.only({"workspace", "output"}) ? workspacePayload(fields) : std::nullopt;
        return workspace ? std::make_optional<KeybindPayload>(std::move(*workspace)) : std::nullopt;
      }
      case ActionArgKind::WorkspaceMoveToOutput: {
        WorkspaceMoveToOutputArg arg;
        if (!fields.only({"destination", "workspace", "output"})
            || !fields.string("destination", true, arg.destination)) {
          return std::nullopt;
        }
        if (!fields.has("workspace") && !fields.has("output")) {
          return arg;
        }
        arg.workspace = workspacePayload(fields);
        return arg.workspace ? std::make_optional<KeybindPayload>(std::move(arg)) : std::nullopt;
      }
      case ActionArgKind::WindowEffectSet:
      case ActionArgKind::WindowEffectCycle:
      case ActionArgKind::ScreenEffectSet:
      case ActionArgKind::ScreenEffectCycle:
      case ActionArgKind::CursorEffectSet:
      case ActionArgKind::CursorEffectCycle:
        return effectPayload(spec.argKind, fields);
      case ActionArgKind::Output:
      case ActionArgKind::OptionalOutput: {
        OutputArg arg;
        if (!fields.only({"output"}) || !fields.string("output", spec.argKind == ActionArgKind::Output, arg.output)) {
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::OptionalScratchpad: {
        ScratchpadArg arg;
        if (!fields.only({"scratchpad"}) || !fields.string("scratchpad", false, arg.name)) {
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::WindowId:
      case ActionArgKind::OptionalWindowId: {
        WindowIdArg arg;
        if (!fields.only({"window"}) || !fields.string("window", spec.argKind == ActionArgKind::WindowId, arg.id)) {
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::WindowRegion: {
        std::string region;
        if (!fields.only({"region"}) || !fields.string("region", true, region)) {
          return std::nullopt;
        }
        const auto named = windowRegionNamed(region);
        if (!named) {
          fields.fail(std::format("unknown region \"{}\"", region));
          return std::nullopt;
        }
        return WindowRegionArg{.region = *named};
      }
      case ActionArgKind::ColumnDisplay: {
        std::string display;
        if (!fields.only({"display"}) || !fields.string("display", true, display)) {
          return std::nullopt;
        }
        if (display != "normal" && display != "tabbed") {
          fields.fail("\"display\" must be normal or tabbed");
          return std::nullopt;
        }
        return ColumnDisplayArg{.display = display == "tabbed" ? ColumnDisplay::Tabbed : ColumnDisplay::Normal};
      }
      case ActionArgKind::TabIndex: {
        TabIndexArg arg;
        if (!fields.only({"index"}) || !fields.integer("index", arg.index)) {
          return std::nullopt;
        }
        if (arg.index == 0) {
          fields.fail("\"index\" must not be 0");
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::SkipConfirmation: {
        QuitArg arg;
        if (!fields.only({"skip_confirmation"}) || !fields.boolean("skip_confirmation", arg.skipConfirmation)) {
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::SdrWhite: {
        SdrWhiteArg arg;
        if (!fields.only({"nits", "output"})
            || !fields.number("nits", arg.nits)
            || !fields.string("output", false, arg.output)) {
          return std::nullopt;
        }
        if (!validSdrWhiteNits(arg.nits)) {
          fields.fail("\"nits\" must be 80 to 1000");
          return std::nullopt;
        }
        return arg;
      }
      case ActionArgKind::LayoutMode:
        return layoutPayload(fields);
      case ActionArgKind::WindowMoveTo: {
        WindowMoveToArg arg;
        std::string anchor;
        if (!fields.only({"anchor", "x", "y", "window"})
            || !fields.string("anchor", true, anchor)
            || !fields.integer("x", arg.position.x)
            || !fields.integer("y", arg.position.y)
            || !fields.string("window", false, arg.id)) {
          return std::nullopt;
        }
        const auto named = windowPositionAnchorFromName(anchor);
        if (!named) {
          fields.fail(std::format("unknown anchor \"{}\"", anchor));
          return std::nullopt;
        }
        arg.position.anchor = *named;
        return arg;
      }
      case ActionArgKind::WindowMoveBy: {
        WindowMoveByArg arg;
        if (!fields.only({"dx", "dy", "window"})
            || !fields.integer("dx", arg.deltaX)
            || !fields.integer("dy", arg.deltaY)
            || !fields.string("window", false, arg.id)) {
          return std::nullopt;
        }
        return arg;
      }
      }
      fields.fail("unsupported argument kind");
      return std::nullopt;
    }

    void putIfSet(nlohmann::json& request, const char* key, const std::string& value) {
      if (!value.empty()) {
        request[key] = value;
      }
    }

    void putWorkspace(nlohmann::json& request, const WorkspaceArg& workspace) {
      if (const auto* index = std::get_if<WorkspaceIndex>(&workspace.reference)) {
        request["workspace"] = index->value;
      } else {
        request["workspace"] = std::get<WorkspaceName>(workspace.reference).value;
      }
      putIfSet(request, "output", workspace.output);
    }

  } // namespace

  std::optional<Keybind> actionFromRequest(const nlohmann::json& request, std::string& error) {
    const auto name = request.find(kActionField);
    if (!request.is_object() || name == request.end() || !name->is_string()) {
      error = "missing \"action\"";
      return std::nullopt;
    }
    const auto& actionName = name->get_ref<const std::string&>();
    const ActionSpec* spec = findActionSpec(actionName);
    if (spec == nullptr) {
      error = "unknown action: " + actionName;
      return std::nullopt;
    }
    std::string detail;
    Fields fields(request, detail);
    auto payload = payloadFor(*spec, fields);
    if (!payload) {
      error = std::format("invalid arguments for {}: {}", actionName, detail);
      return std::nullopt;
    }
    Keybind bind;
    bind.action = spec->action;
    bind.payload = std::move(*payload);
    return bind;
  }

  nlohmann::json actionRequest(const Keybind& bind) {
    const auto specs = actionSpecs();
    const auto spec = std::ranges::find(specs, bind.action, &ActionSpec::action);
    nlohmann::json request{{"cmd", "action"}};
    if (spec == specs.end()) {
      return request;
    }
    request[kActionField] = spec->name;
    std::visit(
        [&](const auto& arg) {
          using Arg = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<Arg, SpawnArg>) {
            request["command"] = arg.command;
          } else if constexpr (std::is_same_v<Arg, SubmapArg>) {
            request["submap"] = arg.name;
          } else if constexpr (std::is_same_v<Arg, FractionArg>) {
            request[spec->argKind == ActionArgKind::FractionDelta ? "delta" : "fraction"] = arg.fraction;
          } else if constexpr (std::is_same_v<Arg, WorkspaceArg>) {
            putWorkspace(request, arg);
          } else if constexpr (std::is_same_v<Arg, WorkspaceMoveToOutputArg>) {
            request["destination"] = arg.destination;
            if (arg.workspace) {
              putWorkspace(request, *arg.workspace);
            }
          } else if constexpr (std::is_same_v<Arg, OutputArg>) {
            putIfSet(request, "output", arg.output);
          } else if constexpr (std::is_same_v<Arg, ScratchpadArg>) {
            putIfSet(request, "scratchpad", arg.name);
          } else if constexpr (std::is_same_v<Arg, WindowIdArg>) {
            putIfSet(request, "window", arg.id);
          } else if constexpr (std::is_same_v<Arg, LayoutModeArg>) {
            if (arg.mode) {
              request["layout"] = layoutModeName(*arg.mode);
            } else {
              request["layout"] = "toggle";
              if (!arg.toggleSequence.empty()) {
                nlohmann::json cycle = nlohmann::json::array();
                for (const LayoutMode mode : arg.toggleSequence) {
                  cycle.push_back(layoutModeName(mode));
                }
                request["cycle"] = std::move(cycle);
              }
            }
          } else if constexpr (std::is_same_v<Arg, ColumnDisplayArg>) {
            request["display"] = arg.display == ColumnDisplay::Tabbed ? "tabbed" : "normal";
          } else if constexpr (std::is_same_v<Arg, TabIndexArg>) {
            request["index"] = arg.index;
          } else if constexpr (std::is_same_v<Arg, QuitArg>) {
            if (arg.skipConfirmation) {
              request["skip_confirmation"] = true;
            }
          } else if constexpr (
              std::is_same_v<Arg, EffectWindowArg>
              || std::is_same_v<Arg, EffectScreenArg>
              || std::is_same_v<Arg, EffectCursorArg>
          ) {
            const bool cycle = spec->argKind == ActionArgKind::WindowEffectCycle
                || spec->argKind == ActionArgKind::ScreenEffectCycle
                || spec->argKind == ActionArgKind::CursorEffectCycle;
            putIfSet(request, cycle ? "pool" : "effect", arg.name);
            if constexpr (std::is_same_v<Arg, EffectWindowArg>) {
              putIfSet(request, "window", arg.id);
            } else if constexpr (std::is_same_v<Arg, EffectScreenArg>) {
              putIfSet(request, "output", arg.output);
            }
          } else if constexpr (std::is_same_v<Arg, SdrWhiteArg>) {
            request["nits"] = arg.nits;
            putIfSet(request, "output", arg.output);
          } else if constexpr (std::is_same_v<Arg, WindowMoveToArg>) {
            request["anchor"] = windowPositionAnchorName(arg.position.anchor);
            request["x"] = arg.position.x;
            request["y"] = arg.position.y;
            putIfSet(request, "window", arg.id);
          } else if constexpr (std::is_same_v<Arg, WindowMoveByArg>) {
            request["dx"] = arg.deltaX;
            request["dy"] = arg.deltaY;
            putIfSet(request, "window", arg.id);
          } else if constexpr (std::is_same_v<Arg, WindowRegionArg>) {
            request["region"] = windowRegionName(arg.region);
          }
        },
        bind.payload
    );
    return request;
  }

} // namespace umbriel
