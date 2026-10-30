#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}

struct wl_client;
struct wl_global;
struct wlr_output;
struct wlr_output_layout;

namespace umbriel {

  // How one output's part of the layout maps into X coordinates. X coordinates are `scale` times finer than layout
  // coordinates: 1, or the output's scale when X11 clients see outputs at their physical resolution.
  struct XwaylandRegion {
    wlr_box layout{};
    wlr_box x{};
    double scale = 1.0;

    [[nodiscard]] int toX(int layoutX) const;
    [[nodiscard]] int toY(int layoutY) const;
    [[nodiscard]] int toLayoutX(int x) const;
    [[nodiscard]] int toLayoutY(int y) const;
    // Lengths spanning the whole output map exactly onto its size on the other side, whatever the rounding.
    [[nodiscard]] int toXWidth(int width) const;
    [[nodiscard]] int toXHeight(int height) const;
    [[nodiscard]] int toLayoutWidth(int width) const;
    [[nodiscard]] int toLayoutHeight(int height) const;

    bool operator==(const XwaylandRegion&) const;
  };

  // The X11 screen as Xwayland sees it. Xwayland learns where outputs are from xdg-output, and its root window only
  // spans non-negative coordinates, so an output left of or above the layout origin would fall outside the X screen.
  // Xwayland therefore gets its own xdg-output global, in which the layout's top-left corner is the X origin. At native
  // resolution each output is published at its physical size, and output positions are scaled by the largest output
  // scale, so outputs keep their arrangement and never overlap. Every X coordinate the compositor sends or receives
  // goes through an XwaylandRegion.
  class XwaylandOutputs {
  public:
    // The scale X11 clients are asked to render each output at — its render
    // scale, resolved through the shell's output binding.
    using RenderScale = std::function<double(const wlr_output*)>;
    // Empty `renderScale` publishes each output's own scale.
    XwaylandOutputs(
        wl_display* display, wlr_output_layout* layout, bool nativeResolution, RenderScale renderScale = {}
    );
    ~XwaylandOutputs();

    XwaylandOutputs(const XwaylandOutputs&) = delete;
    XwaylandOutputs& operator=(const XwaylandOutputs&) = delete;

    // Whether an xdg-output manager `global` may be advertised to `client`: this one only to Xwayland, wlroots' one to
    // everyone else.
    [[nodiscard]] bool advertise(const wl_client* client, const wl_global* global, const wl_client* xwayland) const;
    // Recomputes the X screen from the output layout and republishes the outputs whose X geometry changed. Returns
    // true when the mapping between layout and X coordinates changed.
    bool update();
    // Resolve a connector or monitor identity against the live layout, including outputs still being constructed.
    // An empty selector leaves RandR unmanaged; a missing monitor resolves to an empty name.
    [[nodiscard]] std::optional<std::string> primaryOutput(std::string_view name) const;

    // The region of `output`. Without one, the output nearest the layout point stands in.
    [[nodiscard]] XwaylandRegion region(const wlr_output* output, int layoutX, int layoutY) const;
    // The region of the output at, or else nearest to, an X point.
    [[nodiscard]] XwaylandRegion regionAtX(int x, int y) const;
    // The largest output scale in X pixels per layout unit: 1 unless outputs are at their physical resolution.
    [[nodiscard]] double scale() const { return m_scale; }

  private:
    struct Entry {
      XwaylandOutputs* owner = nullptr;
      wl_resource* resource = nullptr;
      wlr_output* output = nullptr;
      wlr_box sent{};
      wl_listener outputDestroy{};
    };

    struct OutputRegion {
      const wlr_output* output = nullptr;
      XwaylandRegion region;
      bool operator==(const OutputRegion&) const = default;
    };

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleGetXdgOutput(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* outputResource);
    static void handleResourceDestroy(wl_resource* resource);
    static void onOutputDestroy(wl_listener* listener, void* data);
    // The output's rectangle in X coordinates, empty when it is not in the layout.
    [[nodiscard]] wlr_box xBox(const wlr_output* output) const;
    // Sends the geometry when it changed; `initial` also sends the one-time name and description.
    void send(Entry& entry, bool initial);
    void forget(Entry* entry);

    wlr_output_layout* m_layout = nullptr;
    wl_global* m_global = nullptr;
    bool m_nativeResolution = false;
    RenderScale m_renderScale;
    double m_scale = 1.0;
    // Stands in when no output is in the layout: a plain translation to the X origin.
    XwaylandRegion m_fallback;
    std::vector<OutputRegion> m_regions;
    std::vector<std::unique_ptr<Entry>> m_entries;
  };

} // namespace umbriel
