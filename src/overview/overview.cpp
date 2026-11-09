#include "overview/overview.h"

#include "overview/preview_geometry.h"
extern "C" {
#include <umbrielfx/render/effect.h>
}

#include "config/config.h"
#include "core/log.h"
#include "core/tracy.h"
#include "input/cursor.h"
#include "input/event_time.h"
#include "input/gesture_physics.h"
#include "input/gestures.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/shortcut_labels.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/ssd_frame.h"
#include "scene/text_buffer.h"
#include "server/color_manager.h"
#include "server/focus.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <format>
#include <ranges>
#include <tuple>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "workspace/workspace_mirror.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("overview");
    constexpr double kMiddleScrollStepPx = 105.0;

    // Overview progress over which the desktop's shadows and pinned windows fade, starting from the desktop.
    constexpr double kDesktopChromeFade = 0.1;
    // Pointer travel that promotes a press on a card into a relocate drag.
    constexpr double kDragThreshold = 10.0;
    // How much of the focused border color mixes into the unfocused one for a landing target that is not the live one.
    constexpr float kLandingTargetBlend = 0.4F;
    // Inset of a shortcut badge from the card edge it hugs.
    constexpr int kBadgeMargin = 6;

    std::array<float, 4> mixColor(const std::array<float, 4>& from, const std::array<float, 4>& to, float amount) {
      std::array<float, 4> out{};
      for (size_t index = 0; index < out.size(); ++index) {
        out[index] = std::lerp(from[index], to[index], amount);
      }
      return out;
    }

    bool boxContains(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    // The mapping a mirror row places its source's windows with; empty while it has no source or no output size.
    std::optional<MirrorMapping> mirrorRowMapping(const Workspace& row) {
      const WorkspaceMirror* renderer = row.mirrorRenderer();
      return renderer != nullptr ? renderer->mapping() : std::nullopt;
    }

    // A scene buffer's opaque region is in its destination coordinates, so a scaled copy of a surface needs the
    // surface's region scaled with it. Edges round inward: a pixel the opaque area only partly covers stays
    // see-through.
    void setScaledOpaqueRegion(
        wlr_scene_buffer* buffer, const pixman_region32_t& source, int sourceWidth, int sourceHeight, int destWidth,
        int destHeight
    ) {
      pixman_region32_t scaled;
      pixman_region32_init(&scaled);
      if (sourceWidth > 0 && sourceHeight > 0) {
        const double sx = static_cast<double>(destWidth) / sourceWidth;
        const double sy = static_cast<double>(destHeight) / sourceHeight;
        int count = 0;
        const pixman_box32_t* rects = pixman_region32_rectangles(&source, &count);
        for (int index = 0; index < count; ++index) {
          const auto x1 = static_cast<int>(std::ceil(rects[index].x1 * sx));
          const auto y1 = static_cast<int>(std::ceil(rects[index].y1 * sy));
          const auto x2 = static_cast<int>(std::floor(rects[index].x2 * sx));
          const auto y2 = static_cast<int>(std::floor(rects[index].y2 * sy));
          if (x2 > x1 && y2 > y1) {
            pixman_region32_union_rect(&scaled, &scaled, x1, y1, x2 - x1, y2 - y1);
          }
        }
      }
      wlr_scene_buffer_set_opaque_region(buffer, &scaled);
      pixman_region32_fini(&scaled);
    }

    void layoutWorkspaceBackground(
        wlr_scene_rect* background, const wlr_box& box, int radius, const std::array<float, 4>& color
    ) {
      if (background == nullptr) {
        return;
      }
      if (color[3] <= 0.001F) {
        wlr_scene_node_set_enabled(&background->node, false);
        return;
      }

      wlr_scene_node_set_enabled(&background->node, true);
      wlr_scene_node_set_position(&background->node, box.x, box.y);
      wlr_scene_rect_set_size(background, box.width, box.height);
      wlr_scene_rect_set_color(background, color.data());
      wlr_scene_rect_set_clipped_region(background, clipped_region_get_default());
      wlr_scene_rect_set_corner_radii(background, corner_radii_all(radius));
    }

    // Cards are pure output: hit testing runs off Overview's own boxes, so scene
    // input must never land on them (Server::viewAt then sees layer surfaces only).
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

    char asciiLower(char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    char shortcutCharacter(uint32_t keysym) {
      if (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_9) {
        return static_cast<char>('0' + (keysym - XKB_KEY_KP_0));
      }
      const xkb_keysym_t lowered = xkb_keysym_to_lower(keysym);
      return lowered >= 0x21 && lowered <= 0x7E ? static_cast<char>(lowered) : '\0';
    }

    bool shortcutStartsWith(std::string_view label, std::string_view prefix) {
      if (label.size() < prefix.size()) {
        return false;
      }
      for (size_t index = 0; index < prefix.size(); ++index) {
        if (asciiLower(label[index]) != prefix[index]) {
          return false;
        }
      }
      return true;
    }
  } // namespace

  Overview::Overview(Server& server) : m_server(&server) { m_server->registerAnimatable(this); }

  Overview::~Overview() {
    m_server->unregisterAnimatable(this);
    m_zoomAnim.snap(0.0);
    teardown();
  }

  double Overview::settledZoom() { return std::clamp(config().overview.zoom, 0.1, 0.75); }

  double Overview::zoom() const { return 1.0 - m_progress * (1.0 - settledZoom()); }

  float Overview::desktopChromeAlpha() const {
    return static_cast<float>(std::clamp(1.0 - m_progress / kDesktopChromeFade, 0.0, 1.0));
  }

  double Overview::zoomPixelsPerUnit() const {
    // A point `d` along an output extent `E` sits at `base + d * z`, with `base` centering the preview, so it moves
    // at most E / 2 per unit of zoom. Neighbouring rows add a whole step, E * (1 + gap), but they only show near the
    // open end; a close ends with them off the output.
    const double rowFactor = m_targetProgress < m_progressFrom ? 0.5 : 1.5 + kPreviewRowGapFraction;
    double extent = 0.0;
    for (const auto& state : m_outputs) {
      wlr_box box{};
      wlr_output_layout_get_box(m_server->outputLayout(), state->output->wlr(), &box);
      extent = std::max(extent, static_cast<double>(std::max(box.width, box.height)));
    }
    const double zoomSpan = std::abs(m_targetProgress - m_progressFrom) * (1.0 - settledZoom());
    // Whole pixels per unit keep the target (1.0) on the pixel grid finishSpringTail rounds against.
    return std::ceil(zoomSpan * extent * rowFactor);
  }

  // -: geometry

  bool Overview::previewMetrics(const OutputState& state, const Server& server, double zoom, PreviewMetrics& out) {
    wlr_box outputBox{};
    wlr_output_layout_get_box(server.outputLayout(), state.output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }
    const WorkspaceGroup* group = state.output->workspaceGroup();
    out.outputBox = outputBox;
    out.usableBox = state.output->usableArea();
    if (out.usableBox.width <= 0 || out.usableBox.height <= 0) {
      out.usableBox = outputBox;
    }
    out.zoom = zoom;
    out.axis = group != nullptr ? group->workspaceAxis() : WorkspaceAxis::Vertical;
    const PreviewGrid grid = previewGrid(
        outputBox.x, outputBox.y, outputBox.width, outputBox.height, zoom, out.axis == WorkspaceAxis::Horizontal
    );
    out.previewW = grid.width;
    out.previewH = grid.height;
    out.baseX = grid.baseX;
    out.baseY = grid.baseY;
    out.gap = grid.gap;
    return true;
  }

  wlr_box Overview::previewBox(const PreviewMetrics& metrics, double workspaceScroll, size_t workspaceIndex) {
    const bool horizontal = metrics.axis == WorkspaceAxis::Horizontal;
    const PreviewGrid grid{
        .width = metrics.previewW,
        .height = metrics.previewH,
        .baseX = metrics.baseX,
        .baseY = metrics.baseY,
        .gap = metrics.gap,
    };
    const int origin = previewAxisOrigin(grid, horizontal, workspaceIndex, workspaceScroll);
    return {
        .x = horizontal ? origin : metrics.baseX,
        .y = horizontal ? metrics.baseY : origin,
        .width = metrics.previewW,
        .height = metrics.previewH,
    };
  }

  std::optional<MirrorMapping> Overview::cardMapping(const Card& card) {
    return card.row->mirror() ? mirrorRowMapping(*card.row) : std::nullopt;
  }

  double Overview::cardFit(const Card& card) {
    const std::optional<MirrorMapping> mapping = cardMapping(card);
    return mapping ? mapping->fit.scale : 1.0;
  }

  void Overview::layoutCard(
      Card& card, const PreviewMetrics& metrics, double workspaceScroll, const LiveTarget& liveTarget
  ) {
    View* view = card.view;
    ViewPreview& preview = *card.preview;
    const bool dragged = &card == m_dragCard;
    // The lifted window moves with the pointer on one card; its other cards would stay behind at stale positions.
    const auto hideCard = [&] {
      preview.hide(cardEffects(card));
      preview.releaseShadow();
      if (card.badge != nullptr) {
        wlr_scene_node_set_enabled(&card.badge->node, false);
      }
    };
    if (m_dragCard != nullptr && card.view == m_dragCard->view && !dragged) {
      hideCard();
      return;
    }
    // A mirror row draws the window where its mirror places it on the receiving output, smaller by the mirror's fit.
    wlr_box world = view->presentedBox();
    double fit = 1.0;
    if (card.row->mirror()) {
      const std::optional<MirrorMapping> mapping = cardMapping(card);
      if (!mapping) {
        hideCard();
        return;
      }
      world = mapping->toReceiver(world);
      fit = mapping->fit.scale;
    }
    const ViewPreview::EffectContext effects = cardEffects(card);
    if (!preview.prepare(effects)) {
      return;
    }

    const double z = metrics.zoom * fit;
    const wlr_box content = preview.contentSize(z);
    const int contentW = content.width;
    const int contentH = content.height;
    if (dragged) {
      // The drag owns the card origin; only the scale still tracks progress.
      card.box.width = contentW;
      card.box.height = contentH;
    } else {
      const wlr_box previewArea = previewBox(metrics, workspaceScroll, card.workspaceIndex);
      card.box = {
          .x = previewArea.x + static_cast<int>(std::lround((world.x - metrics.outputBox.x) * metrics.zoom)),
          .y = previewArea.y + static_cast<int>(std::lround((world.y - metrics.outputBox.y) * metrics.zoom)),
          .width = contentW,
          .height = contentH,
      };
    }
    preview.layout({
        .box = card.box,
        .zoom = z,
        .opacity = dragged ? static_cast<float>(config().appearance.dragOpacity) : 1.0F,
        .innerBorderColor = cardBorderColor(card, liveTarget),
        .shadowAlpha = dragged ? 0.0F : desktopChromeAlpha(),
        .shadowPool = card.owner->tileShadows,
        .effects = effects,
    });

    if (card.badge != nullptr) {
      // Overshooting curves can push m_progress past [0, 1] and wlr_scene_buffer_set_opacity asserts.
      const auto badgeAlpha = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      const bool matched = card.shortcutMatched != SIZE_MAX;
      const bool fits =
          contentW >= card.badgeWidth + 2 * kBadgeMargin && contentH >= card.badgeHeight + 2 * kBadgeMargin;
      const bool badgeOn = !card.shortcut.empty() && matched && fits && !m_closing && !dragged && badgeAlpha > 0.01F;
      wlr_scene_node_set_enabled(&card.badge->node, badgeOn);
      if (badgeOn) {
        // The badge hugs the card's top-left corner, the only corner the output clip or the top/overlay exclusive
        // zones can hide. On each axis it slides just far enough to clear the start of the usable area, never past
        // the card's opposite inset, where a card scrolled out of view keeps it. `fits` keeps the card-local bounds
        // ordered.
        const auto inset = [](int origin, int extent, int badgeExtent, int clipStart) {
          return std::clamp(
              std::max(clipStart, origin) + kBadgeMargin - origin, kBadgeMargin, extent - badgeExtent - kBadgeMargin
          );
        };
        wlr_scene_node_set_position(
            &card.badge->node, inset(card.box.x, contentW, card.badgeWidth, metrics.usableBox.x),
            inset(card.box.y, contentH, card.badgeHeight, metrics.usableBox.y)
        );
        wlr_scene_buffer_set_opacity(card.badgeText, badgeAlpha);
        const std::array<float, 4> background = tint(card.badgeBackground, badgeAlpha);
        wlr_scene_rect_set_color(card.badgeRect, background.data());
        // The badge renders unscaled, so it takes the zoomed radius the cards
        // around it use instead of the full-size one.
        const int scaledRadius = static_cast<int>(std::lround(view->decorationCornerRadius() * z));
        wlr_scene_rect_set_corner_radius(
            card.badgeRect, std::min(scaledRadius, std::min(card.badgeWidth, card.badgeHeight) / 2)
        );
      }
    }
  }

  void Overview::layoutOutput(OutputState& state) {
    UMBRIEL_ZONE("Overview::layoutOutput");
    PreviewMetrics metrics{};
    if (!previewMetrics(state, *m_server, zoom(), metrics)) {
      return;
    }

    // Previews overhang the output by design (adjacent workspaces peek in). One clip on this output's overview tree
    // contains every card, ring, and workspace background, so none needs to trim its own geometry. The dragged card
    // is reparented out to the unclipped overview root, which is what lets it span outputs.
    wlr_scene_tree_set_clip(state.tree, &metrics.outputBox);
    if (state.backgroundBlur != nullptr) {
      wlr_scene_blur* blur = state.backgroundBlur;
      wlr_scene_node_set_enabled(&blur->node, m_progress > 0.001);
      wlr_scene_node_set_position(&blur->node, metrics.outputBox.x, metrics.outputBox.y);
      if (blur->width != metrics.outputBox.width || blur->height != metrics.outputBox.height) {
        wlr_scene_blur_set_size(blur, metrics.outputBox.width, metrics.outputBox.height);
      }
      // Blur alpha and strength are normalized to [0, 1]; clamp an overshooting progress.
      const auto level = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      if (blur->alpha != level) {
        wlr_scene_blur_set_alpha(blur, level);
      }
      if (blur->strength != level) {
        wlr_scene_blur_set_strength(blur, level);
      }
    }

    wlr_scene_node_set_position(&state.backgroundTint->node, metrics.outputBox.x, metrics.outputBox.y);
    wlr_scene_rect_set_size(state.backgroundTint, metrics.outputBox.width, metrics.outputBox.height);
    const std::array<float, 4> backgroundTint = tint(config().colors.overview.backgroundTint, m_progress);
    // A fully transparent tint leaves the wallpaper untouched.
    wlr_scene_node_set_enabled(&state.backgroundTint->node, backgroundTint[3] > 0.001F);
    wlr_scene_rect_set_color(state.backgroundTint, backgroundTint.data());

    refreshDesktop(state);
    const int backgroundRadius = static_cast<int>(std::lround(config().appearance.cornerRadius * metrics.zoom));
    const std::array<float, 4> backgroundColor = tint(config().colors.overview.workspaceBackground, m_progress);
    const double z = metrics.zoom;
    for (size_t index = 0; index < state.workspaceBackgrounds.size(); ++index) {
      const WorkspaceBackground& background = state.workspaceBackgrounds[index];
      const wlr_box full = previewBox(metrics, state.rowScroll.current(), index);
      layoutWorkspaceBackground(background.fill, full, backgroundRadius, backgroundColor);
      if (background.mirrors.empty()) {
        continue;
      }
      // The preview is the viewport onto the mirrored stack: a partially anchored surface must not reach into the
      // gap between previews.
      wlr_scene_tree_set_clip(background.tree, &full);
      for (const auto& mirror : background.mirrors) {
        wlr_box box{};
        const bool visible = desktopSourceBox(*mirror->source, box);
        wlr_scene_node_set_enabled(&mirror->buffer->node, visible);
        if (!visible) {
          continue;
        }
        wlr_scene_node_set_position(
            &mirror->buffer->node, full.x + static_cast<int>(std::lround(box.x * z)),
            full.y + static_cast<int>(std::lround(box.y * z))
        );
        const int destWidth = std::max(1, static_cast<int>(std::lround(box.width * z)));
        const int destHeight = std::max(1, static_cast<int>(std::lround(box.height * z)));
        wlr_scene_buffer_set_dest_size(mirror->buffer, destWidth, destHeight);
        // Lets the scene skip the background beneath a wallpaper that declares itself opaque.
        setScaledOpaqueRegion(
            mirror->buffer, mirror->source->surface->opaque_region, box.width, box.height, destWidth, destHeight
        );
        // Only a surface spanning the whole output takes the preview's rounding; a smaller one keeps its own edges.
        const bool spansOutput = box.x <= 0
            && box.y <= 0
            && box.x + box.width >= metrics.outputBox.width
            && box.y + box.height >= metrics.outputBox.height;
        wlr_scene_buffer_set_corner_radii(mirror->buffer, corner_radii_all(spansOutput ? backgroundRadius : 0));
      }
    }

    const LiveTarget live = liveTarget();
    for (const auto& card : state.cards) {
      layoutCard(*card, metrics, state.rowScroll.current(), live);
    }
  }

  void Overview::applyPinnedOpacity(float alpha) const {
    // A window unpinned while the overview is up becomes a card, so it returns to full opacity underneath.
    for (const auto& view : m_server->registry().all()) {
      view->setOverviewOpacity(view->pinned() ? alpha : 1.0F);
    }
  }

  Overview::LiveTarget Overview::liveTarget() const {
    const Workspace* row = preferredWorkspace();
    return {.view = row != nullptr ? row->focusedView() : nullptr, .row = row};
  }

  void Overview::activateLiveTarget() {
    if (View* target = liveTarget().view) {
      target->placementRoot()->inputDelegate()->applySeatFocus(false, false);
    } else {
      m_server->deactivateViews();
    }
  }

  ViewPreview::EffectContext Overview::cardEffects(const Card& card) const {
    const View* view = card.view;
    const Workspace* workspace = view->workspace();
    return {
        .gate =
            {
                .focused = workspace != nullptr && workspace->focusedView() == view && &card != m_dragCard,
                .allowUnfocused = config().effects.borderUnfocused,
                .urgent = view->urgent(),
                .fullscreen = view->scheduledFullscreen(),
            },
        .output = card.owner->output,
        .scale = static_cast<float>(zoom() * cardFit(card)),
    };
  }

  void Overview::syncCardEffects(Card& card) { card.preview->syncEffects(cardEffects(card)); }

  void Overview::refreshEffectBindings() {
    if (m_active) {
      syncCardEffects();
    }
  }

  void Overview::syncCardEffects() {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        syncCardEffects(*card);
      }
    }
  }

  std::array<float, 4> Overview::cardBorderColor(const Card& card, const LiveTarget& liveTarget) const {
    const auto& border = card.view != nullptr ? card.view->borderColors() : config().colors.border;
    const Workspace* workspace = card.view != nullptr ? card.view->workspace() : nullptr;
    if (workspace == nullptr || workspace->focusedView() != card.view || &card == m_dragCard) {
      return border.unfocused;
    }
    // No window holds the seat while the overview is up, so exactly one card wears the focused color: the one a focus
    // or close action would act on, on the row it would act from. A mirror row shows the same window as its source, so
    // only the row of the active workspace on the pointer's output wears it. Every other row still marks the card it
    // would land on, with a weaker mix.
    if (card.view == liveTarget.view && card.row == liveTarget.row) {
      return border.focused;
    }
    return mixColor(border.unfocused, border.focused, kLandingTargetBlend);
  }

  void Overview::applyProgress() {
    if (m_shortcutsDirty) {
      m_shortcutsDirty = false;
      updateShortcutAssignments();
    }
    for (const auto& state : m_outputs) {
      layoutOutput(*state);
    }
    // Pinned windows get no card: the real ones fade over the filmstrip instead of blinking at either end.
    const float chrome = desktopChromeAlpha();
    applyPinnedOpacity(chrome);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, chrome > 0.0F);
    scheduleFrames();
  }

  void Overview::scheduleFrames() const {
    for (const auto& state : m_outputs) {
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  // -: background mirrors

  void Overview::collectDesktopSurfaces(const Output& output, std::vector<DesktopEntry>& out) const {
    out.clear();
    if (!config().overview.workspaceWallpaper) {
      return;
    }
    // Scene child order is bottom to top, so walking the two layer trees in place reproduces what renders below the
    // windows. Children that are not a mapped layer surface (an unmap snapshot, for instance) have no surface to
    // mirror and drop out here.
    for (const uint32_t layer : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
      wlr_scene_tree* layerTree = output.layerTree(layer);
      if (layerTree == nullptr) {
        continue;
      }
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &layerTree->children, link) {
        if (child->type != WLR_SCENE_NODE_TREE || !child->enabled) {
          continue;
        }
        wlr_scene_tree* tree = wlr_scene_tree_from_node(child);
        for (const auto& candidate : m_server->layerSurfaces()) {
          if (!candidate->mapped() || candidate->scene() == nullptr || candidate->scene()->tree != tree) {
            continue;
          }
          wlr_surface* surface = candidate->layerSurface()->surface;
          if (surface->buffer == nullptr || surface->current.width <= 0 || surface->current.height <= 0) {
            break;
          }
          out.push_back({.surface = surface, .tree = tree});
          break;
        }
      }
    }
  }

  void Overview::clearDesktop(OutputState& state) const {
    for (const auto& source : state.desktop) {
      wl_list_remove(&source->commit.link);
      wl_list_remove(&source->destroy.link);
    }
    state.desktop.clear();
    for (WorkspaceBackground& background : state.workspaceBackgrounds) {
      for (const auto& mirror : background.mirrors) {
        wl_list_remove(&mirror->outputSample.link);
        wl_list_remove(&mirror->frameDone.link);
        if (mirror->buffer != nullptr) {
          wlr_scene_node_destroy(&mirror->buffer->node);
        }
      }
      background.mirrors.clear();
    }
  }

  void Overview::refreshDesktop(OutputState& state) {
    std::vector<DesktopEntry> resolved;
    collectDesktopSurfaces(*state.output, resolved);
    bool same = resolved.size() == state.desktop.size();
    for (size_t index = 0; same && index < resolved.size(); ++index) {
      same = state.desktop[index]->surface == resolved[index].surface;
    }
    if (same) {
      // A surface keeps its scene tree, but the tree can be reparented between layers, so refresh the pointer the
      // mirror geometry reads.
      for (size_t index = 0; index < resolved.size(); ++index) {
        state.desktop[index]->tree = resolved[index].tree;
      }
      return;
    }

    clearDesktop(state);
    state.desktop.reserve(resolved.size());
    for (const DesktopEntry& entry : resolved) {
      auto source = std::make_unique<DesktopSurface>();
      source->overview = this;
      source->state = &state;
      source->surface = entry.surface;
      source->tree = entry.tree;
      source->commit.notify = onDesktopSurfaceCommit;
      wl_signal_add(&entry.surface->events.commit, &source->commit);
      source->destroy.notify = onDesktopSurfaceDestroy;
      wl_signal_add(&entry.surface->events.destroy, &source->destroy);
      state.desktop.push_back(std::move(source));
    }
    for (WorkspaceBackground& background : state.workspaceBackgrounds) {
      createRowMirrors(state, background);
    }
    syncDesktopMirrors(state);
  }

  void Overview::createRowMirrors(OutputState& state, WorkspaceBackground& background) const {
    if (background.tree == nullptr) {
      return;
    }
    background.mirrors.reserve(state.desktop.size());
    // Created in stack order, above the fill: scene child order is what keeps a bottom-layer copy over the wallpaper.
    for (const auto& source : state.desktop) {
      wlr_scene_buffer* buffer = wlr_scene_buffer_create(background.tree, nullptr);
      if (buffer == nullptr) {
        continue;
      }
      wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
      buffer->point_accepts_input = rejectInput;
      auto mirror = std::make_unique<DesktopMirror>();
      mirror->source = source.get();
      mirror->buffer = buffer;
      mirror->outputSample.notify = onDesktopMirrorOutputSample;
      wl_signal_add(&buffer->events.output_sample, &mirror->outputSample);
      mirror->frameDone.notify = onDesktopMirrorFrameDone;
      wl_signal_add(&buffer->events.frame_done, &mirror->frameDone);
      background.mirrors.push_back(std::move(mirror));
    }
  }

  void Overview::syncDesktopMirrors(const OutputState& state) const {
    for (const WorkspaceBackground& background : state.workspaceBackgrounds) {
      for (const auto& mirror : background.mirrors) {
        wlr_surface* surface = mirror->source->surface;
        // The surface holds the authoritative committed buffer; a scene buffer may already have released the client
        // one after importing its texture.
        wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
        wlr_scene_buffer_set_buffer_options options{
            .damage = committed != nullptr ? &surface->buffer_damage : nullptr,
            .wait_timeline = nullptr,
            .wait_point = 0,
        };
        if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
          options.wait_timeline = sync->acquire_timeline;
          options.wait_point = sync->acquire_point;
        }
        wlr_scene_buffer_set_buffer_with_options(mirror->buffer, committed, &options);
        if (committed == nullptr) {
          continue;
        }
        wlr_fbox src{};
        wlr_surface_get_buffer_source_box(surface, &src);
        wlr_scene_buffer_set_source_box(mirror->buffer, &src);
        wlr_scene_buffer_set_transform(mirror->buffer, surface->current.transform);
        // layoutOutput owns the destination geometry; only the color description follows the live node.
        if (wlr_scene_buffer* live = sourceBufferForSurface(&mirror->source->tree->node, surface)) {
          wlr_scene_buffer_copy_color(mirror->buffer, live);
        }
        if (ColorManager* colorManager = m_server->colorManager()) {
          colorManager->applySurfaceDescriptionToBuffer(surface, mirror->buffer);
        }
      }
    }
  }

  bool Overview::desktopSourceBox(const DesktopSurface& source, wlr_box& out) {
    // The output's layer trees sit at its origin and hold every layer surface as a direct child, so the child's own
    // offset is already output-local. Layout coordinates are unavailable here: the bottom layer is disabled while its
    // copies stand in, and wlr_scene_node_coords reports nothing through a disabled ancestor.
    if (!source.tree->node.enabled) {
      return false;
    }
    out = {source.tree->node.x, source.tree->node.y, source.surface->current.width, source.surface->current.height};
    return out.width > 0 && out.height > 0;
  }

  void Overview::onDesktopSurfaceCommit(wl_listener* listener, void* /*data*/) {
    DesktopSurface* source = nullptr;
    source = wl_container_of(listener, source, commit);
    source->overview->syncDesktopMirrors(*source->state);
    source->overview->layoutOutput(*source->state);
    wlr_output_schedule_frame(source->state->output->wlr());
  }

  void Overview::onDesktopSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    DesktopSurface* source = nullptr;
    source = wl_container_of(listener, source, destroy);
    OutputState* state = source->state;
    wl_list_remove(&source->commit.link);
    wl_list_remove(&source->destroy.link);
    // Drop this surface's copies only. Re-resolving here would re-bind a surface that is already being destroyed;
    // the next layout notices the shorter stack and rebuilds from what is left.
    for (WorkspaceBackground& background : state->workspaceBackgrounds) {
      std::erase_if(background.mirrors, [source](const std::unique_ptr<DesktopMirror>& mirror) {
        if (mirror->source != source) {
          return false;
        }
        wl_list_remove(&mirror->outputSample.link);
        wl_list_remove(&mirror->frameDone.link);
        if (mirror->buffer != nullptr) {
          wlr_scene_node_destroy(&mirror->buffer->node);
        }
        return true;
      });
    }
    std::erase_if(state->desktop, [source](const std::unique_ptr<DesktopSurface>& candidate) {
      return candidate.get() == source;
    });
    wlr_output_schedule_frame(state->output->wlr());
  }

  void Overview::onDesktopMirrorOutputSample(wl_listener* listener, void* data) {
    DesktopMirror* mirror = nullptr;
    mirror = wl_container_of(listener, mirror, outputSample);
    auto* event = static_cast<wlr_scene_output_sample_event*>(data);
    wlr_surface* surface = mirror->source->surface;
    // No wl_surface.enter here: a mirrored surface leaves its outputs exactly as a window behind a card does.
    if (event->direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(surface, event->output->output);
    } else {
      wlr_presentation_surface_textured_on_output(surface, event->output->output);
    }
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
        sync != nullptr && event->release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event->release_timeline, event->release_point, event->output->output->event_loop
      );
    }
  }

  void Overview::onDesktopMirrorFrameDone(wl_listener* listener, void* data) {
    DesktopMirror* mirror = nullptr;
    mirror = wl_container_of(listener, mirror, frameDone);
    auto* event = static_cast<wlr_scene_frame_done_event*>(data);
    // The real bottom layer is hidden, so these copies are what keeps its clients drawing. A second row's copy finds
    // the callback list already empty.
    wlr_surface_send_frame_done(mirror->source->surface, &event->when);
  }

  Overview::WorkspaceBackground Overview::createWorkspaceBackground(OutputState& state) const {
    WorkspaceBackground background{};
    background.tree = wlr_scene_tree_create(state.tree);
    if (background.tree == nullptr) {
      return background;
    }
    // Rows stay below every card even when created after the overview scene was populated.
    wlr_scene_node_place_above(&background.tree->node, &state.backgroundTint->node);
    const std::array<float, 4> color = tint(config().colors.overview.workspaceBackground, m_progress);
    background.fill = wlr_scene_rect_create(background.tree, 1, 1, color.data());
    createRowMirrors(state, background);
    return background;
  }

  // -: cards

  Overview::Card* Overview::createCard(OutputState& state, View* view, Workspace& row) {
    // An attached dialog shows over its parent's card instead of on one of its own.
    if (view == nullptr || !view->mapped() || view->pinned() || view->attached()) {
      return nullptr;
    }
    wlr_surface* surface = view->rootSurface();
    if (surface == nullptr) {
      return nullptr;
    }
    auto card = std::make_unique<Card>();
    card->owner = &state;
    card->view = view;
    card->row = &row;
    card->workspaceIndex = row.index();
    Card* raw = card.get();
    card->preview = ViewPreview::create(*m_server, view, state.tree, [this, raw] {
      // A surface commit refreshes the preview's surfaces; re-derive their crop and scale.
      if (!m_active || raw->owner == nullptr) {
        return;
      }
      PreviewMetrics metrics{};
      if (previewMetrics(*raw->owner, *m_server, zoom(), metrics)) {
        layoutCard(*raw, metrics, raw->owner->rowScroll.current(), liveTarget());
        wlr_output_schedule_frame(raw->owner->output->wlr());
      }
    });
    if (card->preview == nullptr) {
      return nullptr;
    }
    state.cards.push_back(std::move(card));
    m_shortcutsDirty = true;

    // Animation schedules the first output frame. The passive card buffers then
    // pace clients from frames where their content was actually sampled.
    m_server->scheduleSuspensionSync();
    return raw;
  }

  void Overview::snapshotCardForClose(Card& card) {
    if (card.preview == nullptr || card.owner == nullptr || card.owner->output == nullptr || card.view == nullptr) {
      return;
    }
    const ViewPreview& preview = *card.preview;
    wlr_scene_tree* const cardTree = preview.tree();
    wlr_scene_tree* const surfaceTree = preview.surfaceTree();
    wlr_scene_border* const border = preview.border();

    wlr_scene_tree* snapshot = wlr_scene_tree_create(m_tree);
    if (snapshot == nullptr) {
      return;
    }
    if (!umbrielfx_scene_node_share_capture_policy(&snapshot->node, &cardTree->node)) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), card.owner->output->wlr(), &outputBox);
    if (outputBox.width > 0 && outputBox.height > 0) {
      wlr_scene_tree_set_clip(snapshot, &outputBox);
    }

    // The copied surfaces keep the card's window and overlay slots, with time frozen, on a tree of their own.
    wlr_scene_tree* surfaces = wlr_scene_tree_create(snapshot);
    if (surfaces == nullptr) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    wlr_scene_node_set_position(
        &surfaces->node, cardTree->node.x + surfaceTree->node.x, cardTree->node.y + surfaceTree->node.y
    );
    int buffersCopied = 0;
    for (const auto& entry : preview.surfaces()) {
      wlr_scene_buffer* source = entry->buffer;
      if (source == nullptr || source->buffer == nullptr || !source->node.enabled) {
        continue;
      }
      wlr_scene_buffer* copy = wlr_scene_buffer_create(surfaces, source->buffer);
      if (copy == nullptr) {
        continue;
      }
      wlr_scene_node_set_position(&copy->node, source->node.x, source->node.y);
      if (source->dst_width > 0 && source->dst_height > 0) {
        wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      }
      if (source->src_box.width > 0 && source->src_box.height > 0) {
        wlr_scene_buffer_set_source_box(copy, &source->src_box);
      }
      wlr_scene_buffer_set_transform(copy, source->transform);
      wlr_scene_buffer_set_corner_radii(copy, source->corners);
      wlr_scene_buffer_set_corner_box(copy, &source->corner_box);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_node_set_clip_region(&copy->node, wlr_scene_node_get_clip_region(&source->node));
      wlr_scene_buffer_copy_color(copy, source);
      if (ColorManager* colorManager = m_server->colorManager()) {
        colorManager->applySurfaceDescriptionToBuffer(entry->surface, copy);
      }
      wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
      ++buffersCopied;
    }
    if (buffersCopied == 0) {
      wlr_scene_node_destroy(&surfaces->node);
    } else if (card.view->effects().needsSurface()) {
      wlr_scene_node_copy_animations_for_snapshot(&surfaces->node, &surfaceTree->node);
    }

    if (const auto* frame = dynamic_cast<const SsdFrame*>(preview.frame())) {
      auto* chrome = wlr_scene_tree_create(snapshot);
      wlr_scene_node_set_position(&chrome->node, cardTree->node.x, cardTree->node.y);
      frame->snapshot(chrome);
    }
    std::vector<BorderSnapshot> borders;
    if (border != nullptr && border->node.enabled) {
      snapshotBorder(
          snapshot, *border, cardTree->node.x + border->node.x, cardTree->node.y + border->node.y, &border->node,
          {
              .innerColor = cardBorderColor(card, liveTarget()),
              .outerColor = card.view->borderColors().outer,
              .innerWidth = card.view->decorationBorderWidth(),
              .outerWidth = card.view->decorationOuterBorderWidth(),
              .cornerRadius = card.view->decorationCornerRadius(),
              .padding = preview.borderPadding(),
          },
          card.view->presentedOpacity(), borders
      );
    }

    if (buffersCopied == 0 && borders.empty()) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    wlr_scene_node_copy_animations_for_snapshot(&snapshot->node, &cardTree->node);
    // The frozen card owns its captured geometry and windows_out lifecycle. Retain an interrupted windows_in effect,
    // but do not carry the live card's windows_move effect into the close snapshot.
    wlr_scene_node_set_animation(&snapshot->node, static_cast<unsigned>(AnimationEvent::WindowsMove), nullptr, nullptr);
    (void)m_server->animateCloseSnapshot(card.owner->output, snapshot, snapshot, std::move(borders), {});
    wlr_output_schedule_frame(card.owner->output->wlr());
  }

  void Overview::destroyCard(Card* card) {
    card->preview.reset();
    m_server->scheduleSuspensionSync();
  }

  void Overview::dropCardsWhere(const std::function<bool(const Card&)>& match) {
    // The held card may be one of those dropped: the drag ends here, before the cards go.
    if (m_dragCard != nullptr && match(*m_dragCard)) {
      abandonDrag(/*closing=*/false);
    }
    for (const auto& state : m_outputs) {
      std::erase_if(state->cards, [&](const std::unique_ptr<Card>& card) {
        if (!match(*card)) {
          return false;
        }
        if (m_pressCard == card.get()) {
          m_pressCard = nullptr;
        }
        if (m_middlePressCard == card.get()) {
          m_middlePressCard = nullptr;
        }
        destroyCard(card.get());
        m_shortcutsDirty = true;
        return true;
      });
    }
  }

  void Overview::abandonDrag(bool closing) {
    if (!m_dragTabGroup.tabs.empty()) {
      landTabGroup(*m_dragCard->view, std::exchange(m_dragTabGroup, {}), closing);
    }
    hideDropHint();
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_server->cursor()->overrideCursor(nullptr);
  }

  void Overview::dropCards(const View* view, bool mirrorRowsOnly) {
    dropCardsWhere([view, mirrorRowsOnly](const Card& card) {
      return card.view == view && (!mirrorRowsOnly || card.row->mirror());
    });
  }

  void Overview::rebuildCards(View* view) {
    std::vector<OutputState*> touched;
    addOwners(touched, view);
    dropCards(view);
    if (view->mapped()) {
      createCards(view);
    }
    addOwners(touched, view);
    relayout(std::move(touched));
  }

  size_t Overview::createCards(View* view) {
    size_t created = 0;
    for (Workspace* row : rowsShowing(view)) {
      if (findCard(row, view) == nullptr && createCard(*stateForWorkspace(row), view, *row) != nullptr) {
        ++created;
      }
    }
    return created;
  }

  void Overview::addOwners(std::vector<OutputState*>& states, const View* view) {
    for (const Card* card : cardsOf(view)) {
      states.push_back(card->owner);
    }
  }

  void Overview::relayout(std::vector<OutputState*> states) {
    std::ranges::sort(states);
    states.erase(std::ranges::unique(states).begin(), states.end());
    for (OutputState* state : states) {
      if (state != nullptr) {
        layoutOutput(*state);
        wlr_output_schedule_frame(state->output->wlr());
      }
    }
  }

  Overview::OutputState* Overview::stateFor(const Output* output) {
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    return it == m_outputs.end() ? nullptr : it->get();
  }

  Overview::OutputState* Overview::stateForWorkspace(const Workspace* workspace) {
    if (workspace == nullptr || workspace->group() == nullptr) {
      return nullptr;
    }
    return stateFor(workspace->group()->output());
  }

  std::vector<Overview::Card*> Overview::cardsOf(const View* view) {
    std::vector<Card*> cards;
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          cards.push_back(card.get());
        }
      }
    }
    return cards;
  }

  Overview::Card* Overview::findCard(const Workspace* row, const View* view) {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->row == row && card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  std::vector<Workspace*> Overview::rowsShowing(const View* view) {
    std::vector<Workspace*> rows;
    Workspace* home = view->workspace();
    if (home == nullptr) {
      return rows;
    }
    if (stateForWorkspace(home) != nullptr) {
      rows.push_back(home);
    }
    for (Workspace* copy : home->mirrors()) {
      if (stateForWorkspace(copy) != nullptr) {
        rows.push_back(copy);
      }
    }
    return rows;
  }

  bool Overview::presentsLive(const View* view) const {
    if (!m_active || view->presentationSuppressed()) {
      return false;
    }
    return std::ranges::any_of(m_outputs, [view](const auto& state) {
      return std::ranges::any_of(state->cards, [view](const auto& card) { return card->view == view; });
    });
  }

  void Overview::renderCardShortcut(Card& card) {
    if (card.badge != nullptr) {
      wlr_scene_node_destroy(&card.badge->node);
    }
    card.badge = nullptr;
    card.badgeRect = nullptr;
    card.badgeText = nullptr;
    card.badgeWidth = 0;
    card.badgeHeight = 0;

    if (card.shortcut.empty() || card.preview == nullptr || card.owner == nullptr || card.owner->output == nullptr) {
      return;
    }

    const double scale = std::max(1.0, std::ceil(static_cast<double>(card.owner->output->wlr()->scale)));
    const size_t matched = card.shortcutMatched == SIZE_MAX ? 0 : std::min(card.shortcutMatched, card.shortcut.size());
    const std::string_view shortcut(card.shortcut);
    const auto& colors = config().colors;
    const std::array<float, 4>& badgeColor = colors.overview.badge;
    card.badgeBackground = keycapBackgroundColor(colors.background, badgeColor);
    const std::string markup = std::format(
        "<span foreground='{}' weight='bold'>{}</span><span foreground='{}' weight='bold'>{}</span>",
        rgbaHex(colors.textPrimary), escapeMarkup(shortcut.substr(0, matched)), rgbaHex(badgeColor),
        escapeMarkup(shortcut.substr(matched))
    );
    TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = "monospace 19",
        .maxWidth = 350,
        .padding = 0,
        .scale = scale,
        .bgA = 0.0,
    });
    if (rendered.buffer == nullptr) {
      return;
    }

    card.badge = wlr_scene_tree_create(card.preview->tree());
    if (card.badge == nullptr) {
      wlr_buffer_drop(rendered.buffer);
      return;
    }
    // Keycap proportions: the label's line box sets the height, and the badge is
    // never narrower than it is tall, so a single character reads as a square.
    constexpr int kBadgeSidePad = 8;
    const int badgeHeight = rendered.logicalHeight;
    const int badgeWidth = std::max(rendered.logicalWidth + 2 * kBadgeSidePad, badgeHeight);
    const std::array<float, 4> background = tint(card.badgeBackground, 1.0);
    card.badgeRect = wlr_scene_rect_create(card.badge, badgeWidth, badgeHeight, background.data());
    card.badgeText = wlr_scene_buffer_create(card.badge, rendered.buffer);
    wlr_buffer_drop(rendered.buffer);
    if (card.badgeRect == nullptr || card.badgeText == nullptr) {
      wlr_scene_node_destroy(&card.badge->node);
      card.badge = nullptr;
      card.badgeRect = nullptr;
      card.badgeText = nullptr;
      return;
    }

    wlr_scene_node_set_position(
        &card.badgeText->node, (badgeWidth - rendered.logicalWidth) / 2, (badgeHeight - rendered.logicalHeight) / 2
    );
    wlr_scene_buffer_set_dest_size(card.badgeText, rendered.logicalWidth, rendered.logicalHeight);
    card.badgeText->point_accepts_input = rejectInput;
    card.badgeWidth = badgeWidth;
    card.badgeHeight = badgeHeight;
    wlr_scene_node_raise_to_top(&card.badge->node);
    wlr_scene_node_set_enabled(&card.badge->node, false);
  }

  void Overview::assignShortcuts() {
    m_shortcutInput.clear();
    m_shortcutsDirty = true;
    applyProgress();
  }

  void Overview::updateShortcutAssignments() {
    const auto updateCard = [this](Card& card, std::string label) {
      const bool changed = card.shortcut != label;
      card.shortcut = std::move(label);
      card.shortcutMatched = 0;
      if (changed) {
        renderCardShortcut(card);
      }
    };
    const auto clearAll = [&] {
      for (const auto& state : m_outputs) {
        for (const auto& card : state->cards) {
          updateCard(*card, {});
        }
      }
    };

    if (!m_active || m_closing || !config().overview.shortcuts || config().overview.shortcutKeys.size() < 2) {
      clearAll();
      return;
    }

    std::vector<OutputState*> orderedStates;
    orderedStates.reserve(m_outputs.size());
    Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    if (OutputState* state = stateFor(preferred)) {
      orderedStates.push_back(state);
    }
    for (const auto& state : m_outputs) {
      if (orderedStates.empty() || state.get() != orderedStates.front()) {
        orderedStates.push_back(state.get());
      }
    }

    const double z = std::clamp(config().overview.zoom, 0.1, 0.75);
    std::vector<Card*> eligible;
    for (OutputState* state : orderedStates) {
      PreviewMetrics metrics{};
      if (state == nullptr || !previewMetrics(*state, *m_server, z, metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }

      std::vector<size_t> visible;
      visible.reserve(group->workspaceCount());
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        const wlr_box preview = previewBox(metrics, state->rowScroll.target(), index);
        wlr_box shown{};
        if (wlr_box_intersection(&shown, &preview, &metrics.outputBox)) {
          visible.push_back(index);
        }
      }
      std::ranges::stable_sort(visible, [state](size_t left, size_t right) {
        const double leftDistance = std::abs(static_cast<double>(left) - state->rowScroll.target());
        const double rightDistance = std::abs(static_cast<double>(right) - state->rowScroll.target());
        return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
      });

      struct PositionedCard {
        Card* card = nullptr;
        int x = 0;
        int y = 0;
      };
      // Traverse along the scrolling axis first: it is the axis cards are laid out on.
      const bool horizontalWorkspaces = metrics.axis == WorkspaceAxis::Horizontal;
      for (const size_t index : visible) {
        const wlr_box preview = previewBox(metrics, state->rowScroll.target(), index);
        std::vector<PositionedCard> previewCards;
        for (const auto& card : state->cards) {
          if (card->workspaceIndex != index
              || card->view == nullptr
              || !card->view->mapped()
              || card->view->tabHidden()) {
            continue;
          }
          int worldX = card->view->layoutTargetX();
          int worldY = card->view->layoutTargetY();
          if (card->row->mirror()) {
            const std::optional<MirrorMapping> mapping = cardMapping(*card);
            if (!mapping) {
              continue;
            }
            worldX = mapping->toReceiverX(worldX);
            worldY = mapping->toReceiverY(worldY);
          }
          const int x = preview.x + static_cast<int>(std::lround((worldX - metrics.outputBox.x) * z));
          const int y = preview.y + static_cast<int>(std::lround((worldY - metrics.outputBox.y) * z));
          previewCards.push_back({.card = card.get(), .x = x, .y = y});
        }
        std::ranges::stable_sort(
            previewCards, [horizontalWorkspaces](const PositionedCard& left, const PositionedCard& right) {
              if (horizontalWorkspaces) {
                return left.y == right.y ? left.x < right.x : left.y < right.y;
              }
              return left.x == right.x ? left.y < right.y : left.x < right.x;
            }
        );
        for (const PositionedCard& positioned : previewCards) {
          eligible.push_back(positioned.card);
        }
      }
    }

    const auto assignmentFor = [this](const View* view) -> ShortcutAssignment* {
      const auto assignment = std::ranges::find_if(m_shortcutAssignments, [view](const ShortcutAssignment& item) {
        return item.view == view;
      });
      return assignment == m_shortcutAssignments.end() ? nullptr : &*assignment;
    };
    for (Card* card : eligible) {
      if (assignmentFor(card->view) == nullptr) {
        m_shortcutAssignments.push_back({.view = card->view, .label = {}});
      }
    }

    m_shortcutLabelCapacity = std::max(m_shortcutLabelCapacity, m_shortcutAssignments.size());
    const std::vector<std::string> labels = shortcutLabels(m_shortcutLabelCapacity, config().overview.shortcutKeys);
    std::vector<bool> used(labels.size(), false);
    const auto labelIndex = [&labels](std::string_view label) {
      for (size_t index = 0; index < labels.size(); ++index) {
        if (labels[index] == label) {
          return index;
        }
      }
      return labels.size();
    };

    for (ShortcutAssignment& assignment : m_shortcutAssignments) {
      const size_t index = labelIndex(assignment.label);
      if (index == labels.size() || used[index]) {
        assignment.label.clear();
      } else {
        used[index] = true;
      }
    }
    size_t nextUnused = 0;
    for (ShortcutAssignment& assignment : m_shortcutAssignments) {
      if (!assignment.label.empty()) {
        continue;
      }
      while (nextUnused < used.size() && used[nextUnused]) {
        ++nextUnused;
      }
      if (nextUnused == labels.size()) {
        break;
      }
      assignment.label = labels[nextUnused];
      used[nextUnused] = true;
    }

    for (Card* card : eligible) {
      ShortcutAssignment* assignment = assignmentFor(card->view);
      updateCard(*card, assignment != nullptr ? assignment->label : std::string{});
    }
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (std::ranges::find(eligible, card.get()) == eligible.end()) {
          updateCard(*card, {});
        }
      }
    }
  }

  void Overview::populateRow(OutputState& state, Workspace& row) {
    // A mirror owns no windows: its row shows its source's.
    Workspace* owner = row.layoutOwner();
    if (owner == nullptr) {
      return;
    }
    // Tiled, then floating, then fullscreen: mirrors the per-workspace scene
    // layer split so overlapping cards stack the way the real windows do.
    for (int pass = 0; pass < 3; ++pass) {
      for (View* view : owner->allViews()) {
        if (view == nullptr || !view->mapped() || view->pinned()) {
          continue;
        }
        const bool fullscreen = view->currentFullscreen();
        const int layer = fullscreen ? 2 : (view->tiled() ? 0 : 1);
        if (layer == pass) {
          createCard(state, view, row);
        }
      }
    }
  }

  void Overview::populateCards(OutputState& state) {
    WorkspaceGroup* group = state.output->workspaceGroup();
    if (group == nullptr) {
      return;
    }
    for (size_t row = 0; row < group->workspaceCount(); ++row) {
      if (Workspace* workspace = group->workspaceAt(row)) {
        populateRow(state, *workspace);
      }
    }
  }

  void Overview::buildState() {
    UMBRIEL_ZONE("Overview::buildState");
    m_tree = m_server->overviewTree();
    for (const auto& output : m_server->outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr || group->workspaceCount() == 0 || !output->wlr()->enabled) {
        continue;
      }
      auto state = std::make_unique<OutputState>();
      state->output = output.get();
      state->tree = wlr_scene_tree_create(m_tree);
      if (state->tree == nullptr) {
        continue;
      }
      if (config().overview.backgroundBlur && config().appearance.blur.enabled) {
        state->backgroundBlur = wlr_scene_blur_create(m_server->overviewBlurTree(), 1, 1);
        if (state->backgroundBlur != nullptr) {
          wlr_scene_blur_set_should_only_blur_bottom_layer(state->backgroundBlur, config().appearance.blur.optimized);
        }
      }
      const std::array<float, 4> backgroundTint = tint(config().colors.overview.backgroundTint, 0.0);
      state->backgroundTint = wlr_scene_rect_create(state->tree, 1, 1, backgroundTint.data());
      wlr_scene_rect_set_corner_radius(state->backgroundTint, 0);
      state->workspaceBackgrounds.reserve(group->workspaceCount());
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->workspaceBackgrounds.push_back(createWorkspaceBackground(*state));
      }
      state->tileShadows = wlr_scene_tree_create(state->tree);
      state->activeWorkspaceIndex = group->active() != nullptr ? group->active()->index() : 0;
      state->rowScroll.snap(static_cast<double>(state->activeWorkspaceIndex));
      OutputState* raw = state.get();
      m_outputs.push_back(std::move(state));
      populateCards(*raw);
    }
  }

  // -: open/close

  bool Overview::beginPresentation() {
    UMBRIEL_ZONE("Overview::beginPresentation");
    if (m_active) {
      return true;
    }
    if (m_server->sessionLocked()) {
      return false;
    }
    // A data-device drag owns wlroots' pointer and keyboard grabs until the initiating button is released. Taking
    // overview input ownership now would hide that release from the drag and leave both grabs active.
    if (m_server->seat()->wlr()->drag != nullptr) {
      kLog.debug("overview open ignored during active client drag");
      return false;
    }
    m_server->cursor()->resetMode();
    // An in-flight three-finger switch settles on the filmstrip instead of the hidden desktop slide.
    const Gestures::SwitchPick switchPick = m_server->gestures()->pickSwitchForOverview();
    for (const auto& output : m_server->outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      group->slideFinish();
      // Settle the visual scroll onto the layout scroll so cards at progress 0
      // sit exactly where the real windows are.
      if (Workspace* workspace = group->active()) {
        workspace->arrange(false);
      }
    }
    // Before buildState: reconcileDynamic must not invalidate cards mid-build.
    if (switchPick.group != nullptr) {
      switchPick.group->activate(switchPick.target, false);
    }

    buildState();
    if (m_outputs.empty()) {
      return false;
    }
    m_server->cursor()->resetWheelAccumulation();

    if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
      scratchpad->hideAll();
    }

    m_active = true;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pendingFocus = nullptr;
    m_pointerOutput = m_server->outputFromWlr(m_server->preferredOutput());
    m_server->notifyOverviewChanged();

    // Initialize every View's canonical presentation box before cards consume it. Hidden workspaces normally skip
    // scene presentation entirely, but while overview is active their hidden nodes carry the same position and size
    // state that would be shown if the workspace were active.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        for (size_t index = 0; index < group->workspaceCount(); ++index) {
          if (Workspace* workspace = group->workspaceAt(index)) {
            workspace->arrange(false);
          }
        }
      }
    }
    if (switchPick.group != nullptr) {
      if (OutputState* state = stateFor(switchPick.group->output())) {
        // Read the row after activate: reconcileDynamic may have dropped the workspace the swipe left.
        const auto row = static_cast<double>(switchPick.target->index());
        state->rowScroll.snap(row + switchPick.offset);
        animateRow(*state, row, switchPick.velocity);
      }
    }
    assignShortcuts();

    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, false);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, false);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    // Bottom-layer surfaces are mirrored into every row, so the real ones step aside the way windows do. The
    // background layer stays: it is the blur source and what shows around the filmstrip.
    if (config().overview.workspaceWallpaper) {
      wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, false);
    }

    m_server->clearKeyboardFocus();
    activateLiveTarget();
    m_server->cursor()->clearPointerFocus();
    m_server->cursor()->clearConstraint();

    applyProgress();
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, true);
    for (const auto& state : m_outputs) {
      state->output->markBlurBackgroundDirty();
    }
    kLog.debug("overview opened on {} output(s)", m_outputs.size());
    return true;
  }

  void Overview::open() {
    UMBRIEL_ZONE("Overview::open");
    if (m_active) {
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
        scratchpad->hideAll();
      }
      m_closing = false;
      m_server->notifyOverviewChanged();
      m_pendingFocus = nullptr;
      if (m_progress < 1.0 || m_targetProgress < 1.0) {
        startAnimation(1.0, false);
      }
      return;
    }
    if (!beginPresentation()) {
      return;
    }
    startAnimation(1.0, false);
  }

  void Overview::toggle() {
    if (m_active && !m_closing) {
      close();
    } else {
      open();
    }
  }

  void Overview::close() { beginClose(nullptr); }

  void Overview::closeToWorkspace(Workspace* workspace, View* focus, bool warpCursor) {
    if (!m_active || m_closing) {
      return;
    }
    // A card of a mirror row focuses its window through that mirror's output, whichever output holds the pointer.
    m_closeOutput = workspace != nullptr && workspace->mirror() && workspace->group() != nullptr
        ? workspace->group()->output()
        : nullptr;
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->active() != workspace) {
      // No slide: the real trees are hidden, the filmstrip is the transition.
      workspace->group()->activate(workspace, false);
    }
    if (focus != nullptr && focus->mapped()) {
      // Overview focus keeps keyboard input withheld, but it updates the workspace and starts any scrolling-column
      // reveal. Begin it before the closing zoom so both animations receive their first tick together. finishAnimation
      // repeats the focus after teardown to deliver keyboard focus once the real trees own input again.
      m_server->focusViewOn(focus, FocusReason::PointerPress, m_closeOutput);
      if (focus->workspace() != nullptr && focus->workspace()->layoutConfig().scrolling.centerOnOverviewSelect) {
        focus->workspace()->centerFocusedColumn();
      }
    }
    beginClose(focus, 0, warpCursor);
  }

  void Overview::beginClose(View* focus, double releaseVelocity, bool warpCursor) {
    if (!m_active || m_closing) {
      return;
    }
    m_server->cursor()->resetWheelAccumulation();
    // A close releases any navigation mid-gesture. Input events carry monotonic milliseconds, so the release sample
    // shares their clock and bleeds the speed of fingers that came to rest before the close.
    endNavigation(false, monotonicMsec(), m_navigationSource);
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    hideDropHint();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_pressTab = nullptr;
    clearMiddlePress();
    m_pendingFocus = focus;
    m_pendingCursorWarp = warpCursor && config().input.cursor.followsFocus && focus != nullptr;
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && group->active() != nullptr) {
        animateRow(*state, static_cast<double>(group->active()->index()));
      }
    }
    startAnimation(0.0, true, releaseVelocity);
  }

  void Overview::forceClose() {
    if (!m_active) {
      return;
    }
    m_zoomAnim.snap(0.0);
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    restoreFocus(nullptr);
  }

  void Overview::startAnimation(double target, bool closing, double releaseVelocity) {
    m_closing = closing;
    m_server->notifyOverviewChanged();
    m_targetProgress = target;
    m_progressFrom = m_progress;
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      m_zoomAnim.snap(1.0);
      m_progress = target;
      for (const auto& state : m_outputs) {
        state->rowScroll.snap(state->rowScroll.target());
      }
      finishAnimation();
      return;
    }
    m_zoomAnim.snap(0.0);
    if (overview.curve.easing == Easing::Spring) {
      // Physics mode, so the tail can end once it no longer moves a pixel. The zoom runs the span from where the
      // gesture left off to the target, so a release speed given in progress per second is that speed over this span.
      const double span = m_targetProgress - m_progressFrom;
      m_zoomAnim.settleSpring(1.0, overview.curve.spring, span != 0.0 ? releaseVelocity / span : 0.0);
    } else {
      m_zoomAnim.retarget(1.0, overview.durationMs, overview.curve);
    }
    // Animations only tick from an output frame; kick one so an idle desktop starts the zoom.
    scheduleFrames();
  }

  void Overview::animateRow(OutputState& state, double target, double releaseVelocity) {
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      state.rowScroll.snap(target);
      applyProgress();
      return;
    }
    if (overview.workspaceCurve.easing == Easing::Spring) {
      state.rowScroll.settleSpring(target, overview.workspaceCurve.spring, releaseVelocity);
    } else {
      state.rowScroll.retarget(target, overview.durationMs, overview.workspaceCurve);
    }
    // Animations only tick from an output frame; kick one so an idle desktop
    // starts both timelines from the same frame.
    scheduleFrames();
  }

  bool Overview::tickAnimations(double nowMsec) {
    bool active = m_dropHint != nullptr && m_dropHint->tickAnimations(nowMsec);
    const bool zoomTicked = m_zoomAnim.tick(nowMsec);
    if (zoomTicked && m_zoomAnim.animating() && m_zoomAnim.curve().easing == Easing::Spring) {
      static_cast<void>(m_zoomAnim.finishSpringTail(zoomPixelsPerUnit()));
    }
    if (zoomTicked) {
      const double value = m_zoomAnim.current();
      m_progress = m_progressFrom + (m_targetProgress - m_progressFrom) * value;
    }
    bool rowTicked = false;
    for (const auto& state : m_outputs) {
      const bool ticked = state->rowScroll.tick(nowMsec);
      if (ticked && state->rowScroll.animating() && state->rowScroll.curve().easing == Easing::Spring) {
        PreviewMetrics metrics;
        if (previewMetrics(*state, *m_server, zoom(), metrics)) {
          const double step =
              (metrics.axis == WorkspaceAxis::Horizontal ? metrics.previewW : metrics.previewH) + metrics.gap;
          static_cast<void>(state->rowScroll.finishSpringTail(step));
        }
      }
      rowTicked = ticked || rowTicked;
      active = active || state->rowScroll.animating();
    }
    if (zoomTicked || rowTicked || m_cardPresentationDirty) {
      applyProgress();
    }
    m_cardPresentationDirty = false;
    // Cards mirror their views' animation slots, whose timelines keep running after card layout settles. Persistent
    // effect clocks advance only on the card output's effect frames.
    if (m_active) {
      syncCardEffects();
    }
    for (const auto& state : m_outputs) {
      bindAnimationEffect(
          &state->tree->node, AnimationEvent::Overview, m_zoomAnim.animating() ? m_zoomAnim : state->rowScroll,
          m_closing ? -1.0F : 1.0F
      );
    }
    if (zoomTicked && !m_zoomAnim.animating()) {
      finishAnimation();
    }
    return m_zoomAnim.animating() || active;
  }

  bool Overview::hasActiveAnimations() const {
    return m_zoomAnim.animating()
        || std::ranges::any_of(m_outputs, [](const auto& state) { return state->rowScroll.animating(); })
        || (m_dropHint != nullptr && m_dropHint->hasActiveAnimations());
  }

  void Overview::finishAnimation() {
    if (!m_closing) {
      applyProgress();
      return;
    }
    View* focus = m_pendingFocus;
    const bool warpCursor = m_pendingCursorWarp;
    m_pendingFocus = nullptr;
    m_pendingCursorWarp = false;
    // The real trees stay hidden until teardown, so a workspace switch during the close runs as the row scroll. The
    // close still ends with the zoom, so binds keep their timing; a row still travelling hands its offset and velocity
    // to the real slide. beginClose retargets each row to the active workspace, so a row resting there is not carried.
    std::vector<std::tuple<WorkspaceGroup*, double, double>> carried;
    for (const auto& state : m_outputs) {
      const AnimatedValue& row = state->rowScroll;
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && row.animating() && row.from() != row.target()) {
        carried.emplace_back(group, std::clamp(row.current() - row.target(), -1.0, 1.0), row.velocity());
      }
    }
    teardown();
    // Real trees are visible again: settle each active workspace so window
    // positions match where the cards landed.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        if (Workspace* workspace = group->active()) {
          workspace->markArrange(false);
        }
      }
    }
    for (const auto& [group, offset, velocity] : carried) {
      if (group->slideBegin(offset < 0, offset > 0)) {
        group->slideApply(offset);
        group->slideSettle(0, velocity);
      }
    }
    restoreFocus(focus);
    if (warpCursor
        && focus != nullptr
        && focus->mapped()
        && seatFocusedWindow(*m_server) == focus->placementRoot()->inputDelegate()) {
      m_server->cursor()->warpToView(*focus);
    }
  }

  void Overview::restoreFocus(View* focus) {
    if (focus != nullptr && focus->mapped()) {
      m_server->focusViewOn(focus, FocusReason::PointerPress, m_closeOutput);
    } else {
      m_server->refocus();
    }
    m_closeOutput = nullptr;
    // Focus chrome changed while the windows were hidden (the live target moved during the overview, focus returned
    // during the close or just now), so they reappear with the settled result.
    for (const auto& view : m_server->registry().all()) {
      view->settleFocusChrome();
    }
  }

  void Overview::teardown() {
    m_server->cursor()->resetWheelAccumulation();
    cancelNavigation();
    clearMiddlePress();
    hideDropHint();
    for (const auto& state : m_outputs) {
      clearDesktop(*state);
      for (const auto& card : state->cards) {
        destroyCard(card.get());
      }
      state->cards.clear();
      if (state->backgroundBlur != nullptr) {
        wlr_scene_node_destroy(&state->backgroundBlur->node);
        state->backgroundBlur = nullptr;
      }
      if (state->tree != nullptr) {
        wlr_scene_node_destroy(&state->tree->node);
        state->tree = nullptr;
      }
      state->output->markBlurBackgroundDirty();
      wlr_output_schedule_frame(state->output->wlr());
    }
    m_outputs.clear();
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, false);

    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, true);
    if (m_active) {
      applyPinnedOpacity(1.0F);
    }
    wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, true);

    m_active = false;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pointerOutput = nullptr;
    m_server->notifyOverviewChanged();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_pressTab = nullptr;
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_cardPresentationDirty = false;
    m_gestureOpenedHere = false;
    m_shortcutInput.clear();
    m_shortcutAssignments.clear();
    m_shortcutLabelCapacity = 0;
    m_server->reconcileDynamicWorkspaces();
    m_server->scheduleSuspensionSync();
  }

  // -: gesture

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
    }
    m_zoomAnim.snap(0.0);
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    m_closing = false;
    m_server->notifyOverviewChanged();
    m_progress = progress;
    m_targetProgress = progress;
    applyProgress();
  }

  void Overview::gestureEnd(bool commitOpen, double releaseVelocity) {
    if (!m_active) {
      return;
    }
    m_gestureOpenedHere = false;
    if (commitOpen) {
      startAnimation(1.0, false, releaseVelocity);
      return;
    }
    beginClose(nullptr, releaseVelocity);
  }

  // -: hooks

  void Overview::onViewMapped(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    if (view->attached()) {
      onViewAttachmentsChanged(view->placementRoot());
      return;
    }
    if (createCards(view) == 0) {
      return;
    }
    assignShortcuts();
  }

  void Overview::onViewAttachmentsChanged(View* parent) {
    if (!m_active || parent == nullptr) {
      return;
    }
    View* root = parent->placementRoot();
    for (Card* card : cardsOf(root)) {
      // The card's own surfaces stay; the dialogs' are mirrored again from the current attachments.
      card->preview->dropAttachedSurfaces();
      card->preview->syncSurfaces();
    }
    std::vector<OutputState*> touched;
    addOwners(touched, root);
    relayout(std::move(touched));
  }

  void Overview::onViewPinnedChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    // A pinned window shows through the overview only at the desktop chrome's alpha; a card mirrors full opacity.
    view->setOverviewOpacity(view->pinned() ? desktopChromeAlpha() : 1.0F);
    const std::vector<Card*> cards = cardsOf(view);
    std::vector<OutputState*> touched{stateForWorkspace(view->workspace())};
    addOwners(touched, view);
    if (view->pinned()) {
      if (cards.empty()) {
        return;
      }
      if (m_drop.view == view) {
        m_drop.view = nullptr;
        m_drop.edge = 0;
        hideDropHint();
      }
      for (Card* card : cards) {
        if (m_pressCard == card) {
          m_pressCard = nullptr;
        }
        if (m_middlePressCard == card) {
          clearMiddlePress();
        }
        if (m_dragCard == card) {
          abandonDrag(/*closing=*/false);
        }
      }
      std::erase_if(m_shortcutAssignments, [view](const ShortcutAssignment& assignment) {
        return assignment.view == view;
      });
      dropCards(view);
    } else {
      createCards(view);
      addOwners(touched, view);
    }
    relayout(std::move(touched));
    assignShortcuts();
  }

  void Overview::onViewFloatingChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    // Cards stack tiled, then floating (populateRow): rebuild so each one
    // moves to its new layer instead of keeping its old spot.
    rebuildCards(view);
    assignShortcuts();
  }

  void Overview::onViewUnmapped(View* view, bool animateClose) {
    if (!m_active || view == nullptr) {
      return;
    }
    std::erase_if(m_shortcutAssignments, [view](const ShortcutAssignment& assignment) {
      return assignment.view == view;
    });
    if (m_pendingFocus == view) {
      m_pendingFocus = nullptr;
    }
    if (m_pressCard != nullptr && m_pressCard->view == view) {
      m_pressCard = nullptr;
    }
    if (m_pressTab == view) {
      m_pressTab = nullptr;
    }
    if (m_middlePressCard != nullptr && m_middlePressCard->view == view) {
      m_middlePressCard = nullptr;
    }
    if (m_drop.view == view) {
      m_drop.view = nullptr;
      m_drop.edge = 0;
      hideDropHint();
    }
    // A hidden tab leaves the lifted group; the dragged window closing sends the rest back where the group was.
    if (m_dragCard != nullptr && m_dragCard->view != view) {
      std::erase(m_dragTabGroup.tabs, view);
    }
    if (m_dragCard != nullptr && m_dragCard->view == view) {
      abandonDrag(/*closing=*/true);
    }
    Workspace* workspace = view->workspace();
    std::vector<OutputState*> touched{stateForWorkspace(workspace)};
    addOwners(touched, view);
    if (animateClose) {
      // Every row showing the window animates it out, each with its own geometry.
      for (Card* card : cardsOf(view)) {
        snapshotCardForClose(*card);
      }
    }
    dropCards(view);
    // The overview keeps the focus chrome while it owns the seat, so a closed focused window hands focus to the nearest
    // survivor now rather than at zoom-out. Ask before layout detachment so the closing view still identifies its row
    // and column; the replacement is its predecessor, or the next neighbor at the leading edge.
    if (workspace != nullptr && workspace->focusedView() == view) {
      View* replacement = workspace->focusReplacementForRemoval(view);
      if (replacement != nullptr) {
        // The layout judges the pair once this column is gone, so the reveal this unblocks may only fit the survivor.
        workspace->noteRemovalOfFocusedColumn(workspace->layout().columnOf(view));
        if (workspace->active()) {
          m_server->focusView(replacement, FocusReason::Startup);
        } else {
          workspace->setFocusedView(replacement);
        }
      } else {
        workspace->setFocusedView(nullptr);
      }
    }
    relayout(std::move(touched));
    assignShortcuts();
  }

  void Overview::onViewWorkspaceChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    if (view->pinned()) {
      onViewPinnedChanged(view);
      return;
    }

    Workspace* workspace = view->workspace();
    OutputState* target = stateForWorkspace(workspace);
    std::vector<OutputState*> touched{target};
    addOwners(touched, view);
    // The mirrors of the old workspace are not those of the new one: their cards go, and the new mirrors' are made
    // below. The card on the view's own row is kept and moved.
    dropCards(view, /*mirrorRowsOnly=*/true);
    const std::vector<Card*> own = cardsOf(view);
    if (!own.empty() && target == nullptr) {
      dropCards(view);
    } else if (!own.empty()) {
      Card* card = own.front();
      card->row = workspace;
      card->workspaceIndex = workspace->index();
      if (OutputState* source = card->owner; source != target) {
        const auto it = std::ranges::find_if(source->cards, [card](const std::unique_ptr<Card>& candidate) {
          return candidate.get() == card;
        });
        std::unique_ptr<Card> moved = std::move(*it);
        source->cards.erase(it);
        // A hidden card may skip shadow layout; release its source-output shadow before changing owners.
        moved->preview->releaseShadow();
        moved->owner = target;
        wlr_scene_node_reparent(&moved->preview->tree()->node, target->tree);
        target->cards.push_back(std::move(moved));
      }
    }
    createCards(view);
    addOwners(touched, view);
    relayout(std::move(touched));
    assignShortcuts();
  }

  void Overview::onWorkspaceMirrorChanged(Workspace* mirror) {
    if (!m_active || mirror == nullptr) {
      return;
    }
    if (m_dragCard != nullptr && m_dragCard->row == mirror) {
      endDrag(false);
    }
    dropCardsWhere([mirror](const Card& card) { return card.row == mirror; });
    OutputState* state = stateForWorkspace(mirror);
    if (state == nullptr) {
      return;
    }
    populateRow(*state, *mirror);
    relayout({state});
    // An ordinary card held over this row now hovers the windows of another source.
    refreshDragTarget();
    assignShortcuts();
  }

  void Overview::onWorkspaceActivated(WorkspaceGroup* group) {
    if (!m_active || group == nullptr || group->active() == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    OutputState* target = stateFor(group->output());
    if (target == nullptr) {
      return;
    }
    const auto row = static_cast<double>(group->active()->index());
    target->activeWorkspaceIndex = group->active()->index();
    if (std::abs(target->rowScroll.target() - row) < 0.001 && target->rowScroll.animating()) {
      return;
    }
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    animateRow(*target, row);
    assignShortcuts();
  }

  void Overview::onWorkspaceArranged(Workspace* workspace) {
    if (!m_active) {
      return;
    }
    // The mirrors of the workspace show its windows where it just placed them.
    std::vector<OutputState*> states{stateForWorkspace(workspace)};
    for (const Workspace* copy : workspace->mirrors()) {
      states.push_back(stateForWorkspace(copy));
    }
    relayout(std::move(states));
  }

  void Overview::onWorkspaceInventoryChanged(WorkspaceGroup* group) {
    if (!m_active || group == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    // A row pruned mid-drag must not survive in the drop state: endDrag and the edge-scroll timer both resolve
    // the strip through it. The hint clears with it, like every other drop-state reset (onOutputRemoved).
    if (m_drop.workspace != nullptr && !workspaceRowAlive(m_drop.workspace)) {
      hideDropHint();
      m_drop = {};
    }
    // A row that was pruned left its cards behind, and a mirror row moved to another output left its cards on the old
    // one: they go, and the output that has the row builds its own.
    dropCardsWhere([this](const Card& card) {
      return !workspaceRowAlive(card.row) || stateForWorkspace(card.row) != card.owner;
    });
    OutputState* state = stateFor(group->output());
    if (state == nullptr) {
      return;
    }
    // Renumbering rows on one side of the active workspace must shift the filmstrip scroll by the same amount,
    // otherwise every surviving card visibly jumps even though its workspace identity did not move.
    if (Workspace* active = group->active()) {
      const double delta = static_cast<double>(active->index()) - static_cast<double>(state->activeWorkspaceIndex);
      state->activeWorkspaceIndex = active->index();
      state->rowScroll.translate(delta);
    }
    syncWorkspaceRows(*state, *group);
    // A mirror row that has no cards yet, such as one a new workspace just made, shows its source's windows.
    for (size_t index = 0; index < group->workspaceCount(); ++index) {
      Workspace* row = group->workspaceAt(index);
      if (row != nullptr && row->mirror() && std::ranges::none_of(state->cards, [row](const auto& card) {
            return card->row == row;
          })) {
        populateRow(*state, *row);
      }
    }
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
    assignShortcuts();
  }

  void Overview::onFocusChanged() {
    if (!m_active) {
      return;
    }
    cancelNavigation();
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    applyProgress();
  }

  void Overview::onViewPresentationChanged(View* view) {
    if (!m_active || view == nullptr || stateForWorkspace(view->workspace()) == nullptr) {
      return;
    }
    m_cardPresentationDirty = true;
  }

  void Overview::onDesktopLayerChanged(Output* output) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateFor(output)) {
      // layoutOutput re-resolves the stack before placing the rows.
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onOutputRemoved(Output* output) {
    if (m_navigationOutput == output) {
      // The output may already be tearing down its workspace group.
      m_navigationOutput = nullptr;
      cancelNavigation();
    }
    if (!m_active) {
      return;
    }
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    if (it == m_outputs.end()) {
      return;
    }
    if (m_drop.workspace != nullptr && stateForWorkspace(m_drop.workspace) == it->get()) {
      hideDropHint();
      m_drop = {};
    }
    if (m_dropWorkspaceGroup != nullptr && m_dropWorkspaceGroup->output() == output) {
      hideDropHint();
      m_dropWorkspaceGroup = nullptr;
    }
    if (m_dragCard != nullptr && m_dragCard->owner == it->get()) {
      abandonDrag(/*closing=*/false);
    }
    if (m_pressCard != nullptr && m_pressCard->owner == it->get()) {
      m_pressCard = nullptr;
    }
    if (m_middleOutput == output) {
      clearMiddlePress();
    }
    clearDesktop(**it);
    for (const auto& card : (*it)->cards) {
      destroyCard(card.get());
    }
    (*it)->cards.clear();
    if ((*it)->tree != nullptr) {
      wlr_scene_node_destroy(&(*it)->tree->node);
    }
    m_outputs.erase(it);
    if (m_outputs.empty()) {
      forceClose();
    } else {
      assignShortcuts();
    }
  }

  // -: hit testing

  Overview::Card* Overview::cardAt(double lx, double ly) {
    // Topmost first: later outputs and later cards paint over earlier ones.
    // Hit testing uses the same output clip as rendering. The dragged card is
    // reparented to the unclipped root and hits everywhere.
    for (const auto& state : std::views::reverse(m_outputs)) {
      PreviewMetrics metrics{};
      if (!previewMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      for (const auto& card : std::views::reverse(state->cards)) {
        if (card->preview == nullptr || !card->preview->tree()->node.enabled) {
          continue;
        }
        wlr_box hit = card->box;
        if (card.get() != m_dragCard && !wlr_box_intersection(&hit, &card->box, &metrics.outputBox)) {
          continue;
        }
        if (boxContains(hit, lx, ly)) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  View* Overview::tabAt(double lx, double ly) {
    OutputState* state = nullptr;
    size_t workspacePosition = 0;
    Workspace* workspace = workspaceAtPoint(lx, ly, &state, &workspacePosition, false, false);
    PreviewMetrics metrics{};
    if (workspace == nullptr || !previewMetrics(*state, *m_server, zoom(), metrics)) {
      return nullptr;
    }
    const wlr_box preview = previewBox(metrics, state->rowScroll.current(), workspacePosition);
    const std::optional<TabHit> hit = workspace->tabs().tabAt(
        metrics.outputBox.x + (lx - preview.x) / metrics.zoom, metrics.outputBox.y + (ly - preview.y) / metrics.zoom
    );
    return hit ? hit->tab : nullptr;
  }

  Workspace* Overview::workspaceAtPoint(
      double lx, double ly, OutputState** outState, size_t* outIndex, bool extendScrollingAxis, bool extendFilmstripEnds
  ) {
    for (const auto& state : m_outputs) {
      PreviewMetrics metrics{};
      if (!previewMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      // A preview can only own points on its own output, so a preview overhanging
      // a neighbour never steals its targets.
      if (!boxContains(metrics.outputBox, lx, ly)) {
        continue;
      }
      const bool horizontalWorkspaces = metrics.axis == WorkspaceAxis::Horizontal;
      for (size_t index = 0; index < state->workspaceBackgrounds.size(); ++index) {
        Workspace* workspace = group->workspaceAt(index);
        if (workspace == nullptr) {
          continue;
        }
        // Cards intentionally form one output-wide strip even when they overhang
        // the centered preview. Drag targeting must cover that same visible area
        // along the scrolling axis or the extreme gaps become dead zones.
        // Background clicks retain the narrower preview hitbox.
        const bool extend = extendScrollingAxis && workspace->scrollingLayout() != nullptr;
        wlr_box box = previewBox(metrics, state->rowScroll.current(), index);
        if (extend) {
          if (horizontalWorkspaces) {
            box.y = metrics.outputBox.y;
            box.height = metrics.outputBox.height;
          } else {
            box.x = metrics.outputBox.x;
            box.width = metrics.outputBox.width;
          }
        }
        if (extendFilmstripEnds) {
          // The same dead-zone fix along the workspace axis, drop targeting only: the rows at either end of the
          // filmstrip claim the space out to the output edge, so a card dragged (or edge-scroll panned) past the
          // last preview still drops on the end workspace. Insertion gaps keep priority: workspaceGapAt runs
          // first and still owns the spaces between previews and the dynamic edge before row 0. Wheel, pointer
          // scroll and navigation lookups pass this false and keep their previous hitboxes.
          const size_t count = state->workspaceBackgrounds.size();
          if (index == 0) {
            if (horizontalWorkspaces) {
              const int end = box.x + box.width;
              box.x = metrics.outputBox.x;
              box.width = end - box.x;
            } else {
              const int end = box.y + box.height;
              box.y = metrics.outputBox.y;
              box.height = end - box.y;
            }
          }
          if (index + 1 == count) {
            if (horizontalWorkspaces) {
              box.width = metrics.outputBox.x + metrics.outputBox.width - box.x;
            } else {
              box.height = metrics.outputBox.y + metrics.outputBox.height - box.y;
            }
          }
        }
        if (!boxContains(box, lx, ly)) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        return workspace;
      }
    }
    return nullptr;
  }

  WorkspaceGroup*
  Overview::workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox) {
    for (const auto& state : m_outputs) {
      PreviewMetrics metrics{};
      if (!previewMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !group->dynamic() || group->workspaceCount() >= kMaxWorkspaces) {
        continue;
      }
      const bool horizontalWorkspaces = metrics.axis == WorkspaceAxis::Horizontal;
      const size_t previewCount = std::min(group->workspaceCount(), state->workspaceBackgrounds.size());
      for (size_t index = 0; index < previewCount; ++index) {
        const wlr_box lower = previewBox(metrics, state->rowScroll.current(), index);
        // Index zero has no preceding preview. Its insertion gap begins at the output edge, so dynamic workspaces can
        // also be inserted before the first preview.
        int before = 0;
        if (index == 0) {
          before = horizontalWorkspaces ? metrics.outputBox.x : metrics.outputBox.y;
        } else {
          const wlr_box upper = previewBox(metrics, state->rowScroll.current(), index - 1);
          before = horizontalWorkspaces ? upper.x + upper.width : upper.y + upper.height;
        }
        const int after = horizontalWorkspaces ? lower.x : lower.y;
        if (after <= before) {
          continue;
        }
        const wlr_box gap = horizontalWorkspaces ? wlr_box{before, lower.y, after - before, lower.height}
                                                 : wlr_box{lower.x, before, lower.width, after - before};
        wlr_box visible{};
        if (!wlr_box_intersection(&visible, &gap, &metrics.outputBox) || !boxContains(visible, lx, ly)) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        if (outHintBox != nullptr) {
          // A bar across the gap: thin along the workspace axis, preview-sized across it.
          if (horizontalWorkspaces) {
            const int width = std::clamp(visible.width / 3, 4, 18);
            *outHintBox = {
                .x = visible.x + (visible.width - width) / 2,
                .y = visible.y,
                .width = width,
                .height = visible.height,
            };
          } else {
            const int height = std::clamp(visible.height / 3, 4, 18);
            *outHintBox = {
                .x = visible.x,
                .y = visible.y + (visible.height - height) / 2,
                .width = visible.width,
                .height = height,
            };
          }
        }
        return group;
      }
    }
    return nullptr;
  }

  Workspace* Overview::preferredWorkspace() const {
    Output* output = m_server->outputFromWlr(m_server->preferredOutput());
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return nullptr;
    }
    return output->workspaceGroup()->active();
  }

  void Overview::clearMiddlePress() {
    if (m_middleScrolling) {
      m_server->gestures()->endPointerScroll(true, 0);
    }
    if (m_middleDragging) {
      m_server->cursor()->overrideCursor(nullptr);
    }
    m_middlePressCard = nullptr;
    m_middleOutput = nullptr;
    m_middlePressed = false;
    m_middleDragging = false;
    m_middlePanning = false;
    m_middleScrolling = false;
    m_middleAccum = 0;
  }

  // -: input

  bool Overview::handleButton(uint32_t button, bool pressed, double lx, double ly, uint32_t timeMsec) {
    if (pressed) {
      cancelNavigation();
    }
    if (!interactive()) {
      return true; // Swallow everything while zooming back in.
    }

    if (!pressed) {
      if (button == BTN_MIDDLE) {
        Card* card = m_middlePressCard;
        const bool closeCard = m_middlePressed && !m_middleDragging;
        if (m_middleScrolling) {
          // A release settles the strip; clearMiddlePress() cancels whatever is still running.
          m_server->gestures()->endPointerScroll(false, timeMsec);
          m_middleScrolling = false;
        }
        clearMiddlePress();
        if (closeCard && card != nullptr && card->view != nullptr && card->view->mapped()) {
          card->view->requestClose();
        }
        return true;
      }
      if (button != BTN_LEFT) {
        return true;
      }
      if (m_dragCard != nullptr) {
        endDrag(true);
        return true;
      }
      Card* card = m_pressCard;
      Workspace* workspace = m_pressWorkspace;
      View* tab = m_pressTab;
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      m_pressTab = nullptr;
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        closeToWorkspace(card->row, card->view);
      } else if (workspace != nullptr) {
        closeToWorkspace(workspace, tab != nullptr ? tab : workspace->focusedView());
      }
      return true;
    }

    Card* card = cardAt(lx, ly);
    if (button == BTN_MIDDLE) {
      m_middlePressCard = card;
      Workspace* workspace = card != nullptr && card->view != nullptr
          ? card->row
          : workspaceAtPoint(lx, ly, nullptr, nullptr, false, false);
      m_middleOutput = workspace != nullptr && workspace->group() != nullptr ? workspace->group()->output() : nullptr;
      m_middlePressX = lx;
      m_middlePressY = ly;
      m_middleAccum = 0;
      m_middlePressed = true;
      m_middleDragging = false;
      m_middlePanning = false;
      m_middleScrolling = false;
      return true;
    }
    if (button != BTN_LEFT) {
      return true;
    }

    m_pressCard = card;
    m_pressWorkspace = nullptr;
    m_pressTab = nullptr;
    m_pressX = lx;
    m_pressY = ly;
    if (card == nullptr) {
      m_pressWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, false, false);
      m_pressTab = tabAt(lx, ly);
    }
    return true;
  }

  void Overview::handleMotion(double lx, double ly, uint32_t timeMsec) {
    if (!interactive()) {
      return;
    }
    // The live target resolves through preferredOutput(), and the overview owns motion while it is up: repaint when
    // the pointer crosses outputs, whether by hand or through the warp a keybind performs to change output.
    if (Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
        output != m_pointerOutput) {
      m_pointerOutput = output;
      activateLiveTarget();
      applyProgress();
    }
    if (m_middlePressed) {
      const WorkspaceGroup* group = m_middleOutput != nullptr ? m_middleOutput->workspaceGroup() : nullptr;
      const bool horizontal = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
      if (!m_middleDragging) {
        const double dx = lx - m_middlePressX;
        const double dy = ly - m_middlePressY;
        if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
          return;
        }
        m_middleDragging = true;
        // Travel across the workspace axis pans the strip; travel along it steps rows.
        m_middlePanning = (std::abs(dx) > std::abs(dy)) != horizontal;
        if (m_middlePanning) {
          m_middleScrolling = m_server->gestures()->beginPointerScroll(m_middlePressX, m_middlePressY);
          if (m_middleScrolling) {
            m_server->gestures()->updatePointerScroll(dx, dy, timeMsec);
          }
        }
        m_middleAccum = 0;
        m_middlePressX = lx;
        m_middlePressY = ly;
        m_server->cursor()->overrideCursor("grabbing");
      }
      if (m_middlePanning) {
        if (m_middleScrolling) {
          m_server->gestures()->updatePointerScroll(lx - m_middlePressX, ly - m_middlePressY, timeMsec);
        }
        m_middlePressX = lx;
        m_middlePressY = ly;
        return;
      }
      m_middleAccum += horizontal ? lx - m_middlePressX : ly - m_middlePressY;
      m_middlePressX = lx;
      m_middlePressY = ly;
      while (m_middleAccum <= -kMiddleScrollStepPx) {
        m_middleAccum += kMiddleScrollStepPx;
        selectRelativeWorkspace(1, m_middleOutput);
      }
      while (m_middleAccum >= kMiddleScrollStepPx) {
        m_middleAccum -= kMiddleScrollStepPx;
        selectRelativeWorkspace(-1, m_middleOutput);
      }
      return;
    }
    if (m_dragCard != nullptr) {
      updateDrag(lx, ly);
      return;
    }
    if (m_pressCard == nullptr) {
      return;
    }
    const double dx = lx - m_pressX;
    const double dy = ly - m_pressY;
    if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
      return;
    }
    beginDrag();
    updateDrag(lx, ly);
  }

  bool Overview::selectRelativeWorkspace(int delta, Output* output) {
    if (!interactive()) {
      return false;
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return false;
    }
    WorkspaceGroup* group = output->workspaceGroup();
    if (group->active() == nullptr) {
      return false;
    }
    const int index = static_cast<int>(group->active()->index()) + delta;
    if (index < 0 || index >= static_cast<int>(group->workspaceCount())) {
      return false;
    }
    // select() lands in onWorkspaceActivated, which animates the filmstrip onto the new
    // workspace, so the caller does not have to arrange anything.
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    if (m_dragCard != nullptr) {
      // Pan-only while a card drag is live: a wheel notch commits a workspace/focus move, which the contract
      // forbids under the held card (and it would leave the drop stale). Ignore the notch entirely.
      return true;
    }
    cancelNavigation();
    if (!interactive()) {
      return true;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    const WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    const bool horizontalWorkspaces = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
    const int sign = direction < 0 ? -1 : 1;
    // Wheel input commits discrete targets on its physical axis, unlike continuous touchpad navigation.
    if (vertical != horizontalWorkspaces) {
      selectRelativeWorkspace(sign, output);
      return true;
    }
    Workspace* workspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true, false);
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return true;
    }
    View* target = vertical ? workspace->focusVertical(sign) : workspace->focusAdjacent(sign);
    if (target == nullptr) {
      return true;
    }
    clearShortcutInput();
    if (workspace->active()) {
      m_server->focusView(target, FocusReason::Gesture);
    } else {
      workspace->setFocusedView(target);
    }
    workspace->activateFocusedColumn();
    scrolling->snapVisible(scrolling->columnOf(target), workspace->scrollViewportExtent());
    workspace->markArrange(true);
    return true;
  }

  Workspace* Overview::pointerScrollWorkspace(double lx, double ly) {
    if (!interactive()) {
      return nullptr;
    }
    return workspaceAtPoint(lx, ly, nullptr, nullptr, true, false);
  }

  Workspace* Overview::navigationWorkspace() const {
    const WorkspaceGroup* group = m_navigationOutput != nullptr ? m_navigationOutput->workspaceGroup() : nullptr;
    if (group != nullptr) {
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (group->workspaceAt(index) == m_navigationWorkspace) {
          return m_navigationWorkspace;
        }
      }
    }
    return nullptr;
  }

  bool Overview::workspaceRowAlive(const Workspace* workspace) const {
    if (workspace == nullptr) {
      return false;
    }
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output != nullptr ? state->output->workspaceGroup() : nullptr;
      if (group == nullptr) {
        continue;
      }
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (group->workspaceAt(index) == workspace) {
          return true;
        }
      }
    }
    return false;
  }

  Workspace* Overview::dragTargetWorkspace() const {
    return m_dragCard != nullptr && workspaceRowAlive(m_drop.workspace) ? m_drop.workspace : nullptr;
  }

  void Overview::onNavigationDeviceDestroyed(wl_listener* listener, void* /*data*/) {
    Overview* self;
    self = wl_container_of(listener, self, m_navigationDeviceDestroy);
    self->cancelNavigation();
  }

  void Overview::cancelNavigation() { endNavigation(true, 0, m_navigationSource); }

  void Overview::beginNavigation(wlr_pointer* pointer, NavigationSource source, double lx, double ly) {
    cancelNavigation();
    // A held card drag may still scroll: the touchpad finger stream must not be swallowed by the hold.
    // Middle-press keeps refusing; handleMotion gives it its own pointer-scroll path.
    const bool dragScroll = m_dragCard != nullptr && source == NavigationSource::Scroll;
    if (!interactive() || (dragging() && !dragScroll) || pointer == nullptr) {
      return;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return;
    }
    m_navigationOutput = output;
    m_navigationHorizontalWorkspaces = output->workspaceGroup()->workspaceAxis() == WorkspaceAxis::Horizontal;
    m_navigationWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true, false);
    if (m_navigationWorkspace == nullptr || m_navigationWorkspace->group() != output->workspaceGroup()) {
      m_navigationWorkspace = output->workspaceGroup()->active();
    }
    m_navigationPointer = pointer;
    m_navigationDeviceDestroy.notify = onNavigationDeviceDestroyed;
    wl_signal_add(&pointer->base.events.destroy, &m_navigationDeviceDestroy);
    m_navigationSource = source;
    m_navigation.reset();
    m_navigationStarted = false;
  }

  void Overview::updateNavigation(double dx, double dy, uint32_t timeMsec) {
    if (m_navigationOutput == nullptr || !interactive()) {
      return;
    }
    m_navigation.update(dx, dy, timeMsec);
    const auto axis = m_navigation.axis();
    if (axis == OverviewNavigation::Axis::Pending) {
      return;
    }
    OutputState* state = stateFor(m_navigationOutput);
    WorkspaceGroup* group = m_navigationOutput->workspaceGroup();
    if (state == nullptr || group == nullptr || group->active() == nullptr) {
      cancelNavigation();
      return;
    }
    const auto& overview = config().overview;
    const double factor =
        axis == OverviewNavigation::Axis::Horizontal ? overview.scrollFactorHorizontal : overview.scrollFactorVertical;
    const auto travel = OverviewNavigation::travelFor(m_navigationSource);
    if ((axis == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      // The rows follow the fingers in both cases; panning is not a workspace step. While a card drag is live
      // the release settles without selecting anything (see endNavigation) and the drop re-resolves under the
      // stationary pointer instead; that is the pan-only contract the held card needs.
      if (!m_navigationStarted) {
        m_navigationStart = state->rowScroll.current();
        m_navigationScale = OverviewNavigation::travelScale(1.0, settledZoom(), factor, travel.workspace);
        m_navigationStarted = true;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      // snap() also stops any settle still running on this output; row animations elsewhere and the zoom continue.
      state->rowScroll.snap(
          GesturePhysics::rubberBand(
              m_navigationStart + m_navigation.position() * m_navigationScale, 0.0, last,
              GesturePhysics::kOverscrollLimit
          )
      );
      applyProgress();
      if (m_dragCard != nullptr) {
        refreshDragTarget();
      }
      return;
    }
    Workspace* workspace = navigationWorkspace();
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const auto viewport = static_cast<double>(workspace->scrollViewportExtent());
    if (!m_navigationStarted) {
      m_navigationStart = scrolling->scroll();
      m_navigationCentered = scrolling->centeredRest();
      m_navigationScale = OverviewNavigation::travelScale(viewport, settledZoom(), factor, travel.viewport);
      m_navigationStarted = true;
    }
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    // Centering an edge column parks the strip past the last column, so the band opens onto the scroll the gesture
    // started from: the fingers bring the strip back in, not the pan's first frame.
    scrolling->setScroll(
        GesturePhysics::rubberBand(
            m_navigationStart + m_navigation.position() * m_navigationScale, std::min(0.0, m_navigationStart),
            std::max(maximum, m_navigationStart), viewport * GesturePhysics::kOverscrollLimit
        )
    );
    workspace->markArrange(false);
    if (m_dragCard != nullptr) {
      refreshDragTarget();
    }
  }

  void Overview::endNavigation(bool cancelled, uint32_t timeMsec, NavigationSource source) {
    if (source != m_navigationSource) {
      return;
    }
    Workspace* workspace = navigationWorkspace();
    Output* output = m_navigationOutput;
    // Clear the gesture before selecting or focusing below: both re-enter through onWorkspaceActivated and
    // onFocusChanged, which cancel the navigation belonging to this output.
    m_navigationOutput = nullptr;
    m_navigationWorkspace = nullptr;
    if (m_navigationPointer != nullptr) {
      wl_list_remove(&m_navigationDeviceDestroy.link);
      m_navigationPointer = nullptr;
    }
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    const bool started = m_navigationStarted;
    m_navigationStarted = false;
    if (!started || output == nullptr || !interactive()) {
      return;
    }
    if (!cancelled) {
      m_navigation.update(0, 0, timeMsec);
    }
    const double projected = m_navigationStart + m_navigation.projectedPosition() * m_navigationScale;
    if ((m_navigation.axis() == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      OutputState* state = stateFor(output);
      WorkspaceGroup* group = output->workspaceGroup();
      if (state == nullptr || group == nullptr || group->active() == nullptr) {
        return;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      if (m_dragCard != nullptr) {
        // Settle-only while a card drag is live: snap the rows (back to where the pan started on a cancel) and
        // select nothing: the active workspace must not change under the held card. The drop then resolves under
        // the pointer at the settled position; applyDrop activates its destination when it lands.
        state->rowScroll.snap(
            cancelled ? m_navigationStart
                      : static_cast<double>(GesturePhysics::stepTarget(projected, 0, static_cast<int>(last)))
        );
        applyProgress();
        refreshDragTarget();
        return;
      }
      const int index = cancelled ? static_cast<int>(group->active()->index())
                                  : GesturePhysics::stepTarget(projected, 0, static_cast<int>(last));
      // Rubber-banded travel moves the filmstrip slower than the fingers, so the release carries the visible speed.
      const double position = m_navigationStart + m_navigation.position() * m_navigationScale;
      const double velocity = cancelled ? 0.0
                                        : m_navigation.velocity()
              * m_navigationScale
              * GesturePhysics::rubberBandDerivative(position, 0.0, last, GesturePhysics::kOverscrollLimit);
      if (index != static_cast<int>(group->active()->index())) {
        group->select(group->workspaceAt(static_cast<size_t>(index)));
        state = stateFor(output);
      }
      if (state != nullptr) {
        animateRow(*state, static_cast<double>(index), velocity);
      }
      return;
    }
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const int viewport = workspace->scrollViewportExtent();
    const auto maximum = static_cast<double>(scrolling->maxScroll(viewport));
    if (cancelled) {
      // A cancel (an unrelated press, a device switch mid-pan) restores the scroll the gesture started from,
      // drag or not, matching non-drag semantics; the refresh keeps the drop honest at the reverted position.
      scrolling->setScroll(
          m_navigationCentered ? m_navigationStart : std::clamp(m_navigationStart, 0.0, maximum), m_navigationCentered
      );
      workspace->markArrange(true);
      if (m_dragCard != nullptr) {
        refreshDragTarget();
      }
      return;
    }
    double bestDistance = std::numeric_limits<double>::max();
    double bestPosition = std::clamp(projected, 0.0, maximum);
    int bestColumn = -1;
    for (size_t index = 0; index < scrolling->columns().size(); ++index) {
      if (scrolling->columns()[index].views.empty()) {
        continue;
      }
      const auto column = static_cast<int>(index);
      for (const double position : scrolling->snapScrolls(column, viewport)) {
        const double distance = std::abs(position - projected);
        if (distance < bestDistance) {
          bestDistance = distance;
          bestPosition = position;
          bestColumn = column;
        }
      }
    }
    // Match normal strip navigation: focus the outermost fully visible column
    // in the travel direction, retaining the selected card within that column.
    const int direction = projected >= scrolling->scroll() ? 1 : -1;
    const auto count = static_cast<int>(scrolling->columns().size());
    for (int index = bestColumn + direction; bestColumn >= 0 && index >= 0 && index < count; index += direction) {
      const double x = scrolling->columnX(index, viewport);
      const double width = scrolling->columnWidth(index, viewport);
      if (x < bestPosition || x + width > bestPosition + viewport) {
        break;
      }
      bestColumn = index;
    }
    if (m_dragCard != nullptr) {
      // Settle the pan only while a card drag is live: snap the scroll to the chosen column but move no focus and
      // commit no selection under the held card, then re-resolve the drop where the pointer sits.
      scrolling->setScroll(bestPosition);
      workspace->markArrange(true);
      refreshDragTarget();
      return;
    }
    View* target = workspace->focusedView();
    const bool focusMoved = bestColumn >= 0 && (target == nullptr || scrolling->columnOf(target) != bestColumn);
    if (focusMoved) {
      // A tab group is entered on the tab it shows, as a directional focus move enters it.
      target = columnEntry(scrolling->columns()[static_cast<size_t>(bestColumn)]);
    }
    scrolling->setScroll(bestPosition);
    // Do not activate a different workspace merely because its preview was
    // panned. Its selected card will receive focus if the user enters that row.
    if (target != nullptr) {
      if (workspace->active()) {
        m_server->focusView(target, FocusReason::Gesture);
      } else {
        workspace->setFocusedView(target);
      }
    }
    // The pan chose the column, so the centering policy says where it rests. A pan back on the focused column has no
    // side to derive, so the last focus move's pair is judged instead.
    if (focusMoved) {
      workspace->activateFocusedColumn();
    } else {
      workspace->reevaluateFocusedColumn();
    }
    workspace->markArrange(true);
  }

  void Overview::handleTouchpadAxis(
      wlr_pointer* pointer, bool vertical, double delta, uint32_t timeMsec, double lx, double ly
  ) {
    // A three-finger swipe owns the overview until it ends; finger scrolling cannot arrive during one anyway.
    if (m_navigationPointer != nullptr && m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (pointer != m_navigationPointer) {
      if (delta == 0) {
        return;
      }
      beginNavigation(pointer, NavigationSource::Scroll, lx, ly);
    }
    if (m_navigationPointer == nullptr) {
      return;
    }
    if (vertical) {
      m_scrollDy += delta;
      m_scrollStopY = delta == 0;
    } else {
      m_scrollDx += delta;
      m_scrollStopX = delta == 0;
    }
    m_scrollTime = timeMsec;
  }

  void Overview::handleTouchpadFrame() {
    if (m_navigationPointer == nullptr || m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (m_scrollDx != 0 || m_scrollDy != 0) {
      updateNavigation(m_scrollDx, m_scrollDy, m_scrollTime);
    }
    const auto axis = m_navigation.axis();
    const bool stop = axis == OverviewNavigation::Axis::Horizontal ? m_scrollStopX
        : axis == OverviewNavigation::Axis::Vertical               ? m_scrollStopY
                                                                   : m_scrollStopX || m_scrollStopY;
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    if (stop) {
      endNavigation(false, m_scrollTime, NavigationSource::Scroll);
    }
  }

  void Overview::refreshShortcutMatches() {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->shortcut.empty()) {
          continue;
        }
        const size_t matched = shortcutStartsWith(card->shortcut, m_shortcutInput) ? m_shortcutInput.size() : SIZE_MAX;
        const size_t oldDisplayed = card->shortcutMatched == SIZE_MAX ? 0 : card->shortcutMatched;
        const size_t newDisplayed = matched == SIZE_MAX ? 0 : matched;
        card->shortcutMatched = matched;
        if (oldDisplayed != newDisplayed) {
          renderCardShortcut(*card);
        }
      }
    }
    applyProgress();
    scheduleFrames();
  }

  void Overview::clearShortcutInput() {
    m_shortcutInput.clear();
    refreshShortcutMatches();
  }

  Overview::Card* Overview::shortcutTarget(const std::string& label) {
    // The label is shared by every card of a view: it picks the one on the pointer's output, else the view's own row.
    const Output* pointer = m_server->outputFromWlr(m_server->preferredOutput());
    Card* best = nullptr;
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->shortcut.size() != label.size()
            || !shortcutStartsWith(card->shortcut, label)
            || card->view == nullptr
            || !card->view->mapped()) {
          continue;
        }
        if (state->output == pointer) {
          return card.get();
        }
        if (best == nullptr || card->row == card->view->workspace()) {
          best = card.get();
        }
      }
    }
    return best;
  }

  bool Overview::handleShortcutKey(uint32_t keysym) {
    if (!config().overview.shortcuts || dragging()) {
      return false;
    }
    if (!m_shortcutInput.empty() && keysym == XKB_KEY_BackSpace) {
      m_shortcutInput.pop_back();
      refreshShortcutMatches();
      return true;
    }
    if (!m_shortcutInput.empty() && keysym == XKB_KEY_Escape) {
      clearShortcutInput();
      return true;
    }

    const char character = shortcutCharacter(keysym);
    if (character == '\0' || std::ranges::none_of(config().overview.shortcutKeys, [character](char configured) {
          return asciiLower(configured) == character;
        })) {
      return false;
    }

    std::string candidate = m_shortcutInput + character;
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (Card* card = shortcutTarget(candidate)) {
        closeToWorkspace(card->row, card->view, true);
        return true;
      }
      for (const auto& state : m_outputs) {
        for (const auto& card : state->cards) {
          if (shortcutStartsWith(card->shortcut, candidate)) {
            m_shortcutInput = candidate;
            refreshShortcutMatches();
            return true;
          }
        }
      }
      if (m_shortcutInput.empty()) {
        break;
      }
      m_shortcutInput.clear();
      candidate.assign(1, character);
    }

    clearShortcutInput();
    return true;
  }

  bool Overview::handleKeybindAction(KeybindAction action) {
    const bool directional = action == KeybindAction::WindowFocusLeft
        || action == KeybindAction::WindowFocusRight
        || action == KeybindAction::WindowFocusOrOutputLeft
        || action == KeybindAction::WindowFocusOrOutputRight
        || action == KeybindAction::WindowFocusUp
        || action == KeybindAction::WindowFocusDown
        || action == KeybindAction::WindowFocusOrWorkspaceUp
        || action == KeybindAction::WindowFocusOrWorkspaceDown
        || action == KeybindAction::WindowFocusOrOutputUp
        || action == KeybindAction::WindowFocusOrOutputDown;
    const bool outputFocus = action == KeybindAction::OutputFocusLeft
        || action == KeybindAction::OutputFocusRight
        || action == KeybindAction::OutputFocusUp
        || action == KeybindAction::OutputFocusDown;

    if (m_closing && (directional || outputFocus)) {
      m_pendingFocus = nullptr;
    }

    if (directional && interactive() && !m_shortcutInput.empty()) {
      clearShortcutInput();
    }
    return false;
  }

  bool Overview::handleFallbackKey(uint32_t keysym) {
    if (!interactive()) {
      return false;
    }
    if (handleShortcutKey(keysym)) {
      return true;
    }
    auto dispatch = [this](KeybindAction action) {
      Keybind bind;
      bind.action = action;
      return m_server->executeAction(bind);
    };
    // Local windows first, exactly like the composite focus actions. Only an arrow
    // pointing along the output's workspace axis falls through to a workspace step.
    const auto arrow = [this, &dispatch](int sign, bool horizontalArrow, KeybindAction action) {
      Workspace* workspace = preferredWorkspace();
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      const bool alongWorkspaceAxis =
          (workspace->group()->workspaceAxis() == WorkspaceAxis::Horizontal) == horizontalArrow;
      const View* neighbor = horizontalArrow ? workspace->focusAdjacent(sign) : workspace->focusVertical(sign);
      if (neighbor != nullptr || !alongWorkspaceAxis) {
        return dispatch(action);
      }
      clearShortcutInput();
      selectRelativeWorkspace(sign, workspace->group()->output());
      return true;
    };
    switch (keysym) {
    case XKB_KEY_Escape:
      close();
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      clearShortcutInput();
      if (Workspace* workspace = preferredWorkspace()) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    case XKB_KEY_Left:
      return arrow(-1, true, KeybindAction::WindowFocusLeft);
    case XKB_KEY_Right:
      return arrow(1, true, KeybindAction::WindowFocusRight);
    case XKB_KEY_Up:
      return arrow(-1, false, KeybindAction::WindowFocusUp);
    case XKB_KEY_Down:
      return arrow(1, false, KeybindAction::WindowFocusDown);
    default:
      return false;
    }
  }

  // -: drag

  void Overview::beginDrag() {
    Card* card = m_pressCard;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_pressTab = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      return;
    }
    View* view = card->view;
    m_dragCard = card;
    m_dragPointerX = m_pressX;
    m_dragPointerY = m_pressY;
    m_dragOffsetX = m_pressX - card->box.x;
    m_dragOffsetY = m_pressY - card->box.y;
    m_dragSourceWorkspace = view->workspace();
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      // Lifted first, so the source position and width below are those of the group.
      m_dragTabGroup = liftTabGroup(*m_dragSourceWorkspace, *view);
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      m_dragSourceWidth = captureDropColumnWidth(*m_dragSourceWorkspace, view);
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->preview->tree()->node, m_tree);
    wlr_scene_node_raise_to_top(&card->preview->tree()->node);
    // The window's other cards, on its own row and the mirrors of it, leave with the lift.
    for (Card* other : cardsOf(view)) {
      PreviewMetrics metrics{};
      if (other->owner != nullptr && previewMetrics(*other->owner, *m_server, zoom(), metrics)) {
        layoutCard(*other, metrics, other->owner->rowScroll.current(), liveTarget());
      }
    }
    m_server->cursor()->overrideCursor("grabbing");
  }

  // The strip can move under a stationary drag pointer (edge auto-scroll, finger-scroll pan): re-running updateDrag
  // at the stored position keeps the drop resolving where the card visibly sits. Valid before arrange() because
  // computeDropTarget reads layout.scroll() directly (drop_target.cpp), so the refresh sees the new scroll at once.
  void Overview::refreshDragTarget() {
    if (m_dragCard != nullptr) {
      updateDrag(m_dragPointerX, m_dragPointerY);
    }
  }

  bool Overview::filmstripEdgeScrolling(Output& output, double speedPxPerSec) {
    if (speedPxPerSec == 0) {
      return false;
    }
    OutputState* state = stateFor(&output);
    WorkspaceGroup* group = output.workspaceGroup();
    if (state == nullptr || group == nullptr || group->workspaceCount() < 2) {
      return false;
    }
    const double current = state->rowScroll.current();
    const auto last = static_cast<double>(group->workspaceCount() - 1);
    return speedPxPerSec < 0 ? current > 0 : current < last;
  }

  bool Overview::advanceFilmstripEdgeScroll(Output& output, double speedPxPerSec, uint32_t elapsedMsec) {
    OutputState* state = stateFor(&output);
    WorkspaceGroup* group = output.workspaceGroup();
    PreviewMetrics metrics{};
    if (state == nullptr
        || group == nullptr
        || elapsedMsec == 0
        || !previewMetrics(*state, *m_server, zoom(), metrics)) {
      return false;
    }
    // Neighbouring previews step one preview plus the gap along the workspace axis. The strip slides at
    // speedPxPerSec world px/s (visible at zoom in the overview); convert so the filmstrip's visible slide matches.
    const int step = (metrics.axis == WorkspaceAxis::Horizontal ? metrics.previewW : metrics.previewH) + metrics.gap;
    if (step <= 0) {
      return false;
    }
    const double rowsPerSec = speedPxPerSec * metrics.zoom / static_cast<double>(step);
    const double current = state->rowScroll.current();
    const auto last = static_cast<double>(group->workspaceCount() - 1);
    const double next = std::clamp(current + rowsPerSec * static_cast<double>(elapsedMsec) / 1000.0, 0.0, last);
    if (next == current) {
      return false;
    }
    state->rowScroll.snap(next);
    applyProgress();
    // The rows moved under a stationary pointer: a live card drag must drop where it visibly sits now.
    refreshDragTarget();
    return true;
  }

  void Overview::updateDrag(double lx, double ly) {
    Card* card = m_dragCard;
    m_dragPointerX = lx;
    m_dragPointerY = ly;
    card->box.x = static_cast<int>(std::lround(lx - m_dragOffsetX));
    card->box.y = static_cast<int>(std::lround(ly - m_dragOffsetY));
    wlr_scene_node_set_position(&card->preview->tree()->node, card->box.x, card->box.y);

    OutputState* state = nullptr;
    size_t workspaceIndex = 0;
    wlr_box workspaceHint{};
    if (WorkspaceGroup* group = workspaceGapAt(lx, ly, &state, &workspaceIndex, &workspaceHint)) {
      m_drop = {};
      m_dropWorkspaceGroup = group;
      m_dropWorkspaceIndex = workspaceIndex;
      showWorkspaceInsertHint(state->output, workspaceHint);
      scheduleFrames();
      return;
    }
    m_dropWorkspaceGroup = nullptr;

    size_t workspacePosition = 0;
    Workspace* workspace = workspaceAtPoint(lx, ly, &state, &workspacePosition, true, true);
    m_drop = {.workspace = workspace};
    if (workspace == nullptr || state == nullptr) {
      hideDropHint();
      scheduleFrames();
      return;
    }

    PreviewMetrics metrics{};
    if (!previewMetrics(*state, *m_server, zoom(), metrics)) {
      hideDropHint();
      return;
    }
    // Map the pointer out of the thumbnail and back into workspace world space. A mirror row shows another workspace
    // through its mapping, so the pointer maps on into that workspace's own space and a drop lands in it.
    const wlr_box preview = previewBox(metrics, state->rowScroll.current(), workspacePosition);
    double worldX = metrics.outputBox.x + (lx - preview.x) / metrics.zoom;
    double worldY = metrics.outputBox.y + (ly - preview.y) / metrics.zoom;
    Workspace* layout = workspace;
    std::optional<MirrorMapping> mapping;
    if (workspace->mirror()) {
      mapping = mirrorRowMapping(*workspace);
      layout = workspace->layoutOwner();
      if (layout == nullptr || !mapping) {
        hideDropHint();
        scheduleFrames();
        return;
      }
      worldX = mapping->toSourceX(worldX);
      worldY = mapping->toSourceY(worldY);
    }

    if (card->view->tiled()) {
      // The overview applies its own projection after target selection. Keep
      // strip hints attached to content that is outside the normal viewport but
      // visible in the overview margin. Other layouts retain their normal
      // usable-area bounds.
      const bool scrolling = layout->scrollingLayout() != nullptr;
      m_drop = computeDropTarget(
          *layout, worldX, worldY, card->view,
          DropTargetOptions{
              .clipHintToUsable = !scrolling,
              .reserveScrollingViewportEdges = false,
              .endpointGapsOutsideColumns = scrolling,
          }
      );
      // The target keeps the row under the pointer; endDrag resolves the workspace it shows.
      if (m_drop.workspace == layout) {
        m_drop.workspace = workspace;
      }
    } else {
      m_drop = {
          .workspace = workspace,
          .column = static_cast<int>(layout->layout().columns().size()),
      };
    }
    wlr_box hint = m_drop.hintBox;
    if (mapping) {
      hint = mapping->toReceiver(hint);
    }
    if (hint.width > 0 && hint.height > 0) {
      showDropHint(hint, metrics, state->rowScroll.current(), workspacePosition, state->output);
    } else {
      hideDropHint();
    }
    scheduleFrames();
  }

  void Overview::endDrag(bool drop) {
    Card* card = m_dragCard;
    if (card == nullptr) {
      return;
    }
    // A finger-scroll still in flight settles pan-only before the drop resolves: left armed, its next frames
    // take the normal navigation path (filmstrip step, focus-moving settle) after the drop and strand keyboard
    // focus off the dropped window.
    if (m_navigationPointer != nullptr && m_navigationSource == NavigationSource::Scroll) {
      endNavigation(false, m_scrollTime, NavigationSource::Scroll);
    }
    View* view = card->view;
    WorkspaceGroup* insertionGroup = drop ? m_dropWorkspaceGroup : nullptr;
    // A mirror row drops into the workspace it shows, never into the mirror.
    Workspace* dropRow = drop ? m_drop.workspace : nullptr;
    Workspace* target = dropRow != nullptr ? dropRow->layoutOwner() : nullptr;
    if (dropRow != nullptr && dropRow->mirror() && !mirrorRowMapping(*dropRow)) {
      target = nullptr;
    }
    DropTarget targetDrop = m_drop;
    targetDrop.workspace = target;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(dropRow) : nullptr;

    m_dragCard = nullptr;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    hideDropHint();
    m_server->cursor()->overrideCursor(nullptr);
    // A card drag is a drag this timer may be serving: edge auto-scroll dies with it (mirror handleDataDragEnded).
    m_server->cursor()->handleDataDragEnded();

    if (view == nullptr || !view->mapped()) {
      settleRowsOnActiveWorkspace();
      return;
    }

    bool insertedWorkspace = false;
    if (insertionGroup != nullptr) {
      target = insertionGroup->insertDynamicWorkspace(m_dropWorkspaceIndex);
      dropRow = target;
      dropState = target != nullptr ? stateForWorkspace(target) : nullptr;
      if (target != nullptr && dropState != nullptr) {
        targetDrop = {
            .workspace = target,
            .column = 0,
        };
        insertedWorkspace = true;
      }
    }

    if (target != nullptr && target->layoutMode() == LayoutMode::Stacking && view->tiled()) {
      view->floatForStackingLayout();
    }
    if (target != nullptr && view->tiled()) {
      applyDrop(
          *m_server, *view, *target, targetDrop, m_dragSourceWidth.has_value() ? &*m_dragSourceWidth : nullptr,
          /*animate=*/false
      );
    } else if (target != nullptr && dropState != nullptr && insertedWorkspace) {
      view->rememberFloatingPosition();
      if (view->workspace() != target) {
        view->moveToWorkspace(target);
      }
      if (view->floating()) {
        view->restoreFloatingPosition();
      }
      m_server->focusView(view, FocusReason::DragDrop);
    } else if (target != nullptr && dropState != nullptr) {
      // Floating: map the card origin back out of the thumbnail.
      PreviewMetrics metrics{};
      if (previewMetrics(*dropState, *m_server, zoom(), metrics)) {
        const wlr_box preview = previewBox(metrics, dropState->rowScroll.current(), dropRow->index());
        int x = metrics.outputBox.x + static_cast<int>(std::lround((cardBox.x - preview.x) / metrics.zoom));
        int y = metrics.outputBox.y + static_cast<int>(std::lround((cardBox.y - preview.y) / metrics.zoom));
        if (const std::optional<MirrorMapping> mapping =
                dropRow->mirror() ? mirrorRowMapping(*dropRow) : std::nullopt) {
          x = static_cast<int>(std::lround(mapping->toSourceX(x)));
          y = static_cast<int>(std::lround(mapping->toSourceY(y)));
        }
        if (view->workspace() != target) {
          view->moveToWorkspace(target);
          target->exitFullscreenForIncomingView(view);
        }
        if (view->floating()) {
          view->setPosition(x, y);
          view->rememberFloatingPosition();
        }
        // Cards are laid out from the presented box, which setPosition does not refresh.
        target->syncViewPresentation(view);
      }
    } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
      // Cancelled or dropped on nothing: put the tile back where it came from.
      if (m_dragSourceWorkspace->layoutMode() == LayoutMode::Stacking) {
        m_dragSourceWorkspace->layoutAttach(view);
      } else if (m_dragSourceWidth.has_value()) {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
        const int column = m_dragSourceWorkspace->layout().columnOf(view);
        m_dragSourceWorkspace->layout().setWidthFraction(column, m_dragSourceWidth->fraction);
        if (m_dragSourceWidth->fullWidth) {
          m_dragSourceWorkspace->layout().toggleFullWidth(column);
        }
        view->setMaximizedState(m_dragSourceWidth->fullWidth);
      } else if (m_dragSourceWorkspace->dwindleLayout() != nullptr) {
        // Gap-index insert restores the exact flat position the drag removed.
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
      } else if (m_dragSourceRow >= 0) {
        m_dragSourceWorkspace->layout().insertViewIntoColumn(view, m_dragSourceColumn, m_dragSourceRow);
      } else {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
      }
      m_dragSourceWorkspace->arrange(false);
    }
    if (!m_dragTabGroup.tabs.empty()) {
      landTabGroup(*view, std::exchange(m_dragTabGroup, {}), /*closing=*/false);
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCards(view);
    applyProgress();
    settleRowsOnActiveWorkspace();
  }

  void Overview::settleRowsOnActiveWorkspace() {
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || group->active() == nullptr) {
        continue;
      }
      const auto row = static_cast<double>(group->active()->index());
      if (std::abs(state->rowScroll.target() - row) >= 0.001) {
        animateRow(*state, row);
      }
    }
  }

  void Overview::syncWorkspaceRows(OutputState& state, WorkspaceGroup& group) {
    const bool grew = state.workspaceBackgrounds.size() < group.workspaceCount();
    while (state.workspaceBackgrounds.size() > group.workspaceCount()) {
      WorkspaceBackground background = std::move(state.workspaceBackgrounds.back());
      state.workspaceBackgrounds.pop_back();
      for (const auto& mirror : background.mirrors) {
        wl_list_remove(&mirror->outputSample.link);
        wl_list_remove(&mirror->frameDone.link);
      }
      if (background.tree != nullptr) {
        wlr_scene_node_destroy(&background.tree->node);
      }
    }
    while (state.workspaceBackgrounds.size() < group.workspaceCount()) {
      state.workspaceBackgrounds.push_back(createWorkspaceBackground(state));
    }
    if (grew) {
      syncDesktopMirrors(state);
    }
    for (const auto& card : state.cards) {
      card->workspaceIndex = card->row->index();
    }
  }

  void Overview::showDropHint(
      const wlr_box& worldBox, const PreviewMetrics& metrics, double workspaceScroll, size_t workspaceIndex,
      Output* output
  ) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    const double z = metrics.zoom;
    const wlr_box preview = previewBox(metrics, workspaceScroll, workspaceIndex);
    const wlr_box mappedBox{
        .x = preview.x + static_cast<int>(std::lround((worldBox.x - metrics.outputBox.x) * z)),
        .y = preview.y + static_cast<int>(std::lround((worldBox.y - metrics.outputBox.y) * z)),
        .width = std::max(1, static_cast<int>(std::lround(worldBox.width * z))),
        .height = std::max(1, static_cast<int>(std::lround(worldBox.height * z))),
    };
    wlr_box visibleBox{};
    if (!wlr_box_intersection(&visibleBox, &mappedBox, &metrics.outputBox)) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(
        output, visibleBox, static_cast<int>(std::lround(config().appearance.cornerRadius * metrics.zoom))
    );
    if (m_dragCard != nullptr && m_dragCard->preview != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->preview->tree()->node);
    }
  }

  void Overview::showWorkspaceInsertHint(Output* output, const wlr_box& box) {
    if (output == nullptr || box.width <= 0 || box.height <= 0) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(output, box, box.height / 2);
    if (m_dragCard != nullptr && m_dragCard->preview != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->preview->tree()->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      m_dropHint->hideImmediate();
    }
  }

} // namespace umbriel
