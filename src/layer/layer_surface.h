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
    [[nodiscard]] static LayerSurface* fromSurface(wlr_surface* surface);

    LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface);
    ~LayerSurface() override;

    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    [[nodiscard]] wlr_layer_surface_v1* layerSurface() const { return m_layerSurface; }
    [[nodiscard]] wlr_scene_layer_surface_v1* scene() const { return m_scene; }
    [[nodiscard]] bool mapped() const { return m_mapped; }
    [[nodiscard]] bool arrangingOut() const { return m_arrangingOut; }
    [[nodiscard]] bool exclusiveKeyboard() const;
    [[nodiscard]] bool acceptsKeyboard() const;
    [[nodiscard]] bool hasKeyboardFocus() const;
    [[nodiscard]] SurfaceBlurOptions blurOptions() const;
    [[nodiscard]] SurfaceBlurOptions popupBlurOptions() const;

    void focus();
    // A grabbing xdg_popup of this layer mapped. wlroots routes the grab's keys to the seat's focused surface and
    // never moves focus itself, so an on-demand layer that does not hold the keyboard yet takes it here.
    void focusForPopupGrab();
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Push the owning output's scale to every surface of this layer surface
    // (fractional-scale + preferred buffer scale, popups included).
    void notifyOutputScale();
    [[nodiscard]] Output* output() const;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return m_fade.animating(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override;

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
