#include "xwayland/outputs.h"

#include "output/identity.h"
#include "wlr.h"
#include "xdg-output-unstable-v1-protocol.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace umbriel {

  namespace {
    constexpr uint32_t kVersion = 3;

    void destroyResource(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

    const struct zxdg_output_v1_interface kOutputImplementation = {
        .destroy = destroyResource,
    };

    int scaled(int value, double factor) { return static_cast<int>(std::lround(value * factor)); }

    // Squared distance from a point to a box; 0 inside it.
    long long distanceSquared(const wlr_box& box, int x, int y) {
      const long long dx = std::max({box.x - x, 0, x - (box.x + box.width - 1)});
      const long long dy = std::max({box.y - y, 0, y - (box.y + box.height - 1)});
      return (dx * dx) + (dy * dy);
    }
  } // namespace

  int XwaylandRegion::toX(int layoutX) const { return x.x + scaled(layoutX - layout.x, scale); }

  int XwaylandRegion::toY(int layoutY) const { return x.y + scaled(layoutY - layout.y, scale); }

  int XwaylandRegion::toLayoutX(int xX) const { return layout.x + scaled(xX - x.x, 1.0 / scale); }

  int XwaylandRegion::toLayoutY(int xY) const { return layout.y + scaled(xY - x.y, 1.0 / scale); }

  int XwaylandRegion::toXWidth(int width) const { return width == layout.width ? x.width : scaled(width, scale); }

  int XwaylandRegion::toXHeight(int height) const { return height == layout.height ? x.height : scaled(height, scale); }

  int XwaylandRegion::toLayoutWidth(int width) const {
    return width == x.width ? layout.width : scaled(width, 1.0 / scale);
  }

  int XwaylandRegion::toLayoutHeight(int height) const {
    return height == x.height ? layout.height : scaled(height, 1.0 / scale);
  }

  bool XwaylandRegion::operator==(const XwaylandRegion& other) const {
    return wlr_box_equal(&layout, &other.layout) && wlr_box_equal(&x, &other.x) && scale == other.scale;
  }

  XwaylandOutputs::XwaylandOutputs(
      wl_display* display, wlr_output_layout* layout, bool nativeResolution, RenderScale renderScale
  )
      : m_layout(layout), m_nativeResolution(nativeResolution), m_renderScale(std::move(renderScale)) {
    m_global = wl_global_create(display, &zxdg_output_manager_v1_interface, kVersion, this, bind);
    update();
  }

  XwaylandOutputs::~XwaylandOutputs() {
    for (const auto& entry : m_entries) {
      wl_resource_set_user_data(entry->resource, nullptr);
      wl_list_remove(&entry->outputDestroy.link);
    }
    m_entries.clear();
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  bool XwaylandOutputs::advertise(const wl_client* client, const wl_global* global, const wl_client* xwayland) const {
    return (global == m_global) == (client == xwayland);
  }

  std::optional<std::string> XwaylandOutputs::primaryOutput(std::string_view name) const {
    if (name.empty()) {
      return std::nullopt;
    }
    const auto text = [](const char* value) { return value != nullptr ? std::string_view(value) : std::string_view(); };
    wlr_output_layout_output* layoutOutput = nullptr;
    wl_list_for_each(layoutOutput, &m_layout->outputs, link) {
      const wlr_output* output = layoutOutput->output;
      const OutputIdentity identity{
          .connector = text(output->name),
          .make = text(output->make),
          .model = text(output->model),
          .serial = text(output->serial),
      };
      if (outputNameMatch(identity, name) != OutputNameMatch::None) {
        return std::string(identity.connector);
      }
    }
    return std::string();
  }

  bool XwaylandOutputs::update() {
    wlr_box extents{};
    wlr_output_layout_get_box(m_layout, nullptr, &extents);
    std::vector<OutputRegion> regions;
    double positionScale = 0;
    wlr_output_layout_output* layoutOutput = nullptr;
    wl_list_for_each(layoutOutput, &m_layout->outputs, link) {
      wlr_output* output = layoutOutput->output;
      wlr_box box{};
      wlr_output_layout_get_box(m_layout, output, &box);
      if (wlr_box_empty(&box)) {
        continue;
      }
      XwaylandRegion region{.layout = box, .x = {.x = 0, .y = 0, .width = box.width, .height = box.height}};
      if (m_nativeResolution) {
        // X11 clients render at the output's render scale, like Wayland clients.
        region.scale = m_renderScale != nullptr ? m_renderScale(output) : static_cast<double>(output->scale);
        // X coordinates measure render-scale pixels, so the X rectangle must
        // too: scale the physical resolution by the same factor the mapping
        // uses. A render scale that follows the logical one has factor 1 and
        // keeps the physical size exactly.
        wlr_output_transformed_resolution(output, &region.x.width, &region.x.height);
        region.x.width = scaled(region.x.width, region.scale / output->scale);
        region.x.height = scaled(region.x.height, region.scale / output->scale);
      }
      positionScale = std::max(positionScale, region.scale);
      regions.push_back({.output = output, .region = region});
    }
    // The largest scale spreads the outputs at least as far apart as any of them grew.
    for (OutputRegion& entry : regions) {
      entry.region.x.x = scaled(entry.region.layout.x - extents.x, positionScale);
      entry.region.x.y = scaled(entry.region.layout.y - extents.y, positionScale);
    }
    const XwaylandRegion fallback{.layout = {.x = extents.x, .y = extents.y, .width = 0, .height = 0}};
    const bool changed = regions != m_regions || fallback != m_fallback;
    m_regions = std::move(regions);
    m_fallback = fallback;
    m_scale = m_regions.empty() ? 1.0 : positionScale;
    for (const auto& entry : m_entries) {
      send(*entry, false);
    }
    return changed;
  }

  XwaylandRegion XwaylandOutputs::region(const wlr_output* output, int layoutX, int layoutY) const {
    const OutputRegion* nearest = nullptr;
    long long nearestDistance = std::numeric_limits<long long>::max();
    for (const OutputRegion& entry : m_regions) {
      if (entry.output == output) {
        return entry.region;
      }
      const long long distance = distanceSquared(entry.region.layout, layoutX, layoutY);
      if (distance < nearestDistance) {
        nearest = &entry;
        nearestDistance = distance;
      }
    }
    return nearest != nullptr ? nearest->region : m_fallback;
  }

  XwaylandRegion XwaylandOutputs::regionAtX(int x, int y) const {
    const OutputRegion* nearest = nullptr;
    long long nearestDistance = std::numeric_limits<long long>::max();
    for (const OutputRegion& entry : m_regions) {
      const long long distance = distanceSquared(entry.region.x, x, y);
      if (distance < nearestDistance) {
        nearest = &entry;
        nearestDistance = distance;
      }
    }
    return nearest != nullptr ? nearest->region : m_fallback;
  }

  wlr_box XwaylandOutputs::xBox(const wlr_output* output) const {
    const auto found = std::ranges::find(m_regions, output, &OutputRegion::output);
    return found != m_regions.end() ? found->region.x : wlr_box{};
  }

  void XwaylandOutputs::send(Entry& entry, bool initial) {
    const wlr_box box = xBox(entry.output);
    if (wlr_box_empty(&box) || (!initial && wlr_box_equal(&box, &entry.sent))) {
      return;
    }
    entry.sent = box;
    const int version = wl_resource_get_version(entry.resource);
    if (initial && version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
      zxdg_output_v1_send_name(entry.resource, entry.output->name);
      if (entry.output->description != nullptr) {
        zxdg_output_v1_send_description(entry.resource, entry.output->description);
      }
    }
    zxdg_output_v1_send_logical_position(entry.resource, box.x, box.y);
    zxdg_output_v1_send_logical_size(entry.resource, box.width, box.height);
    // Version 3 replaced xdg_output.done with wl_output.done.
    if (version >= 3) {
      wlr_output_schedule_done(entry.output);
    } else {
      zxdg_output_v1_send_done(entry.resource);
    }
  }

  void XwaylandOutputs::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    static const struct zxdg_output_manager_v1_interface kManagerImplementation = {
        .destroy = destroyResource,
        .get_xdg_output = handleGetXdgOutput,
    };
    wl_resource* resource = wl_resource_create(
        client, &zxdg_output_manager_v1_interface, static_cast<int>(std::min(version, kVersion)), id
    );
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
  }

  void XwaylandOutputs::handleGetXdgOutput(
      wl_client* client, wl_resource* manager, uint32_t id, wl_resource* outputResource
  ) {
    auto* self = static_cast<XwaylandOutputs*>(wl_resource_get_user_data(manager));
    wl_resource* resource = wl_resource_create(client, &zxdg_output_v1_interface, wl_resource_get_version(manager), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kOutputImplementation, nullptr, handleResourceDestroy);
    wlr_output* output = wlr_output_from_resource(outputResource);
    if (output == nullptr) {
      return; // an inert wl_output stays an inert xdg_output
    }
    auto entry = std::make_unique<Entry>();
    entry->owner = self;
    entry->resource = resource;
    entry->output = output;
    entry->outputDestroy.notify = onOutputDestroy;
    wl_signal_add(&output->events.destroy, &entry->outputDestroy);
    wl_resource_set_user_data(resource, entry.get());
    Entry& added = *self->m_entries.emplace_back(std::move(entry));
    self->send(added, true);
  }

  void XwaylandOutputs::handleResourceDestroy(wl_resource* resource) {
    if (auto* entry = static_cast<Entry*>(wl_resource_get_user_data(resource))) {
      entry->owner->forget(entry);
    }
  }

  void XwaylandOutputs::onOutputDestroy(wl_listener* listener, void* /*data*/) {
    Entry* entry = wl_container_of(listener, entry, outputDestroy); // NOLINT(modernize-use-auto)
    // The client still owns the resource; it only stops receiving events.
    wl_resource_set_user_data(entry->resource, nullptr);
    entry->owner->forget(entry);
  }

  void XwaylandOutputs::forget(Entry* entry) {
    wl_list_remove(&entry->outputDestroy.link);
    std::erase_if(m_entries, [entry](const std::unique_ptr<Entry>& candidate) { return candidate.get() == entry; });
  }

} // namespace umbriel
