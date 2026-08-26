#include "core/dirty.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    // Headless outputs are created at the size the backend gives the ones from WLR_HEADLESS_OUTPUTS.
    constexpr unsigned int kHeadlessWidth = 1280;
    constexpr unsigned int kHeadlessHeight = 720;

    wlr_backend* headlessBackend(wlr_backend* backend) {
      if (backend == nullptr || wlr_backend_is_headless(backend)) {
        return backend;
      }
      if (!wlr_backend_is_multi(backend)) {
        return nullptr;
      }
      wlr_backend* headless = nullptr;
      wlr_multi_for_each_backend(
          backend,
          [](wlr_backend* candidate, void* data) {
            if (wlr_backend_is_headless(candidate)) {
              *static_cast<wlr_backend**>(data) = candidate;
            }
          },
          &headless
      );
      return headless;
    }
  } // namespace

  std::string Server::createHeadlessOutput(const std::string& name, std::string* error) {
    wlr_backend* headless = headlessBackend(m_backend);
    if (headless == nullptr) {
      *error = "no headless backend in this session";
      return {};
    }
    for (const auto& output : m_outputs) {
      if (output->wlr()->name != nullptr && name == output->wlr()->name) {
        *error = "output already exists: " + name;
        return {};
      }
    }
    m_pendingOutputName = name;
    wlr_output* output = wlr_headless_add_output(headless, kHeadlessWidth, kHeadlessHeight);
    m_pendingOutputName.clear();
    if (output == nullptr) {
      *error = "failed to create a headless output";
      return {};
    }
    return output->name != nullptr ? output->name : "";
  }

  bool Server::destroyOutput(const std::string& name, std::string* error) {
    for (const auto& output : m_outputs) {
      if (output->wlr()->name != nullptr && name == output->wlr()->name) {
        wlr_output_destroy(output->wlr());
        return true;
      }
    }
    *error = "unknown output: " + name;
    return false;
  }

  void Server::arrangeLayers(wlr_output* output) {
    if (Output* out = outputFromWlr(output)) {
      out->markDirty(Dirty::LayerArrange);
    }
  }

  void Server::refreshSurfaceScales() {
    for (const auto& view : m_registry.all()) {
      view->notifyOutputScale();
    }
    for (const auto& layer : m_layerSurfaces) {
      layer->notifyOutputScale();
    }
  }

  void Server::refreshOutputPolicies() {
    for (const auto& output : m_outputs) {
      output->updateVrr();
      output->updateHdr();
    }
  }

  wlr_output* Server::preferredOutput() const {
    wlr_output* output = wlr_output_layout_output_at(m_outputLayout, m_cursor->wlr()->x, m_cursor->wlr()->y);
    if (output != nullptr) {
      return output;
    }
    // Disabled outputs are removed from the layout; never fall back onto one,
    // or focus and new layer surfaces would land on a monitor that is off.
    for (const auto& entry : m_outputs) {
      if (entry->wlr()->enabled) {
        return entry->wlr();
      }
    }
    return nullptr;
  }

  Output* Server::outputFromWlr(wlr_output* output) const {
    if (output == nullptr) {
      return nullptr;
    }
    if (output->data != nullptr) {
      return static_cast<Output*>(output->data);
    }
    for (const auto& entry : m_outputs) {
      if (entry->wlr() == output) {
        return entry.get();
      }
    }
    return nullptr;
  }

  Output* Server::outputFromName(const std::string& name) const {
    for (const auto& entry : m_outputs) {
      // Disabled outputs are off the desktop: rules and keybinds must not be
      // able to address them, or windows and focus would land on a blank screen.
      if (entry->wlr()->enabled && entry->wlr()->name != nullptr && name == entry->wlr()->name) {
        return entry.get();
      }
    }
    return nullptr;
  }

  Output* Server::focusedOutput() const {
    wlr_surface* surface = m_seat->wlr()->keyboard_state.focused_surface;
    if (surface == nullptr) {
      return nullptr;
    }
    if (View* view = View::fromSurface(surface);
        view != nullptr && view->workspace() != nullptr && view->workspace()->group() != nullptr) {
      return view->workspace()->group()->output();
    }
    const wlr_surface* root = wlr_surface_get_root_surface(surface);
    for (const auto& entry : m_layerSurfaces) {
      if (entry->layerSurface()->surface == root) {
        return outputFromWlr(entry->layerSurface()->output);
      }
    }
    return nullptr;
  }

  wlr_box Server::usableAreaAt(double lx, double ly) const {
    wlr_output* output = wlr_output_layout_output_at(m_outputLayout, lx, ly);
    if (output == nullptr) {
      output = preferredOutput();
    }
    if (Output* out = outputFromWlr(output)) {
      wlr_box usable = out->usableArea();
      if (usable.width > 0 && usable.height > 0) {
        return usable;
      }
    }

    wlr_box fullArea{};
    wlr_output_layout_get_box(m_outputLayout, output, &fullArea);
    return fullArea;
  }

} // namespace umbriel
