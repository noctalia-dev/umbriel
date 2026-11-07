#include "view/view_preview.h"

extern "C" {
#include <umbrielfx/render/effect.h>
}

#include "output/output.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/ssd_frame.h"
#include "scene/surface_mirror.h"
#include "scene/surface_shape.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "xwayland/xwayland.h"

namespace umbriel {

  namespace {
    // A preview that is not interactive is pure output: its owner hit-tests off its own boxes.
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

    // Maps a hit on the preview's buffer, in its destination pixels, to the surface-local point it shows.
    bool acceptInput(wlr_scene_buffer* buffer, double* sx, double* sy) {
      auto* entry = static_cast<ViewPreview::Surface*>(sceneNodeFrom(buffer->node.data));
      if (entry == nullptr || entry->surface == nullptr || buffer->dst_width <= 0 || buffer->dst_height <= 0) {
        return false;
      }
      const double x = entry->crop.x + *sx * entry->crop.width / buffer->dst_width;
      const double y = entry->crop.y + *sy * entry->crop.height / buffer->dst_height;
      if (!wlr_surface_point_accepts_input(entry->surface, x, y)) {
        return false;
      }
      *sx = x;
      *sy = y;
      return true;
    }

    // The size `surface` is shown at in its view, through the view's scene buffer `source`. It differs from the surface
    // size when the view draws the surface scaled.
    void viewSurfaceSize(wlr_scene_buffer* source, const wlr_surface* surface, int* width, int* height) {
      if (wlr_scene_surface* sceneSurface = source != nullptr ? wlr_scene_surface_try_from_buffer(source) : nullptr) {
        wlr_scene_surface_get_size(sceneSurface, width, height);
        return;
      }
      *width = surface->current.width;
      *height = surface->current.height;
    }
  } // namespace

  wlr_scene_buffer* sourceBufferForSurface(wlr_scene_node* node, wlr_surface* surface) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
      wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
      wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
      return sceneSurface != nullptr && sceneSurface->surface == surface ? buffer : nullptr;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
      return nullptr;
    }
    wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
    wlr_scene_node* child = nullptr;
    wl_list_for_each(child, &tree->children, link) {
      if (wlr_scene_buffer* buffer = sourceBufferForSurface(child, surface)) {
        return buffer;
      }
    }
    return nullptr;
  }

  // -: construction

  ViewPreview::ViewPreview(Server& server, View* view, std::function<void()> onChanged)
      : m_server(&server), m_view(view), m_onChanged(std::move(onChanged)) {}

  std::unique_ptr<ViewPreview> ViewPreview::create(
      Server& server, View* view, wlr_scene_tree* parent, std::function<void()> onChanged, bool interactive
  ) {
    std::unique_ptr<ViewPreview> preview(new ViewPreview(server, view, std::move(onChanged)));
    preview->m_interactive = interactive;
    preview->m_tree = wlr_scene_tree_create(parent);
    if (preview->m_tree == nullptr) {
      return nullptr;
    }
    preview->m_surfaceTree = wlr_scene_tree_create(preview->m_tree);
    (void)umbrielfx_scene_node_share_capture_policy(&preview->m_tree->node, &view->sceneTree()->node);
    if (preview->m_surfaceTree == nullptr) {
      return nullptr;
    }
    const std::array<float, 4> innerColor = tint(view->borderColors().unfocused, 1.0);
    const std::array<float, 4> outerColor = tint(view->borderColors().outer, 1.0);
    preview->m_border = wlr_scene_border_create(preview->m_tree, innerColor.data(), outerColor.data());
    if (preview->m_border == nullptr) {
      return nullptr;
    }
    preview->forEachSurface(addSurface);
    return preview;
  }

  bool ViewPreview::resolveBuffer(wlr_scene_buffer* buffer, wlr_surface** surface, View** view) {
    auto* tag = sceneNodeFrom(buffer->node.data);
    if (tag == nullptr || tag->kind != SceneNodeKind::PreviewSurface) {
      return false;
    }
    *surface = static_cast<PreviewSurfaceNode*>(tag)->surface;
    *view = *surface != nullptr ? View::fromSurface(*surface) : nullptr;
    if (*view != nullptr && (*view)->blocked()) {
      *surface = nullptr;
    }
    return true;
  }

  ViewPreview::~ViewPreview() {
    m_view->effects().detachNodes(
        m_surfaceTree != nullptr ? &m_surfaceTree->node : nullptr, m_border != nullptr ? &m_border->node : nullptr
    );
    for (const auto& entry : m_surfaces) {
      removeListeners(*entry);
    }
    m_surfaces.clear();
    // The chrome copy's nodes hang under the root tree, so it goes before the tree does.
    m_chrome.reset();
    m_frame.reset();
    releaseShadow();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
  }

  // -: layout

  void ViewPreview::hide(const EffectContext& effects) {
    m_blur.hide();
    wlr_scene_node_set_enabled(&m_tree->node, false);
    syncEffects(effects);
  }

  bool ViewPreview::prepare(const EffectContext& effects) {
    if (m_shadowTree != nullptr) {
      wlr_scene_node_set_enabled(&m_shadowTree->node, false);
    }
    const wlr_box& geometry = m_view->geometryBox();
    // A tiled opener waiting for the arrange that places it is not showing yet, and a hidden tab shares its box with
    // the tab on show. Their previews follow.
    if (geometry.width <= 0 || geometry.height <= 0 || m_view->presentationSuppressed()) {
      hide(effects);
      return false;
    }
    const wlr_box& world = m_view->presentedBox();
    if (world.width <= 0 || world.height <= 0) {
      hide(effects);
      return false;
    }
    wlr_scene_node_set_enabled(&m_tree->node, true);
    return true;
  }

  wlr_box ViewPreview::contentSize(double zoom) const {
    const wlr_box& world = m_view->presentedBox();
    return {
        .x = 0,
        .y = 0,
        .width = std::max(1, static_cast<int>(std::lround(world.width * zoom))),
        .height = std::max(1, static_cast<int>(std::lround(world.height * zoom))),
    };
  }

  void ViewPreview::layout(const Placement& placement) {
    View* view = m_view;
    const wlr_box& geometry = view->geometryBox();
    const wlr_box& world = view->presentedBox();
    const double z = placement.zoom;
    const int contentW = placement.box.width;
    const int contentH = placement.box.height;
    wlr_scene_node_set_position(&m_tree->node, placement.box.x, placement.box.y);
    // The parent decides what of the preview shows.
    wlr_scene_tree_set_clip(m_tree, nullptr);
    layoutShadow(placement);
    const float presentedOpacity = view->presentedOpacity() * placement.opacity;

    const int borderWidth = view->decorationBorderWidth();
    const int outerBorderWidth = view->decorationOuterBorderWidth();
    const int total = borderWidth + outerBorderWidth;
    const bool decorated = total > 0 && !view->currentFullscreen() && !view->maximizedToEdges();
    const int scaledRadius = static_cast<int>(std::lround(view->decorationCornerRadius() * z));
    const int outerRadius = decorated ? scaledRadius : 0;
    const auto scaledWidth = [z](int width) {
      return width > 0 ? std::max(1, static_cast<int>(std::lround(width * z))) : 0;
    };
    const int innerWidth = scaledWidth(borderWidth);
    const int outerWidth = scaledWidth(outerBorderWidth);
    const int surfaceRadius = nestedRadius(outerRadius, innerWidth + outerWidth);
    const bool borderVisible = decorated && innerWidth + outerWidth > 0;
    wlr_scene_node_set_enabled(&m_border->node, borderVisible);
    if (borderVisible) {
      m_borderPadding = scaledWidth(view->borderEffectPadding());
      applyBorderGeometry(
          m_border,
          makeBorderRing(
              contentW, contentH + static_cast<int>(std::lround(view->titleBarHeight() * z)), outerRadius, innerWidth,
              outerWidth, m_borderPadding
          ),
          innerWidth, outerWidth
      );
      wlr_scene_node_set_position(
          &m_border->node, m_border->node.x,
          m_border->node.y - static_cast<int>(std::lround(view->titleBarHeight() * z))
      );
      const std::array<float, 4> innerColor = tint(placement.innerBorderColor, presentedOpacity);
      const std::array<float, 4> outerColor = tint(view->borderColors().outer, presentedOpacity);
      wlr_scene_border_set_colors(m_border, innerColor.data(), outerColor.data());
    }
    layoutChrome(world, decorated ? total : 0, z, presentedOpacity, placement.effects.output);
    syncEffects(placement.effects);

    // Every surface rounds against the content box, the same rule the live window uses, so a client that draws its
    // corners from a subsurface keeps them rounded in the preview.
    const fx_corner_radii contentCorners = view->titleBarHeight() > 0
        ? corner_radii_new(0, 0, surfaceRadius, surfaceRadius)
        : corner_radii_all(surfaceRadius);
    const auto roundToContentBox = [surfaceRadius, contentW, contentH, contentCorners](wlr_scene_buffer* buffer) {
      const wlr_box cornerBox{-buffer->node.x, -buffer->node.y, contentW, contentH};
      wlr_scene_buffer_set_corner_radii(buffer, contentCorners);
      wlr_scene_buffer_set_corner_box(buffer, surfaceRadius > 0 ? &cornerBox : nullptr);
    };
    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    bool blurUpdated = false;
    for (const auto& entry : m_surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr) {
        continue;
      }
      // A client retires a subsurface with a null buffer, or by destroying its subsurface role. Either drops the
      // surface out of the commit walk; skipping its mirror would leave the last frame at the zoom it was laid out at.
      // A destroyed role emits no commit, so `mapped` catches it before the next walk does.
      if (!entry->inTree || !surface->mapped || surface->current.width <= 0 || surface->current.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      const float surfaceOpacity =
          entry->sourceBuffer != nullptr ? entry->sourceBuffer->opacity * placement.opacity : placement.opacity;
      wlr_scene_buffer_set_opacity(entry->buffer, surfaceOpacity);
      const pixman_region32_t* shape = entry->view->xwayland() ? m_server->xwayland()->surfaceShape(surface) : nullptr;
      if (!entry->isRoot) {
        wlr_box sub{
            placement.box.x + static_cast<int>(std::lround((entry->sx - geometry.x) * fx)),
            placement.box.y + static_cast<int>(std::lround((entry->sy - geometry.y) * fy)),
            std::max(1, static_cast<int>(std::lround(surface->current.width * fx))),
            std::max(1, static_cast<int>(std::lround(surface->current.height * fy))),
        };
        entry->crop = {
            0.0, 0.0, static_cast<double>(surface->current.width), static_cast<double>(surface->current.height)
        };
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x - placement.box.x, sub.y - placement.box.y);
        wlr_scene_buffer_set_dest_size(entry->buffer, sub.width, sub.height);
        applySurfaceShape(entry->buffer, surface, shape);
        roundToContentBox(entry->buffer);
        continue;
      }
      // Root surface: crop to the committed window geometry so CSD shadow padding never leaks into the preview, then
      // scale the visible part of that region onto the content box.
      wlr_fbox base{};
      wlr_surface_get_buffer_source_box(surface, &base);
      int width = 0;
      int height = 0;
      viewSurfaceSize(entry->sourceBuffer, surface, &width, &height);
      const double bx = base.width / width;
      const double by = base.height / height;
      wlr_fbox src{base.x + geometry.x * bx, base.y + geometry.y * by, geometry.width * bx, geometry.height * by};
      if (src.x < base.x) {
        src.width -= base.x - src.x;
        src.x = base.x;
      }
      if (src.y < base.y) {
        src.height -= base.y - src.y;
        src.y = base.y;
      }
      src.width = std::min(src.width, base.x + base.width - src.x);
      src.height = std::min(src.height, base.y + base.height - src.y);
      if (src.width <= 0 || src.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      // In surface-local units, which a scaled X11 surface does not share with its scene size.
      const double ux = base.width / surface->current.width;
      const double uy = base.height / surface->current.height;
      entry->crop = {(src.x - base.x) / ux, (src.y - base.y) / uy, src.width / ux, src.height / uy};
      wlr_scene_node_set_enabled(&entry->buffer->node, true);
      wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
      wlr_scene_buffer_set_source_box(entry->buffer, &src);
      wlr_scene_buffer_set_dest_size(entry->buffer, contentW, contentH);
      applySurfaceShape(entry->buffer, surface, shape);
      roundToContentBox(entry->buffer);
      const wlr_box blurBox{0, 0, contentW, contentH};
      m_blur.setAlpha(1.0F);
      m_blur.update(
          m_tree, surface, blurBox, geometry, contentCorners, nullptr, view->blurOptions(), entry->buffer->opacity,
          entry->buffer
      );
      blurUpdated = true;
    }
    if (!blurUpdated) {
      m_blur.hide();
    }
  }

  void ViewPreview::layoutChrome(const wlr_box& world, int borderInset, double zoom, float alpha, Output* output) {
    ViewChromeGeometry geometry{
        .contentX = 0,
        .contentY = 0,
        .contentWidth = world.width,
        .contentHeight = world.height,
        .borderInset = borderInset,
        .cornerRadius = m_view->maximizedToEdges() ? 0 : m_view->decorationCornerRadius(),
        .titleBarHeight = m_view->titleBarHeight(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = m_view->currentFullscreen(),
        .zoom = zoom,
    };
    const auto update = [&](const ViewChromeAttachment* live, std::unique_ptr<ViewChromeAttachment>& preview) {
      if (live == nullptr) {
        preview.reset();
        return;
      }
      if (preview == nullptr) {
        preview = live->makePreview(m_tree);
      }
      if (preview == nullptr) {
        return;
      }
      live->syncPreview(*preview);
      preview->setAlpha(alpha);
      preview->layout(geometry);
    };
    update(m_view->ssdFrame(), m_frame);
    geometry.suppressed = geometry.suppressed || m_view->maximizedToEdges();
    update(m_view->chromeAttachment(), m_chrome);
  }

  void ViewPreview::layoutShadow(const Placement& placement) {
    const wlr_scene_shadow* source = m_view->shadowNode();
    if (placement.shadowAlpha <= 0.0F || source == nullptr || !source->node.enabled) {
      return;
    }
    wlr_scene_tree* parent = m_view->shadowPooled() ? placement.shadowPool : m_tree;
    if (parent == nullptr) {
      return;
    }
    if (m_shadowTree == nullptr) {
      m_shadowTree = wlr_scene_tree_create(parent);
      if (m_shadowTree == nullptr) {
        return;
      }
      (void)umbrielfx_scene_node_share_capture_policy(&m_shadowTree->node, &m_tree->node);
      m_shadow = wlr_scene_shadow_create(m_shadowTree, 0, 0, 0, 0.0F, source->color);
      if (m_shadow == nullptr) {
        releaseShadow();
        return;
      }
    } else if (m_shadowTree->node.parent != parent) {
      wlr_scene_node_reparent(&m_shadowTree->node, parent);
    }
    // Under the root tree the shadow follows the preview origin and sits below its content, the way the real
    // container sits under the frame; in the pool it carries the preview origin itself.
    if (parent == m_tree) {
      wlr_scene_node_lower_to_bottom(&m_shadowTree->node);
      wlr_scene_node_set_position(&m_shadowTree->node, 0, 0);
    } else {
      wlr_scene_node_set_position(&m_shadowTree->node, placement.box.x, placement.box.y);
    }
    wlr_scene_node_set_enabled(&m_shadowTree->node, true);

    const double zoom = placement.zoom;
    const auto scaled = [zoom](int value) { return static_cast<int>(std::lround(value * zoom)); };
    const auto scaledCorner = [zoom](uint16_t value) {
      return static_cast<uint16_t>(std::lround(static_cast<double>(value) * zoom));
    };
    wlr_scene_node_set_position(&m_shadow->node, scaled(source->node.x), scaled(source->node.y));
    wlr_scene_shadow_set_size(m_shadow, std::max(0, scaled(source->width)), std::max(0, scaled(source->height)));
    wlr_scene_shadow_set_blur_sigma(m_shadow, static_cast<float>(source->blur_sigma * zoom));
    wlr_scene_shadow_set_corner_radius(m_shadow, scaled(source->corner_radius));
    const float color[4] = {
        source->color[0], source->color[1], source->color[2], source->color[3] * placement.shadowAlpha
    };
    wlr_scene_shadow_set_color(m_shadow, color);
    const clipped_region& hole = source->clipped_region;
    wlr_scene_shadow_set_clipped_region(
        m_shadow,
        clipped_region{
            .area =
                {
                    .x = scaled(hole.area.x),
                    .y = scaled(hole.area.y),
                    .width = scaled(hole.area.width),
                    .height = scaled(hole.area.height),
                },
            .corners =
                {
                    .top_left = scaledCorner(hole.corners.top_left),
                    .top_right = scaledCorner(hole.corners.top_right),
                    .bottom_right = scaledCorner(hole.corners.bottom_right),
                    .bottom_left = scaledCorner(hole.corners.bottom_left),
                },
            .inside = hole.inside,
        }
    );
  }

  void ViewPreview::releaseShadow() {
    if (m_shadowTree != nullptr) {
      wlr_scene_node_destroy(&m_shadowTree->node);
    }
    m_shadowTree = nullptr;
    m_shadow = nullptr;
  }

  void ViewPreview::syncEffects(const EffectContext& effects) {
    BorderEffectGate gate = effects.gate;
    gate.decorated = m_border != nullptr && m_border->node.enabled && m_tree->node.enabled;
    m_view->syncAnimationEffects(
        m_tree, m_border != nullptr ? &m_border->node : nullptr, &m_surfaceTree->node, &gate, effects.output,
        effects.scale
    );
  }

  // -: surfaces

  void ViewPreview::syncBuffer(Surface& entry) {
    if (entry.surface == nullptr || entry.buffer == nullptr) {
      return;
    }
    int width = 0;
    int height = 0;
    viewSurfaceSize(entry.sourceBuffer, entry.surface, &width, &height);
    syncSurfaceMirror(
        entry.buffer, entry.surface, entry.sourceBuffer, width, height, entry.preview->m_server->colorManager()
    );
  }

  void ViewPreview::forEachSurface(void (*visit)(wlr_surface*, int, int, void*)) {
    View* view = m_view;
    wlr_surface* root = view->rootSurface();
    if (root == nullptr) {
      return;
    }
    Walk own{.preview = this, .view = view};
    wlr_surface_for_each_surface(root, visit, &own);
    if (view->attachments().empty()) {
      return;
    }
    // A dialog hangs in the window's frame, so the scene holds both at their real offset.
    wlr_scene_buffer* rootBuffer = sourceBufferForSurface(&view->sceneTree()->node, root);
    if (rootBuffer == nullptr) {
      return;
    }
    int rootX = 0;
    int rootY = 0;
    wlr_scene_node_coords(&rootBuffer->node, &rootX, &rootY);
    const auto walk = [&](const auto& self, const View* parent) -> void {
      for (View* dialog : parent->attachments()) {
        wlr_surface* surface = dialog->rootSurface();
        wlr_scene_buffer* buffer =
            surface != nullptr ? sourceBufferForSurface(&dialog->sceneTree()->node, surface) : nullptr;
        if (buffer != nullptr) {
          int x = 0;
          int y = 0;
          wlr_scene_node_coords(&buffer->node, &x, &y);
          Walk offset{.preview = this, .view = dialog, .dx = x - rootX, .dy = y - rootY};
          wlr_surface_for_each_surface(surface, visit, &offset);
        }
        self(self, dialog);
      }
    };
    walk(walk, view);
  }

  void ViewPreview::addSurface(wlr_surface* surface, int sx, int sy, void* data) {
    const auto* walk = static_cast<const Walk*>(data);
    ViewPreview* preview = walk->preview;
    wlr_scene_buffer* source = sourceBufferForSurface(&walk->view->sceneTree()->node, surface);
    if (source == nullptr) {
      return;
    }
    wlr_scene_buffer* buffer = wlr_scene_buffer_create(preview->m_surfaceTree, nullptr);
    if (buffer == nullptr) {
      return;
    }
    auto entry = std::make_unique<Surface>();
    entry->preview = preview;
    entry->view = walk->view;
    entry->surface = surface;
    entry->sourceBuffer = source;
    entry->buffer = buffer;
    entry->sx = sx + walk->dx;
    entry->sy = sy + walk->dy;
    entry->isRoot = surface == preview->m_view->rootSurface();
    wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
    if (preview->m_interactive) {
      buffer->node.data = sceneNodeData(entry.get());
      buffer->point_accepts_input = acceptInput;
    } else {
      buffer->point_accepts_input = rejectInput;
    }
    entry->commit.notify = onSurfaceCommit;
    wl_signal_add(&surface->events.commit, &entry->commit);
    entry->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    entry->outputSample.notify = onBufferOutputSample;
    wl_signal_add(&buffer->events.output_sample, &entry->outputSample);
    entry->frameDone.notify = onBufferFrameDone;
    wl_signal_add(&buffer->events.frame_done, &entry->frameDone);
    syncBuffer(*entry);
    preview->m_surfaces.push_back(std::move(entry));
  }

  void ViewPreview::syncSurface(wlr_surface* surface, int sx, int sy, void* data) {
    const auto* walk = static_cast<const Walk*>(data);
    for (const auto& entry : walk->preview->m_surfaces) {
      if (entry->surface == surface) {
        // A surface given a new subsurface role has a new scene node; the old one went with the old role.
        entry->sourceBuffer = sourceBufferForSurface(&walk->view->sceneTree()->node, surface);
        if (entry->sourceBuffer == nullptr) {
          return;
        }
        entry->inTree = true;
        entry->sx = sx + walk->dx;
        entry->sy = sy + walk->dy;
        syncBuffer(*entry);
        return;
      }
    }
    addSurface(surface, sx, sy, data);
  }

  void ViewPreview::syncSurfaces() {
    for (const auto& entry : m_surfaces) {
      entry->inTree = false;
    }
    forEachSurface(syncSurface);
  }

  void ViewPreview::removeListeners(Surface& entry) {
    wl_list_remove(&entry.commit.link);
    wl_list_remove(&entry.destroy.link);
    wl_list_remove(&entry.outputSample.link);
    wl_list_remove(&entry.frameDone.link);
  }

  void ViewPreview::dropAttachedSurfaces() {
    std::erase_if(m_surfaces, [this](const std::unique_ptr<Surface>& entry) {
      if (entry->view == m_view) {
        return false;
      }
      removeListeners(*entry);
      if (entry->buffer != nullptr) {
        wlr_scene_node_destroy(&entry->buffer->node);
      }
      return true;
    });
  }

  void ViewPreview::onSurfaceCommit(wl_listener* listener, void* /*data*/) {
    Surface* entry;
    entry = wl_container_of(listener, entry, commit);
    ViewPreview* preview = entry->preview;
    // The source scene surface reconfigures on every commit. Refresh the buffer copies; the owner then lays them out
    // again, before the next frame shows them uncropped.
    preview->syncSurfaces();
    // A copy, so the owner may destroy the preview from it.
    if (const std::function<void()> onChanged = preview->m_onChanged) {
      onChanged();
    }
  }

  void ViewPreview::onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    Surface* entry;
    entry = wl_container_of(listener, entry, destroy);
    ViewPreview* preview = entry->preview;
    removeListeners(*entry);
    if (entry->buffer != nullptr) {
      wlr_scene_node_destroy(&entry->buffer->node);
      entry->buffer = nullptr;
    }
    std::erase_if(preview->m_surfaces, [entry](const std::unique_ptr<Surface>& candidate) {
      return candidate.get() == entry;
    });
  }

  void ViewPreview::onBufferOutputSample(wl_listener* listener, void* data) {
    Surface* entry;
    entry = wl_container_of(listener, entry, outputSample);
    sampleSurfaceMirror(entry->surface, *static_cast<wlr_scene_output_sample_event*>(data));
  }

  void ViewPreview::onBufferFrameDone(wl_listener* listener, void* data) {
    Surface* entry;
    entry = wl_container_of(listener, entry, frameDone);
    frameSurfaceMirror(entry->surface, *static_cast<wlr_scene_frame_done_event*>(data));
  }

} // namespace umbriel
