// Maps a managed X11 window and eagerly reads CLIPBOARD and PRIMARY after
// XFixes ownership notifications. Cached reads and owned sources are separate:
// an unsuccessful read must never change the data this client serves.
// Commands: `state LABEL`, `read clipboard|primary`, `own clipboard|primary PAYLOAD`,
// `clear clipboard|primary`, `fullscreen true|false`, and `quit`. Only owner
// notifications and explicit `read` commands initiate transfers, never `state`.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

namespace {
  using Clock = std::chrono::steady_clock;
  constexpr auto kReadTimeout = std::chrono::seconds(5);
  constexpr size_t kMaxPendingReads = 64;
  constexpr size_t kMaxPayloadBytes = 64 * 1024;
  constexpr size_t kMaxCommandBytes = kMaxPayloadBytes + 256;
  constexpr size_t kMaxEventsPerDrain = 4096;

  struct Atoms {
    xcb_atom_t clipboard = XCB_ATOM_NONE;
    xcb_atom_t targets = XCB_ATOM_NONE;
    xcb_atom_t utf8String = XCB_ATOM_NONE;
    xcb_atom_t clipboardProperty = XCB_ATOM_NONE;
    xcb_atom_t primaryProperty = XCB_ATOM_NONE;
    xcb_atom_t netWmName = XCB_ATOM_NONE;
    xcb_atom_t netWmState = XCB_ATOM_NONE;
    xcb_atom_t netWmStateFullscreen = XCB_ATOM_NONE;
    xcb_atom_t wmProtocols = XCB_ATOM_NONE;
    xcb_atom_t wmDeleteWindow = XCB_ATOM_NONE;
  };

  struct Selection {
    std::string_view name;
    xcb_atom_t atom = XCB_ATOM_NONE;
    xcb_atom_t property = XCB_ATOM_NONE;
    uint64_t generation = 0;
    std::optional<std::string> cache;
    std::optional<std::string> source;
  };

  enum class ReadStage : uint8_t { Targets, Data };

  struct SelectionRead {
    size_t selectionIndex = 0;
    uint64_t generation = 0;
    xcb_window_t requestor = XCB_NONE;
    ReadStage stage = ReadStage::Targets;
    Clock::time_point deadline;
  };

  struct State {
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    xcb_window_t window = XCB_NONE;
    uint8_t xfixesEventBase = 0;
    Atoms atoms;
    std::array<Selection, 2> selections{{
        {.name = "clipboard", .cache = std::nullopt, .source = std::nullopt},
        {.name = "primary", .cache = std::nullopt, .source = std::nullopt},
    }};
    std::vector<SelectionRead> reads;
    std::string commands;
    bool running = true;
    bool failed = false;
  };

  void fail(State& state, std::string_view message) {
    std::println(stderr, "xwayland-selection-client: {}", message);
    state.failed = true;
    state.running = false;
  }

  bool checkRequest(State& state, xcb_void_cookie_t cookie, std::string_view operation) {
    xcb_generic_error_t* error = xcb_request_check(state.connection, cookie);
    if (error != nullptr) {
      std::println(stderr, "xwayland-selection-client: {} failed with X11 error {}", operation, error->error_code);
      std::free(error);
      state.failed = true;
      state.running = false;
      return false;
    }
    if (xcb_connection_has_error(state.connection) != 0) {
      fail(state, "X11 connection failed during request");
      return false;
    }
    return true;
  }

  xcb_atom_t internAtom(xcb_connection_t* connection, std::string_view name) {
    const auto cookie = xcb_intern_atom(connection, false, static_cast<uint16_t>(name.size()), name.data());
    auto* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (reply == nullptr) {
      return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
  }

  bool loadAtoms(State& state) {
    const std::array<std::pair<xcb_atom_t*, std::string_view>, 10> names{{
        {&state.atoms.clipboard, "CLIPBOARD"},
        {&state.atoms.targets, "TARGETS"},
        {&state.atoms.utf8String, "UTF8_STRING"},
        {&state.atoms.clipboardProperty, "_UMBRIEL_CLIPBOARD_READ"},
        {&state.atoms.primaryProperty, "_UMBRIEL_PRIMARY_READ"},
        {&state.atoms.netWmName, "_NET_WM_NAME"},
        {&state.atoms.netWmState, "_NET_WM_STATE"},
        {&state.atoms.netWmStateFullscreen, "_NET_WM_STATE_FULLSCREEN"},
        {&state.atoms.wmProtocols, "WM_PROTOCOLS"},
        {&state.atoms.wmDeleteWindow, "WM_DELETE_WINDOW"},
    }};
    for (const auto& [atom, name] : names) {
      *atom = internAtom(state.connection, name);
      if (*atom == XCB_ATOM_NONE) {
        return false;
      }
    }
    state.selections[0].atom = state.atoms.clipboard;
    state.selections[0].property = state.atoms.clipboardProperty;
    state.selections[1].atom = XCB_ATOM_PRIMARY;
    state.selections[1].property = state.atoms.primaryProperty;
    return true;
  }

  bool initializeXfixes(State& state) {
    const auto* extension = xcb_get_extension_data(state.connection, &xcb_xfixes_id);
    if (extension == nullptr || extension->present == 0) {
      fail(state, "XFixes extension is unavailable");
      return false;
    }
    state.xfixesEventBase = extension->first_event;
    const auto cookie = xcb_xfixes_query_version(state.connection, 5, 0);
    auto* reply = xcb_xfixes_query_version_reply(state.connection, cookie, nullptr);
    const bool supported = reply != nullptr && reply->major_version >= 1;
    std::free(reply);
    if (!supported) {
      fail(state, "XFixes selection notifications are unavailable");
      return false;
    }
    return true;
  }

  bool createWindow(State& state, std::string_view title) {
    state.window = xcb_generate_id(state.connection);
    constexpr uint32_t eventMask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE;
    const uint32_t values[] = {state.screen->black_pixel, eventMask};
    if (!checkRequest(
            state,
            xcb_create_window_checked(
                state.connection, XCB_COPY_FROM_PARENT, state.window, state.screen->root, 80, 80, 480, 360, 0,
                XCB_WINDOW_CLASS_INPUT_OUTPUT, state.screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values
            ),
            "creating managed window"
        )) {
      return false;
    }
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        static_cast<uint32_t>(title.size()), title.data()
    );
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, state.window, state.atoms.netWmName, state.atoms.utf8String, 8,
        static_cast<uint32_t>(title.size()), title.data()
    );
    const std::string wmClass = std::string(title) + '\0' + "UmbrielXwaylandSelection" + '\0';
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, state.window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
        static_cast<uint32_t>(wmClass.size()), wmClass.data()
    );
    xcb_change_property(
        state.connection, XCB_PROP_MODE_REPLACE, state.window, state.atoms.wmProtocols, XCB_ATOM_ATOM, 32, 1,
        &state.atoms.wmDeleteWindow
    );
    constexpr uint32_t selectionMask = XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER
        | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY
        | XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
    for (const auto& selection : state.selections) {
      if (!checkRequest(
              state,
              xcb_xfixes_select_selection_input_checked(state.connection, state.window, selection.atom, selectionMask),
              "subscribing to selection ownership"
          )) {
        return false;
      }
    }
    return checkRequest(state, xcb_map_window_checked(state.connection, state.window), "mapping managed window");
  }

  std::optional<xcb_window_t> selectionOwner(State& state, const Selection& selection) {
    const auto cookie = xcb_get_selection_owner(state.connection, selection.atom);
    auto* reply = xcb_get_selection_owner_reply(state.connection, cookie, nullptr);
    if (reply == nullptr) {
      fail(state, "querying selection owner failed");
      return std::nullopt;
    }
    const xcb_window_t owner = reply->owner;
    std::free(reply);
    return owner;
  }

  void beginRead(State& state, size_t selectionIndex, xcb_window_t owner) {
    auto& selection = state.selections[selectionIndex];
    ++selection.generation;
    selection.cache.reset();
    if (owner == XCB_NONE) {
      std::println("selection name={} none", selection.name);
      return;
    }
    if (state.reads.size() >= kMaxPendingReads) {
      fail(state, "too many outstanding selection reads");
      return;
    }
    // A separate, unmapped requestor for each generation prevents an older
    // owner's response from overwriting a new cache. Keep superseded requestors
    // alive until their reply or timeout so sources can finish normally.
    const xcb_window_t requestor = xcb_generate_id(state.connection);
    xcb_create_window(
        state.connection, 0, requestor, state.screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT, 0, nullptr
    );
    state.reads.push_back({
        .selectionIndex = selectionIndex,
        .generation = selection.generation,
        .requestor = requestor,
        .deadline = Clock::now() + kReadTimeout,
    });
    xcb_convert_selection(
        state.connection, requestor, selection.atom, state.atoms.targets, selection.property, XCB_CURRENT_TIME
    );
    xcb_flush(state.connection);
  }

  void requestRead(State& state, size_t selectionIndex) {
    const auto owner = selectionOwner(state, state.selections[selectionIndex]);
    if (owner.has_value()) {
      beginRead(state, selectionIndex, *owner);
    }
  }

  void finishRead(State& state, size_t readIndex, std::optional<std::string> payload) {
    const auto& read = state.reads[readIndex];
    auto& selection = state.selections[read.selectionIndex];
    if (read.generation == selection.generation) {
      selection.cache = std::move(payload);
      if (selection.cache.has_value()) {
        std::println("selection name={} value={}", selection.name, *selection.cache);
      } else {
        std::println("selection name={} denied", selection.name);
      }
    }
    xcb_destroy_window(state.connection, read.requestor);
    state.reads.erase(state.reads.begin() + static_cast<std::ptrdiff_t>(readIndex));
  }

  void handleSelectionNotify(State& state, const xcb_selection_notify_event_t& event) {
    const auto found = std::ranges::find(state.reads, event.requestor, &SelectionRead::requestor);
    if (found == state.reads.end()) {
      return;
    }
    const size_t readIndex = static_cast<size_t>(found - state.reads.begin());
    auto& selection = state.selections[found->selectionIndex];
    const xcb_atom_t target = found->stage == ReadStage::Targets ? state.atoms.targets : state.atoms.utf8String;
    if (event.selection != selection.atom || event.target != target) {
      return;
    }
    if (found->generation != selection.generation || event.property == XCB_ATOM_NONE) {
      finishRead(state, readIndex, std::nullopt);
      return;
    }
    if (event.property != selection.property) {
      finishRead(state, readIndex, std::nullopt);
      return;
    }
    const auto cookie = xcb_get_property(
        state.connection, true, found->requestor, selection.property, XCB_GET_PROPERTY_TYPE_ANY, 0,
        static_cast<uint32_t>(kMaxPayloadBytes / 4)
    );
    auto* reply = xcb_get_property_reply(state.connection, cookie, nullptr);
    if (reply == nullptr || reply->bytes_after != 0) {
      std::free(reply);
      finishRead(state, readIndex, std::nullopt);
      return;
    }
    const int byteLength = xcb_get_property_value_length(reply);
    if (found->stage == ReadStage::Targets) {
      bool offersUtf8 = false;
      if (reply->type == XCB_ATOM_ATOM
          && reply->format == 32
          && byteLength > 0
          && byteLength % static_cast<int>(sizeof(xcb_atom_t)) == 0) {
        const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
        const std::span targets(atoms, static_cast<size_t>(byteLength) / sizeof(xcb_atom_t));
        offersUtf8 = std::ranges::find(targets, state.atoms.utf8String) != targets.end();
      }
      std::free(reply);
      if (!offersUtf8) {
        finishRead(state, readIndex, std::nullopt);
        return;
      }
      found->stage = ReadStage::Data;
      found->deadline = Clock::now() + kReadTimeout;
      xcb_convert_selection(
          state.connection, found->requestor, selection.atom, state.atoms.utf8String, selection.property,
          XCB_CURRENT_TIME
      );
      xcb_flush(state.connection);
      return;
    }
    std::optional<std::string> payload;
    if (reply->type == state.atoms.utf8String && reply->format == 8 && byteLength >= 0) {
      payload.emplace(static_cast<const char*>(xcb_get_property_value(reply)), static_cast<size_t>(byteLength));
    }
    std::free(reply);
    finishRead(state, readIndex, std::move(payload));
  }

  Selection* findSelection(State& state, xcb_atom_t atom) {
    auto* const found = std::ranges::find(state.selections, atom, &Selection::atom);
    return found == state.selections.end() ? nullptr : &*found;
  }

  void handleSelectionRequest(State& state, const xcb_selection_request_event_t& request) {
    auto* selection = findSelection(state, request.selection);
    const xcb_atom_t property = request.property == XCB_ATOM_NONE ? request.target : request.property;
    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request.time;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = XCB_ATOM_NONE;

    if (request.owner == state.window && selection != nullptr && selection->source.has_value()) {
      std::optional<xcb_void_cookie_t> write;
      if (request.target == state.atoms.targets) {
        const std::array<xcb_atom_t, 2> targets{state.atoms.targets, state.atoms.utf8String};
        write = xcb_change_property_checked(
            state.connection, XCB_PROP_MODE_REPLACE, request.requestor, property, XCB_ATOM_ATOM, 32,
            static_cast<uint32_t>(targets.size()), targets.data()
        );
      } else if (request.target == state.atoms.utf8String) {
        write = xcb_change_property_checked(
            state.connection, XCB_PROP_MODE_REPLACE, request.requestor, property, state.atoms.utf8String, 8,
            static_cast<uint32_t>(selection->source->size()), selection->source->data()
        );
      }
      if (write.has_value()) {
        auto* error = xcb_request_check(state.connection, *write);
        if (error == nullptr) {
          notify.property = property;
        } else {
          // Requestors may disappear while a source is preparing a response.
          std::println(
              stderr, "xwayland-selection-client: serving selection failed with X11 error {}", error->error_code
          );
          std::free(error);
          return;
        }
      }
    }
    const auto cookie = xcb_send_event_checked(
        state.connection, false, request.requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify)
    );
    auto* error = xcb_request_check(state.connection, cookie);
    if (error != nullptr) {
      std::println(
          stderr, "xwayland-selection-client: sending selection reply failed with X11 error {}", error->error_code
      );
      std::free(error);
    }
  }

  void handleSelectionClear(State& state, const xcb_selection_clear_event_t& event) {
    auto* selection = findSelection(state, event.selection);
    if (selection == nullptr || event.owner != state.window) {
      return;
    }
    // A queued clear can precede a later `own` command. Do not discard a newly
    // reclaimed source just because that older notification is still buffered.
    const auto owner = selectionOwner(state, *selection);
    if (owner.has_value() && *owner != state.window) {
      selection->source.reset();
    }
  }

  void handleEvent(State& state, const xcb_generic_event_t& event) {
    const uint8_t type = event.response_type & static_cast<uint8_t>(~0x80U);
    if (type == 0) {
      const auto& error = reinterpret_cast<const xcb_generic_error_t&>(event);
      std::println(
          stderr, "xwayland-selection-client: X11 error {} on request {}:{}", error.error_code, error.major_code,
          error.minor_code
      );
      state.failed = true;
      state.running = false;
    } else if (type == state.xfixesEventBase + XCB_XFIXES_SELECTION_NOTIFY) {
      const auto& notify = reinterpret_cast<const xcb_xfixes_selection_notify_event_t&>(event);
      auto* const found = std::ranges::find(state.selections, notify.selection, &Selection::atom);
      if (notify.window == state.window && found != state.selections.end()) {
        beginRead(state, static_cast<size_t>(found - state.selections.begin()), notify.owner);
      }
    } else if (type == XCB_SELECTION_NOTIFY) {
      handleSelectionNotify(state, reinterpret_cast<const xcb_selection_notify_event_t&>(event));
    } else if (type == XCB_SELECTION_REQUEST) {
      handleSelectionRequest(state, reinterpret_cast<const xcb_selection_request_event_t&>(event));
    } else if (type == XCB_SELECTION_CLEAR) {
      handleSelectionClear(state, reinterpret_cast<const xcb_selection_clear_event_t&>(event));
    } else if (type == XCB_CLIENT_MESSAGE) {
      const auto& message = reinterpret_cast<const xcb_client_message_event_t&>(event);
      if (message.window == state.window
          && message.type == state.atoms.wmProtocols
          && message.format == 32
          && message.data.data32[0] == state.atoms.wmDeleteWindow) {
        state.running = false;
      }
    } else if (type == XCB_DESTROY_NOTIFY) {
      const auto& destroy = reinterpret_cast<const xcb_destroy_notify_event_t&>(event);
      if (destroy.window == state.window) {
        state.window = XCB_NONE;
        state.running = false;
      }
    }
  }

  void drainEvents(State& state) {
    for (size_t count = 0; state.running && count < kMaxEventsPerDrain; ++count) {
      auto* event = xcb_poll_for_event(state.connection);
      if (event == nullptr) {
        return;
      }
      handleEvent(state, *event);
      std::free(event);
    }
    if (state.running) {
      fail(state, "X11 event processing limit exceeded");
    }
  }

  void expireReads(State& state) {
    const auto now = Clock::now();
    size_t index = 0;
    while (index < state.reads.size()) {
      if (state.reads[index].deadline <= now) {
        finishRead(state, index, std::nullopt);
      } else {
        ++index;
      }
    }
  }

  std::string_view cachedPayload(const Selection& selection) {
    return selection.cache.has_value() ? std::string_view(*selection.cache) : "<none>";
  }

  void reportState(State& state, std::string_view label) {
    const auto cookie = xcb_get_input_focus(state.connection);
    auto* reply = xcb_get_input_focus_reply(state.connection, cookie, nullptr);
    if (reply == nullptr) {
      fail(state, "state round trip failed");
      return;
    }
    std::free(reply);
    // Synchronous replies can pull events into XCB's userspace queue without
    // leaving the connection fd readable. Drain that queue before reporting.
    drainEvents(state);
    expireReads(state);
    if (state.running) {
      std::println(
          "cache label={} clipboard={} primary={}", label, cachedPayload(state.selections[0]),
          cachedPayload(state.selections[1])
      );
    }
  }

  void requestFullscreen(State& state, bool fullscreen) {
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = state.window;
    event.type = state.atoms.netWmState;
    event.data.data32[0] = fullscreen ? 1 : 0;
    event.data.data32[1] = state.atoms.netWmStateFullscreen;
    event.data.data32[3] = 1;
    xcb_send_event(
        state.connection, false, state.screen->root,
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, reinterpret_cast<const char*>(&event)
    );
    xcb_flush(state.connection);
    std::println("fullscreen-requested={}", fullscreen);
  }

  void ownSelection(State& state, size_t selectionIndex, std::string_view payload) {
    auto& selection = state.selections[selectionIndex];
    if (payload.size() > kMaxPayloadBytes) {
      fail(state, "owned payload exceeds small-transfer limit");
      return;
    }
    selection.source.emplace(payload);
    xcb_set_selection_owner(state.connection, state.window, selection.atom, XCB_CURRENT_TIME);
    const auto owner = selectionOwner(state, selection);
    if (owner.has_value() && *owner != state.window) {
      fail(state, "failed to acquire selection ownership");
    } else if (owner.has_value()) {
      std::println("own name={} value={}", selection.name, payload);
    }
  }

  void clearSelection(State& state, size_t selectionIndex) {
    auto& selection = state.selections[selectionIndex];
    selection.source.reset();
    xcb_set_selection_owner(state.connection, XCB_NONE, selection.atom, XCB_CURRENT_TIME);
    const auto owner = selectionOwner(state, selection);
    if (owner.has_value() && *owner != XCB_NONE) {
      fail(state, "failed to clear selection ownership");
    }
  }

  void handleCommand(State& state, std::string_view command) {
    constexpr std::string_view statePrefix = "state ";
    if (command.starts_with(statePrefix) && command.size() > statePrefix.size()) {
      reportState(state, command.substr(statePrefix.size()));
      return;
    }
    if (command == "fullscreen true" || command == "fullscreen false") {
      requestFullscreen(state, command == "fullscreen true");
      return;
    }
    if (command == "quit") {
      state.running = false;
      return;
    }
    if (command.empty()) {
      return;
    }
    for (size_t index = 0; index < state.selections.size(); ++index) {
      const auto name = state.selections[index].name;
      if (command.starts_with("read ") && command.substr(5) == name) {
        requestRead(state, index);
        return;
      }
      if (command.starts_with("clear ") && command.substr(6) == name) {
        clearSelection(state, index);
        return;
      }
      if (command.starts_with("own ")) {
        const auto arguments = command.substr(4);
        if (arguments.starts_with(name) && arguments.size() > name.size() && arguments[name.size()] == ' ') {
          ownSelection(state, index, arguments.substr(name.size() + 1));
          return;
        }
      }
    }
    std::println(stderr, "xwayland-selection-client: unknown command '{}'", command);
    state.failed = true;
    state.running = false;
  }

  void readCommands(State& state) {
    std::array<char, 4096> buffer{};
    const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        std::println(stderr, "xwayland-selection-client: stdin read failed: {}", std::strerror(errno));
        state.failed = true;
        state.running = false;
      }
      return;
    }
    if (count == 0) {
      if (!state.commands.empty()) {
        handleCommand(state, state.commands);
      }
      state.running = false;
      return;
    }
    state.commands.append(buffer.data(), static_cast<size_t>(count));
    size_t newline = std::string::npos;
    while (state.running && (newline = state.commands.find('\n')) != std::string::npos) {
      if (newline > kMaxCommandBytes) {
        fail(state, "command exceeds size limit");
        return;
      }
      handleCommand(state, std::string_view(state.commands).substr(0, newline));
      state.commands.erase(0, newline + 1);
    }
    if (state.commands.size() > kMaxCommandBytes) {
      fail(state, "command exceeds size limit");
    }
  }

  int pollTimeout(const State& state) {
    auto timeout = std::chrono::milliseconds(1000);
    const auto now = Clock::now();
    for (const auto& read : state.reads) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(read.deadline - now);
      timeout = std::min(timeout, remaining);
    }
    return static_cast<int>(std::max(timeout.count(), decltype(timeout.count()){0}));
  }

  void destroyWindows(State& state) {
    for (const auto& read : state.reads) {
      xcb_destroy_window(state.connection, read.requestor);
    }
    if (state.window != XCB_NONE) {
      xcb_destroy_window(state.connection, state.window);
    }
    xcb_flush(state.connection);
  }
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::println(stderr, "usage: xwayland-selection-client TITLE");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  State state;
  int screenNumber = 0;
  state.connection = xcb_connect(nullptr, &screenNumber);
  if (state.connection == nullptr || xcb_connection_has_error(state.connection) != 0) {
    std::println(stderr, "xwayland-selection-client: failed to connect to DISPLAY");
    if (state.connection != nullptr) {
      xcb_disconnect(state.connection);
    }
    return 1;
  }
  const auto* setup = xcb_get_setup(state.connection);
  auto screens = xcb_setup_roots_iterator(setup);
  for (int index = 0; index < screenNumber && screens.rem != 0; ++index) {
    xcb_screen_next(&screens);
  }
  state.screen = screens.rem != 0 ? screens.data : nullptr;
  state.reads.reserve(kMaxPendingReads);
  if (state.screen == nullptr || !loadAtoms(state) || !initializeXfixes(state) || !createWindow(state, argv[1])) {
    std::println(stderr, "xwayland-selection-client: failed to initialize window and selections");
    destroyWindows(state);
    xcb_disconnect(state.connection);
    return 1;
  }
  for (size_t index = 0; index < state.selections.size() && state.running; ++index) {
    requestRead(state, index);
  }
  xcb_flush(state.connection);
  if (state.running) {
    std::println("ready window=0x{:x}", state.window);
  }

  const int xcbFd = xcb_get_file_descriptor(state.connection);
  while (state.running && xcb_connection_has_error(state.connection) == 0) {
    // Always drain before polling, including events buffered by a synchronous
    // property/owner reply or by the preceding stdin command.
    drainEvents(state);
    expireReads(state);
    xcb_flush(state.connection);
    if (!state.running || xcb_connection_has_error(state.connection) != 0) {
      break;
    }
    std::array<pollfd, 2> ready{{
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = xcbFd, .events = POLLIN, .revents = 0},
    }};
    const int result = poll(ready.data(), ready.size(), pollTimeout(state));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::println(stderr, "xwayland-selection-client: poll failed: {}", std::strerror(errno));
      state.failed = true;
      break;
    }
    if ((ready[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      fail(state, "X11 connection closed");
      break;
    }
    drainEvents(state);
    if (state.running && (ready[0].revents & (POLLERR | POLLNVAL)) != 0) {
      fail(state, "stdin polling failed");
    } else if (state.running && (ready[0].revents & (POLLIN | POLLHUP)) != 0) {
      readCommands(state);
    }
  }
  if (xcb_connection_has_error(state.connection) != 0) {
    std::println(
        stderr, "xwayland-selection-client: X11 connection failed: {}", xcb_connection_has_error(state.connection)
    );
    state.failed = true;
  }
  destroyWindows(state);
  xcb_disconnect(state.connection);
  return state.failed ? 1 : 0;
}
