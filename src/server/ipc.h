#pragma once

#include "server/event_pacer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_source;

namespace umbriel {

  class Output;
  class Server;

  class Ipc {
  public:
    // One bit per subscribable event name; the subscribe request ORs the bits it asked for into the connection. Keep
    // the names below in sync with the event builders in ipc.cpp.
    enum : uint16_t {
      EventTheme = 1 << 0,
      EventOverview = 1 << 1,
      EventKeyboardLayout = 1 << 2,
      EventWindows = 1 << 3,
      EventWorkspaces = 1 << 4,
      EventSubmap = 1 << 5,
      EventScreenCast = 1 << 6,
      EventLayers = 1 << 7,
      EventOutputs = 1 << 8,
    };
    // Number of bits above; sizes the last-broadcast cache.
    static constexpr size_t kEventCount = 9;
    // Subscribable names in bit order: the subscribe handler matches against this table and `umbriel subscribe`
    // lists it, so a new family cannot be accepted by one and unknown to the other.
    static constexpr std::array<std::string_view, kEventCount> kEventNames = {
        "theme", "overview", "keyboard_layout", "windows", "workspaces", "submap", "screencast", "layers", "outputs"
    };

    Ipc(Server& server, const std::string& waylandSocketName);
    ~Ipc();

    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

    void notifyThemeChanged();
    void notifyOverviewChanged();
    void notifyKeyboardLayoutChanged();
    void notifySubmapChanged();
    void notifyScreenCastChanged();
    // Paced: these families can change on every pointer motion or layer commit, and each event is a full snapshot.
    void notifyWindowsChanged() { m_windowsPacer.schedule(); }
    void notifyWorkspacesChanged() { m_workspacesPacer.schedule(); }
    void notifyLayersChanged() { m_layersPacer.schedule(); }
    void notifyOutputsChanged() { m_outputsPacer.schedule(); }
#ifdef UMBRIEL_HARNESS
    void setEventPacingHeld(bool held);
#endif
    // End of an output frame: answers frame waits (settle, clock-advance) once every output has drawn a frame.
    void notifyOutputFrame(const Output& output);
    // A window committed. A window off screen draws no frame when it commits its configure, so while a settle waits,
    // this asks every output for the frame that checks it again.
    void notifyWindowCommitted();

  private:
    enum class FrameWait : uint8_t { None, Drawn, Settled };

    struct Connection {
      Ipc* owner = nullptr;
      int fd = -1;
      std::string input;
      std::string output;
      size_t writeOffset = 0;
      wl_event_source* fdSource = nullptr;
      wl_event_source* deadline = nullptr;
      bool responding = false;
      bool screenCastActive = false;
      uint16_t subscribedEvents = 0;
      // A frame wait holds its reply until these outputs have each drawn a frame, and for settle until the server is
      // settled as well.
      FrameWait frameWait = FrameWait::None;
      std::vector<std::string> waitOutputs;
      std::string waitReply;
    };

    static int onListenReadable(int fd, uint32_t mask, void* data);
    static int onConnectionEvent(int fd, uint32_t mask, void* data);
    static int onConnectionTimeout(void* data);

    void acceptConnections();
    void addConnection(int clientFd);
    bool readRequest(Connection& connection);
    bool writeResponse(Connection& connection);
    void prepareResponse(Connection& connection, std::string response);
    void removeConnection(Connection* connection);
    static void closeConnection(Connection& connection);
    // Empty when the reply is deferred, as for settle.
    std::optional<std::string> handleRequest(Connection& connection, std::string_view line);
    std::optional<std::string>
    dispatchRequest(Connection& connection, const nlohmann::json& request, size_t requestSize);
    void beginFrameWait(Connection& connection, FrameWait wait, std::string reply);
    void finishFrameWait(Connection& connection, std::string response);
    EventPacer snapshotPacer(uint16_t event, nlohmann::json (*buildEvent)(Server&));
    std::string& lastBroadcast(uint16_t event);
    void broadcastEvent(uint16_t event, const nlohmann::json& payload);
    void refreshScreenCastActive();
    void dropAudioOwner();

    Server* m_server;
    std::string m_socketPath;
    Connection* m_audio = nullptr;
    int m_listenFd = -1;
    wl_event_source* m_eventSource = nullptr;
    std::vector<std::unique_ptr<Connection>> m_connections;
    // Last payload broadcast per family, indexed by the bit position of the event. A family that recomputes to the
    // same JSON is dropped rather than woken through to every subscriber; a fresh subscriber gets its own initial
    // line from the subscribe handler, which forgets a paced family's entry when that line is newer, so skipping a
    // no-op broadcast never leaves anyone behind.
    std::array<std::string, kEventCount> m_lastBroadcast;
    EventPacer m_windowsPacer;
    EventPacer m_workspacesPacer;
    EventPacer m_layersPacer;
    EventPacer m_outputsPacer;
  };

} // namespace umbriel
