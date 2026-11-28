#pragma once

#include "config/config.h"
#include "core/animation.h"
#include "scene/node.h"
#include "scene/surface_blur.h"

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_xdg_popup;

namespace umbriel {

  class Output;
  class Server;

  class LayerSurface : public SceneNode, public Animatable {
  public:
    // Resolve a layer owner through subsurfaces and nested XDG popups.
    static LayerSurface* fromSurface(wlr_surface* surface);

    LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface);
    ~LayerSurface() override;

    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    wlr_layer_surface_v1* layerSurface() const { return m_layerSurface; }
    wlr_scene_layer_surface_v1* scene() const { return m_scene; }
    bool mapped() const { return m_mapped; }
    bool arrangingOut() const { return m_arrangingOut; }
    bool exclusiveKeyboard() const;
    bool acceptsKeyboard() const;
    bool hasKeyboardFocus() const;
    SurfaceBlurOptions blurOptions() const;
    SurfaceBlurOptions popupBlurOptions() const;

    void focus();
    // A grabbing xdg_popup of this layer mapped. wlroots routes the grab's keys to the seat's focused surface and
    // never moves focus itself, so an on-demand layer that does not hold the keyboard yet takes it here.
    void focusForPopupGrab();
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Push the owning output's scale to every surface of this layer surface
    // (fractional-scale + preferred buffer scale, popups included).
    void notifyOutputScale();
    // Whether this layer surface renders at its output's render_scale. Only a `supersample` layer rule opts out.
    bool supersampled() const { return m_rule.supersample.value_or(true); }
    void updateDimBehind();
    Output* output() const;

    AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    bool hasActiveAnimations() const override { return m_fade.animating(); }
    bool animatesOn(const Output* output) const override;

  private:
    friend class Server;

    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onCommit(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onNewPopup(wl_listener* listener, void* data);
    static void onSceneUpdate(wl_listener* listener, void* data);
    static void onSceneDestroy(wl_listener* listener, void* data);

    void handleMap();
    void handleUnmap();
    void handleCommit();
    void handleDestroy();
    void handleNewPopup(void* data);
    void reparentToLayer(uint32_t layer);
    void syncPopupTree();
    // Tell an open overview to re-resolve this output's mirrored background stack.
    void notifyDesktopStack();
    void updateBlur();
    // The rules `layer` matches. The constructor passes the requested layer, which the surface has not committed yet.
    [[nodiscard]] ResolvedLayerRule resolveRule(uint32_t layer) const;
    // Re-resolve the layer rules and re-send the scale hints when `supersample` flips.
    void resolveRules();
    void applyConfig();
    void refreshStartupRuleEffects();
    void applyFadeAlpha();
    void beginCloseAnimation(wlr_scene_tree* tree);

    Server* m_server = nullptr;
    wlr_layer_surface_v1* m_layerSurface = nullptr;
    wlr_scene_layer_surface_v1* m_scene = nullptr;
    wlr_scene_tree* m_popupTree = nullptr;
    SurfaceBlur m_blur;
    ResolvedLayerRule m_rule;
    AnimatedValue m_fade{1.0};
    bool m_mapped = false;
    bool m_arrangingOut = false;

    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_commit{};
    wl_listener m_destroy{};
    wl_listener m_newPopup{};
    wl_listener m_sceneUpdate{};
    wl_listener m_sceneDestroy{};
  };

} // namespace umbriel
