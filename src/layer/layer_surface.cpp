#include "layer/layer_surface.h"

#include "config/resolve.h"
#include "core/dirty.h"
#include "core/log.h"
#include "input/seat.h"
#include "output/output.h"
#include "server/server.h"
#include "view/popup.h"
#include "wlr.h"

#include <algorithm>

namespace umbriel {

  namespace {
    constexpr Logger kLog("layer");
  } // namespace

  LayerSurface::LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface)
      : SceneNode(SceneNodeKind::LayerSurface), m_server(&server), m_layerSurface(layerSurface) {
    if (m_layerSurface->output == nullptr) {
      m_layerSurface->output = m_server->preferredOutput();
    }
    if (m_layerSurface->output == nullptr) {
      kLog.error("no output for layer surface, destroying");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }

    Output* out = output();
    if (out == nullptr) {
      kLog.error("layer surface output has no Output wrapper, destroying");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }

    const auto layer = m_layerSurface->pending.layer;
    kLog.debug(
        "new layer surface ns={} layer={} output={} exclusive={}",
        m_layerSurface->namespace_ != nullptr ? m_layerSurface->namespace_ : "", static_cast<unsigned>(layer),
        m_layerSurface->output->name, m_layerSurface->pending.exclusive_zone
    );

    m_scene = wlr_scene_layer_surface_v1_create(out->layerTree(layer), m_layerSurface);
    if (m_scene == nullptr) {
      kLog.error("failed to create scene layer surface");
      wlr_layer_surface_v1_destroy(m_layerSurface);
      m_layerSurface = nullptr;
      return;
    }
    m_rule = resolveLayerRules(config(), m_layerSurface->namespace_);
    m_scene->tree->node.data = sceneNodeData(this);
    m_layerSurface->data = this;

    m_map.notify = onMap;
    wl_signal_add(&m_layerSurface->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_layerSurface->surface->events.unmap, &m_unmap);
    m_commit.notify = onCommit;
    wl_signal_add(&m_layerSurface->surface->events.commit, &m_commit);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_layerSurface->events.destroy, &m_destroy);
    m_newPopup.notify = onNewPopup;
    wl_signal_add(&m_layerSurface->events.new_popup, &m_newPopup);

    m_server->registerAnimatable(this);
    notifyOutputScale();
  }

  LayerSurface::~LayerSurface() {
    m_server->unregisterAnimatable(this);
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_newPopup.link);
    }
  }

  bool LayerSurface::tickAnimations(uint64_t nowMsec) {
    if (!m_fade.tick(nowMsec)) {
      return false;
    }
    applyFadeAlpha();
    return true;
  }

  bool LayerSurface::animatesOn(const Output* out) const { return output() == out; }

  void LayerSurface::applyFadeAlpha() {
    if (m_scene == nullptr) {
      return;
    }
    // Overshooting curves can push this out of range; wlr_scene_buffer_set_opacity asserts opacity is in [0, 1].
    float alpha = std::clamp(static_cast<float>(m_fade.current()), 0.0F, 1.0F);
    wlr_scene_node_for_each_buffer(
        &m_scene->tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &alpha
    );
  }

  void LayerSurface::beginCloseAnimation() {
    Output* out = output();
    const auto& animation = config().animation;
    const auto& layers = animation.layers;
    if (out == nullptr || m_scene == nullptr || !animation.enabled || !layers.enabled) {
      return;
    }

    wlr_scene_tree* snap = wlr_scene_tree_create(out->layerTree(m_layerSurface->current.layer));
    if (snap == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&snap->node, m_scene->tree->node.x, m_scene->tree->node.y);

    struct CopyCtx {
      wlr_scene_tree* snap;
      int rootX;
      int rootY;
      int buffersCopied;
    };
    CopyCtx ctx{snap, m_scene->tree->node.x, m_scene->tree->node.y, 0};
    wlr_scene_node_for_each_buffer(
        &m_scene->tree->node,
        [](wlr_scene_buffer* src, int sx, int sy, void* data) {
          auto* c = static_cast<CopyCtx*>(data);
          if (src->buffer == nullptr || !src->node.enabled) {
            return;
          }
          wlr_scene_buffer* copy = wlr_scene_buffer_create(c->snap, src->buffer);
          if (copy == nullptr) {
            return;
          }
          wlr_scene_node_set_position(&copy->node, sx - c->rootX, sy - c->rootY);
          if (src->dst_width > 0 && src->dst_height > 0) {
            wlr_scene_buffer_set_dest_size(copy, src->dst_width, src->dst_height);
          }
          if (src->src_box.width > 0 && src->src_box.height > 0) {
            wlr_scene_buffer_set_source_box(copy, &src->src_box);
          }
          wlr_scene_buffer_set_transform(copy, src->transform);
          wlr_scene_buffer_set_corner_radii(copy, src->corners);
          wlr_scene_buffer_set_opacity(copy, src->opacity);
          wlr_scene_buffer_set_transfer_function(copy, src->transfer_function);
          wlr_scene_buffer_set_primaries(copy, src->primaries);
          wlr_scene_buffer_set_luminance_multiplier(copy, src->luminance_multiplier);
          wlr_scene_buffer_set_color_encoding(copy, src->color_encoding);
          wlr_scene_buffer_set_color_range(copy, src->color_range);
          ++c->buffersCopied;
        },
        &ctx
    );

    if (ctx.buffersCopied == 0) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }

    m_server->animateCloseSnapshot(
        out, snap, {},
        Server::CloseSnapshotOverrides{.durationMs = layers.durationMs, .curve = layers.curve, .style = "fade"}
    );
    wlr_output_schedule_frame(out->wlr());
  }

  Output* LayerSurface::output() const {
    if (m_layerSurface == nullptr || m_layerSurface->output == nullptr) {
      return nullptr;
    }
    return static_cast<Output*>(m_layerSurface->output->data);
  }

  void LayerSurface::notifyOutputScale() {
    Output* out = output();
    if (out == nullptr || m_layerSurface == nullptr) {
      return;
    }
    wlr_layer_surface_v1_for_each_surface(m_layerSurface, &Output::notifySurfaceScaleIter, out);
    wlr_layer_surface_v1_for_each_popup_surface(m_layerSurface, &Output::notifySurfaceScaleIter, out);
  }

  bool LayerSurface::exclusiveKeyboard() const {
    return m_mapped
        && m_layerSurface != nullptr
        && m_layerSurface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
  }

  bool LayerSurface::acceptsKeyboard() const {
    if (!m_mapped || m_layerSurface == nullptr) {
      return false;
    }
    const auto interactivity = m_layerSurface->current.keyboard_interactive;
    return interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
        || interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
  }

  bool LayerSurface::hasKeyboardFocus() const {
    if (m_layerSurface == nullptr) {
      return false;
    }
    return m_server->seat()->wlr()->keyboard_state.focused_surface == m_layerSurface->surface;
  }

  void LayerSurface::reparentToLayer(uint32_t layer) {
    Output* out = output();
    if (out == nullptr || m_scene == nullptr) {
      return;
    }
    wlr_scene_node_reparent(&m_scene->tree->node, out->layerTree(layer));
  }

  void LayerSurface::focus() {
    if (m_server->sessionLocked() || !acceptsKeyboard()) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = m_layerSurface->surface;
    // Layers take the seat: drop every window's activated/border so none stay "focused".
    m_server->deactivateViews(nullptr);
    if (seat->keyboard_state.focused_surface == surface) {
      return;
    }

    // End any keyboard grab (e.g. xdg_popup) that would intercept the enter and block focus from reaching this layer
    // surface. Pointer/touch grabs remain intact so the popup stays visually mapped.
    wlr_seat_keyboard_end_grab(seat);

    // Give the layer seat keyboard focus so clients (e.g. Noctalia) receive Escape.
    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    } else {
      wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
    }
    m_server->refreshOutputPolicies();
  }

  SurfaceBlurOptions LayerSurface::blurOptions() const {
    return SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(m_rule.ignoreAlpha.value_or(0.0)),
        .enabled = m_rule.blur.value_or(false),
        .optimized = m_rule.optimized,
    };
  }

  SurfaceBlurOptions LayerSurface::popupBlurOptions() const {
    return SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(m_rule.ignoreAlpha.value_or(0.0)),
        .enabled = m_rule.blurPopups.value_or(false),
        .optimized = m_rule.optimized,
    };
  }

  void LayerSurface::unconstrainPopup(wlr_xdg_popup* popup) {
    Output* out = output();
    if (out == nullptr || m_scene == nullptr) {
      return;
    }

    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(m_layerSurface->output, &width, &height);

    // wlroots expects the box in the root layer surface coordinate system and
    // translates it through every ancestor popup itself.
    const wlr_box box = {
        .x = -m_scene->tree->node.x,
        .y = -m_scene->tree->node.y,
        .width = width,
        .height = height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
  }

  void LayerSurface::updateBlur() {
    const wlr_box box{0, 0, m_layerSurface->surface->current.width, m_layerSurface->surface->current.height};
    m_blur.update(m_scene->tree, m_layerSurface->surface, box, box, 0, nullptr, blurOptions());
  }

  void LayerSurface::applyConfig() {
    m_rule = resolveLayerRules(config(), m_layerSurface->namespace_);
    updateBlur();
  }

  void LayerSurface::onMap(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void LayerSurface::onUnmap(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void LayerSurface::onCommit(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void LayerSurface::onDestroy(wl_listener* listener, void* /*data*/) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void LayerSurface::onNewPopup(wl_listener* listener, void* data) {
    LayerSurface* self;
    self = wl_container_of(listener, self, m_newPopup);
    self->handleNewPopup(data);
  }

  void LayerSurface::handleMap() {
    m_mapped = true;
    m_server->updateIdleInhibit();
    if (Output* out = output()) {
      out->markDirty(Dirty::LayerArrange);
      if (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        out->markBlurBackgroundDirty();
      }
    }
    // Protocol: exclusive must receive keyboard focus. On-demand is click-to-focus.
    if (exclusiveKeyboard()) {
      focus();
    }
    updateBlur();

    const auto& animation = config().animation;
    const auto& layers = animation.layers;
    if (animation.enabled && layers.enabled) {
      m_fade.snap(0.0);
      applyFadeAlpha();
      m_fade.retarget(1.0, layers.durationMs, layers.curve);
      if (Output* out = output()) {
        wlr_output_schedule_frame(out->wlr());
      }
    } else {
      m_fade.snap(1.0);
      applyFadeAlpha();
    }
  }

  void LayerSurface::handleUnmap() {
    // Snapshot before any other unmap bookkeeping runs: the live buffer is still valid here.
    beginCloseAnimation();
    const bool hadFocus = hasKeyboardFocus();
    m_mapped = false;
    m_server->updateIdleInhibit();
    m_blur.hide();
    m_fade.snap(1.0);
    // Avoid sending configures while unmapping (wrong serial / client abort).
    m_arrangingOut = true;
    if (Output* out = output()) {
      out->markDirty(Dirty::LayerArrange);
      if (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
        out->markBlurBackgroundDirty();
      }
    }
    m_arrangingOut = false;
    if (hadFocus) {
      m_server->refocus();
    }
  }

  void LayerSurface::handleCommit() {
    updateBlur();
    if (Output* out = output(); out != nullptr
        && (m_layerSurface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND
            || (m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) != 0)) {
      out->markBlurBackgroundDirty();
    }
    if (m_layerSurface->initial_commit) {
      if (Output* out = output()) {
        out->arrangeLayers();
        out->markDirty(Dirty::LayerArrange);
      }
      return;
    }

    if ((m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) != 0) {
      reparentToLayer(m_layerSurface->current.layer);
    }

    if ((m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY) != 0) {
      if (exclusiveKeyboard()) {
        focus();
      } else if (hasKeyboardFocus() && !acceptsKeyboard()) {
        m_server->refocus();
      }
    }

    if (m_layerSurface->current.committed != 0) {
      if (Output* out = output()) {
        out->markDirty(Dirty::LayerArrange);
      }
    }
  }

  void LayerSurface::handleDestroy() {
    Server* server = m_server;
    wlr_output* wlrOutput = m_layerSurface != nullptr ? m_layerSurface->output : nullptr;
    kLog.debug("destroy layer surface output={}", wlrOutput != nullptr ? wlrOutput->name : "(none)");

    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_newPopup.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_commit.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_newPopup.link.next = nullptr;

    // m_scene is already freed: wlr_scene_layer_surface_v1_create registers its own destroy listener on
    // m_layerSurface->events.destroy before ours, and that handler destroys the tree and frees the helper.
    m_scene = nullptr;
    if (m_layerSurface != nullptr) {
      m_layerSurface->data = nullptr;
    }
    // removeLayerSurface deletes this. Arrange after erase, never via this->m_server.
    server->removeLayerSurface(this, wlrOutput);
  }

  void LayerSurface::handleNewPopup(void* data) {
    auto* popup = static_cast<wlr_xdg_popup*>(data);
    // Keep popup scene under the layer surface so geometry stays surface-local.
    new Popup(popup, m_scene->tree);
  }

} // namespace umbriel
