#pragma once

#include "scene/node.h"
#include "scene/surface_blur.h"
#include "view/chrome_attachment.h"
#include "view/effects.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_border;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  class Output;
  class Server;
  class View;

  // The scene buffer inside `node` that shows `surface`, or null.
  wlr_scene_buffer* sourceBufferForSurface(wlr_scene_node* node, wlr_surface* surface);

  // One scaled presentation of one view under a caller-given parent tree: the window's surfaces, its border, shadow,
  // blur, corners, chrome, and effect bindings. The caller decides where and how large it is. It rejects pointer input
  // unless created interactive, when its surfaces resolve to the real ones and its title bar takes the pointer.
  class ViewPreview {
  public:
    // A surface of the view, or of a dialog attached to it, mirrored by one scene buffer of the preview. The base's
    // `surface` is the mirrored one.
    struct Surface : PreviewSurfaceNode {
      ViewPreview* preview = nullptr;
      // The window whose surface tree holds `surface`: the preview's own, or a dialog attached to it.
      View* view = nullptr;
      wlr_scene_buffer* sourceBuffer = nullptr;
      wlr_scene_buffer* buffer = nullptr;
      // The part of the surface, in surface coordinates, that `buffer` shows over its destination size.
      wlr_fbox crop{};
      int sx = 0;
      int sy = 0;
      bool isRoot = false;
      // Whether the latest walk of the preview's surfaces reached `surface`. A surface that left the tree keeps its
      // entry until destroyed, and `sourceBuffer` may already be freed with its subsurface role.
      bool inTree = true;
      wl_listener commit{};
      wl_listener destroy{};
      wl_listener outputSample{};
      wl_listener frameDone{};
    };

    // What drives the view's animation effects on the preview. The preview fills in whether it is decorated.
    struct EffectContext {
      BorderEffectGate gate;
      Output* output = nullptr;
      float scale = 1.0F;
    };

    struct Placement {
      // Content box relative to the parent tree: `x`, `y` position the root tree, `width`, `height` are the scaled
      // content size from contentSize().
      wlr_box box{};
      // How much smaller than the view the preview is drawn.
      double zoom = 1.0;
      // Opacity multiplier on top of the view's own.
      float opacity = 1.0F;
      // Straight-alpha color of the border's inner ring.
      std::array<float, 4> innerBorderColor{};
      // Opacity multiplier of the shadow; 0 hides it.
      float shadowAlpha = 0.0F;
      // Tree holding the shadow of a view whose shadow is pooled below every tile, at the parent's origin; null for no
      // pool.
      wlr_scene_tree* shadowPool = nullptr;
      EffectContext effects;
    };

    // Builds the preview under `parent`, or returns null when its scene nodes cannot be created. `onChanged` runs after
    // a surface commit has refreshed the preview's surfaces, so the owner can lay it out again; it may destroy the
    // preview.
    static std::unique_ptr<ViewPreview> create(
        Server& server, View* view, wlr_scene_tree* parent, std::function<void()> onChanged, bool interactive = false
    );
    // Resolves a scene buffer that shows a surface of a preview or a mirror to that surface and its window, which is
    // null for a surface of no window; `surface` is null when that window takes no pointer. False for any other buffer.
    static bool resolveBuffer(wlr_scene_buffer* buffer, wlr_surface** surface, View** view);
    ~ViewPreview();
    ViewPreview(const ViewPreview&) = delete;
    ViewPreview& operator=(const ViewPreview&) = delete;

    // Hides the shadow. When the view has nothing to present, also hides the preview, binds its effects, and returns
    // false; otherwise enables the root tree.
    bool prepare(const EffectContext& effects);
    // Hides the preview and binds its effects.
    void hide(const EffectContext& effects);
    // The scaled size of the view's presented box at `zoom`.
    wlr_box contentSize(double zoom) const;
    // Lays the preview out at `placement`; follows a true prepare().
    void layout(const Placement& placement);
    void syncEffects(const EffectContext& effects);
    // Destroys the shadow, which the next layout creates again.
    void releaseShadow();

    // Mirrors the surfaces of the view and its attached dialogs again, creating and retiring entries.
    void syncSurfaces();
    // Drops the entries of attached dialogs; the view's own stay.
    void dropAttachedSurfaces();

    wlr_scene_tree* tree() const { return m_tree; }
    wlr_scene_tree* surfaceTree() const { return m_surfaceTree; }
    wlr_scene_border* border() const { return m_border; }
    // Effect padding the border ring was last laid out with, scaled.
    int borderPadding() const { return m_borderPadding; }
    const ViewChromeAttachment* frame() const { return m_frame.get(); }
    const std::vector<std::unique_ptr<Surface>>& surfaces() const { return m_surfaces; }

  private:
    // One walk over the preview's surfaces, for one window of it, offset from the preview window's root surface.
    struct Walk {
      ViewPreview* preview = nullptr;
      View* view = nullptr;
      int dx = 0;
      int dy = 0;
    };

    ViewPreview(Server& server, View* view, std::function<void()> onChanged);

    static void onSurfaceCommit(wl_listener* listener, void* data);
    static void onSurfaceDestroy(wl_listener* listener, void* data);
    static void onBufferOutputSample(wl_listener* listener, void* data);
    static void onBufferFrameDone(wl_listener* listener, void* data);
    // The window's surfaces, then those of every dialog attached to it at their offset over the window.
    void forEachSurface(void (*visit)(wlr_surface*, int, int, void*));
    static void addSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void syncSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void syncBuffer(Surface& entry);
    static void removeListeners(Surface& entry);

    // Lays the chrome copy over `world`, the view's presented box, or drops it with the view's chrome.
    void layoutChrome(const wlr_box& world, int borderInset, double zoom, float alpha, Output* output);
    void layoutShadow(const Placement& placement);

    Server* m_server = nullptr;
    View* m_view = nullptr;
    bool m_interactive = false;
    std::function<void()> m_onChanged;
    wlr_scene_tree* m_tree = nullptr;
    // The root tree's tag while interactive.
    PreviewRootNode m_rootTag;
    // Parent of every surface buffer, at the preview origin: the window and overlay effect slots live here, as they
    // live on the view's surface tree.
    wlr_scene_tree* m_surfaceTree = nullptr;
    wlr_scene_border* m_border = nullptr;
    int m_borderPadding = 0;
    SurfaceBlur m_blur;
    // Scaled copy of the view's drop shadow, under the root tree or in the placement's pool.
    wlr_scene_tree* m_shadowTree = nullptr;
    wlr_scene_shadow* m_shadow = nullptr;
    // Scaled copy of the chrome the view carries beside its borders, such as a tab bar, under the root tree.
    std::unique_ptr<ViewChromeAttachment> m_chrome;
    std::unique_ptr<ViewChromeAttachment> m_frame;
    std::vector<std::unique_ptr<Surface>> m_surfaces;
  };

} // namespace umbriel
