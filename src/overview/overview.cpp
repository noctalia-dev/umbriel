#include "overview/overview.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "output/output.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/hint_rect.h"
#include "server/server.h"
#include "server/wine_color_manager.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <ranges>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("overview");

    // Gap between workspace thumbnails, as a fraction of the scaled row height.
    constexpr double kRowGapFraction = 0.1;
    // Pointer travel that promotes a press on a card into a relocate drag.
    constexpr double kDragThreshold = 10.0;

    bool boxContains(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    // wlr_scene_rect colors are premultiplied; straight alpha renders as an
    // over-bright wash (scene/color.h).
    std::array<float, 4> tint(const std::array<float, 4>& base, double opacity) {
      std::array<float, 4> out{};
      premultiplied(out.data(), base, static_cast<float>(opacity));
      return out;
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
  } // namespace

  Overview::Overview(Server& server) : m_server(&server) { m_server->registerAnimatable(this); }

  Overview::~Overview() {
    m_server->unregisterAnimatable(this);
    m_anim.snap(0.0);
    teardown();
  }

  double Overview::zoom() const {
    const double configured = std::clamp(config().overview.zoom, 0.1, 0.75);
    return 1.0 - m_progress * (1.0 - configured);
  }

  // -: geometry

  bool Overview::rowMetrics(const OutputState& state, const Server& server, double zoom, RowMetrics& out) {
    wlr_box outputBox{};
    wlr_output_layout_get_box(server.outputLayout(), state.output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }
    out.outputBox = outputBox;
    out.zoom = zoom;
    out.rowW = std::max(1, static_cast<int>(std::lround(outputBox.width * zoom)));
    out.rowH = std::max(1, static_cast<int>(std::lround(outputBox.height * zoom)));
    out.rowX = outputBox.x + static_cast<int>(std::lround((outputBox.width - out.rowW) / 2.0));
    out.baseY = outputBox.y + (outputBox.height - out.rowH) / 2.0;
    out.gap = kRowGapFraction * outputBox.height * zoom;
    return true;
  }

  int Overview::rowTop(const RowMetrics& metrics, double rowScroll, size_t row) {
    const double offset = (static_cast<double>(row) - rowScroll) * (metrics.rowH + metrics.gap);
    return static_cast<int>(std::lround(metrics.baseY + offset));
  }

  void Overview::layoutCard(Card& card, const RowMetrics& metrics, double rowScroll) {
    View* view = card.view;
    const wlr_box& geometry = view->toplevel()->base->geometry;
    if (geometry.width <= 0 || geometry.height <= 0) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      return;
    }
    wlr_scene_node_set_enabled(&card.tree->node, true);

    const double z = metrics.zoom;
    const wlr_box& world = view->presentedBox();
    if (world.width <= 0 || world.height <= 0) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      return;
    }
    const int contentW = std::max(1, static_cast<int>(std::lround(world.width * z)));
    const int contentH = std::max(1, static_cast<int>(std::lround(world.height * z)));
    if (&card == m_dragCard) {
      // The drag owns the card origin; only the scale still tracks progress.
      card.box.width = contentW;
      card.box.height = contentH;
    } else {
      card.box = {
          .x = metrics.rowX + static_cast<int>(std::lround((world.x - metrics.outputBox.x) * z)),
          .y =
              rowTop(metrics, rowScroll, card.row) + static_cast<int>(std::lround((world.y - metrics.outputBox.y) * z)),
          .width = contentW,
          .height = contentH,
      };
    }
    wlr_scene_node_set_position(&card.tree->node, card.box.x, card.box.y);
    const bool clipToRow = view->workspace() != nullptr && view->workspace()->scrollingVertical();
    if (&card == m_dragCard || !clipToRow) {
      wlr_scene_tree_set_clip(card.tree, nullptr);
    } else {
      // A vertical strip can extend into the next workspace row. Keep the row
      // as a live viewport while focus and scroll actions move cards through it.
      const wlr_box rowClip{
          .x = metrics.rowX - card.box.x,
          .y = rowTop(metrics, rowScroll, card.row) - card.box.y,
          .width = metrics.rowW,
          .height = metrics.rowH,
      };
      wlr_scene_tree_set_clip(card.tree, &rowClip);
    }
    const float cardOpacity = &card == m_dragCard ? config().appearance.dragOpacity : 1.0F;
    const float presentedOpacity = view->presentedOpacity() * cardOpacity;

    const auto& appearance = config().appearance;
    const int total = appearance.totalBorderWidth();
    const bool decorated = total > 0 && !view->toplevel()->current.fullscreen && !view->maximizedToEdges();
    const int outerRadius = decorated ? static_cast<int>(std::lround(appearance.cornerRadius * z)) : 0;
    const auto scaledWidth = [z](int width) {
      return width > 0 ? std::max(1, static_cast<int>(std::lround(width * z))) : 0;
    };
    const int innerWidth = scaledWidth(appearance.borderWidth);
    const int outerWidth = scaledWidth(appearance.outerBorderWidth);
    const int surfaceRadius = nestedRadius(outerRadius, innerWidth + outerWidth);
    const bool borderVisible = decorated && innerWidth + outerWidth > 0;
    wlr_scene_node_set_enabled(&card.border->node, borderVisible);
    if (borderVisible) {
      applyBorderGeometry(
          card.border, makeBorderRing(contentW, contentH, outerRadius, innerWidth, outerWidth), innerWidth, outerWidth
      );
      // Every row advertises its own focused window, not just the active one:
      // the filmstrip is a browsing aid, and the border identifies its target.
      const Workspace* workspace = view->workspace();
      const bool focused = workspace != nullptr && workspace->focusedView() == view && m_dragCard != &card;
      const std::array<float, 4> innerColor =
          tint(focused ? appearance.borderFocused : appearance.borderUnfocused, presentedOpacity);
      const std::array<float, 4> outerColor = tint(appearance.outerBorderColor, presentedOpacity);
      wlr_scene_border_set_colors(card.border, innerColor.data(), outerColor.data());
    }

    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    bool blurUpdated = false;
    for (const auto& entry : card.surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr || surface->current.width <= 0 || surface->current.height <= 0) {
        continue;
      }
      const float surfaceOpacity =
          entry->sourceBuffer != nullptr ? entry->sourceBuffer->opacity * cardOpacity : cardOpacity;
      wlr_scene_buffer_set_opacity(entry->buffer, surfaceOpacity);
      if (!entry->isRoot) {
        wlr_box sub{
            card.box.x + static_cast<int>(std::lround((entry->sx - geometry.x) * fx)),
            card.box.y + static_cast<int>(std::lround((entry->sy - geometry.y) * fy)),
            std::max(1, static_cast<int>(std::lround(surface->current.width * fx))),
            std::max(1, static_cast<int>(std::lround(surface->current.height * fy))),
        };
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x - card.box.x, sub.y - card.box.y);
        wlr_scene_buffer_set_dest_size(entry->buffer, sub.width, sub.height);
        continue;
      }
      // Root surface: crop to the committed window geometry so CSD shadow padding never leaks into the thumbnail, then
      // scale the visible part of that region onto the card.
      wlr_fbox base{};
      wlr_surface_get_buffer_source_box(surface, &base);
      const double bx = base.width / surface->current.width;
      const double by = base.height / surface->current.height;
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
      wlr_scene_node_set_enabled(&entry->buffer->node, true);
      wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
      wlr_scene_buffer_set_source_box(entry->buffer, &src);
      wlr_scene_buffer_set_dest_size(entry->buffer, contentW, contentH);
      wlr_scene_buffer_set_corner_radii(entry->buffer, corner_radii_all(surfaceRadius));
      const wlr_box blurBox{0, 0, contentW, contentH};
      card.blur.setAlpha(1.0F);
      card.blur.update(
          card.tree, surface, blurBox, geometry, surfaceRadius, nullptr, view->blurOptions(), entry->buffer->opacity,
          entry->buffer
      );
      blurUpdated = true;
    }
    if (!blurUpdated) {
      card.blur.hide();
    }
  }

  void Overview::layoutOutput(OutputState& state) {
    RowMetrics metrics{};
    if (!rowMetrics(state, *m_server, zoom(), metrics)) {
      return;
    }

    // Rows overhang the output by design (adjacent workspaces peek in). One clip on this output's overview tree
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
      const auto level = static_cast<float>(m_progress);
      if (blur->alpha != level) {
        wlr_scene_blur_set_alpha(blur, level);
      }
      if (blur->strength != level) {
        wlr_scene_blur_set_strength(blur, level);
      }
    }

    wlr_scene_node_set_position(&state.backgroundTint->node, metrics.outputBox.x, metrics.outputBox.y);
    wlr_scene_rect_set_size(state.backgroundTint, metrics.outputBox.width, metrics.outputBox.height);
    const std::array<float, 4> backgroundTint = tint(config().overview.backgroundTint, m_progress);
    // A fully transparent tint leaves the wallpaper untouched.
    wlr_scene_node_set_enabled(&state.backgroundTint->node, backgroundTint[3] > 0.001F);
    wlr_scene_rect_set_color(state.backgroundTint, backgroundTint.data());

    const int backgroundRadius = static_cast<int>(std::lround(config().appearance.cornerRadius * metrics.zoom));
    const std::array<float, 4> backgroundColor = tint(config().overview.workspaceBackground, m_progress);
    for (size_t row = 0; row < state.workspaceBackgrounds.size(); ++row) {
      wlr_scene_rect* background = state.workspaceBackgrounds[row];
      const wlr_box full{metrics.rowX, rowTop(metrics, state.rowScroll, row), metrics.rowW, metrics.rowH};
      layoutWorkspaceBackground(background, full, backgroundRadius, backgroundColor);
    }

    for (const auto& card : state.cards) {
      layoutCard(*card, metrics, state.rowScroll);
    }
  }

  void Overview::applyProgress() {
    for (const auto& state : m_outputs) {
      layoutOutput(*state);
    }
    scheduleFrames();
  }

  void Overview::scheduleFrames() const {
    for (const auto& state : m_outputs) {
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  // -: cards

  void Overview::syncCardBuffer(CardSurface& entry) {
    wlr_surface* surface = entry.surface;
    if (surface == nullptr || entry.buffer == nullptr) {
      return;
    }

    wlr_scene_buffer_set_buffer_options options{
        .damage = &surface->buffer_damage,
        .wait_timeline = nullptr,
        .wait_point = 0,
    };
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
      options.wait_timeline = sync->acquire_timeline;
      options.wait_point = sync->acquire_point;
    }
    // A scene buffer may clear its `buffer` pointer after importing a texture and releasing the client buffer. The
    // surface retains the authoritative committed buffer, including for hidden workspaces.
    wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
    if (committed == nullptr) {
      options.damage = nullptr;
    }
    wlr_scene_buffer_set_buffer_with_options(entry.buffer, committed, &options);

    // Presentation state comes from the surface's committed state, NEVER from the view's scene buffers: workspace
    // slides clip those (applyPresentation → applyPresentedCrop), and a window parked on a hidden workspace keeps the
    // sliver crop. Copying it smears subsurface-presented content (games) into a single stretched line. layoutCard then
    // re-crops the root surface and re-scales every entry for the thumbnail.
    wlr_fbox src{};
    wlr_surface_get_buffer_source_box(surface, &src);
    wlr_scene_buffer_set_source_box(entry.buffer, &src);
    wlr_scene_buffer_set_dest_size(entry.buffer, surface->current.width, surface->current.height);
    wlr_scene_buffer_set_transform(entry.buffer, surface->current.transform);
    wlr_scene_buffer_set_opaque_region(entry.buffer, &surface->opaque_region);

    // Protocol-derived display properties (alpha-modifier, color management)
    // are clip-independent, so the view's scene buffer is a safe source.
    if (wlr_scene_buffer* source = entry.sourceBuffer) {
      wlr_scene_buffer_set_opacity(entry.buffer, source->opacity);
      wlr_scene_buffer_set_transfer_function(entry.buffer, source->transfer_function);
      wlr_scene_buffer_set_primaries(entry.buffer, source->primaries);
      wlr_scene_buffer_set_luminance_multiplier(entry.buffer, source->luminance_multiplier);
      wlr_scene_buffer_set_color_encoding(entry.buffer, source->color_encoding);
      wlr_scene_buffer_set_color_range(entry.buffer, source->color_range);
    }
    // wlroots restores scene surfaces to its protocol-owned color state on
    // every commit. Wine compatibility metadata is repaired at the render
    // boundary, after this passive mirror has copied the transient state, so
    // apply the authoritative committed description directly to the mirror.
    if (WineColorManager* colorManager = entry.card->overview->m_server->wineColorManager()) {
      colorManager->applySurfaceDescriptionToBuffer(surface, entry.buffer);
    }
  }

  void Overview::addCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    wlr_scene_buffer* source = sourceBufferForSurface(&card->view->sceneTree()->node, surface);
    if (source == nullptr) {
      return;
    }
    wlr_scene_buffer* buffer = wlr_scene_buffer_create(card->tree, nullptr);
    if (buffer == nullptr) {
      return;
    }
    auto entry = std::make_unique<CardSurface>();
    entry->card = card;
    entry->surface = surface;
    entry->sourceBuffer = source;
    entry->buffer = buffer;
    entry->sx = sx;
    entry->sy = sy;
    entry->isRoot = surface == card->view->toplevel()->base->surface;
    wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
    buffer->point_accepts_input = rejectInput;
    entry->commit.notify = onCardSurfaceCommit;
    wl_signal_add(&surface->events.commit, &entry->commit);
    entry->destroy.notify = onCardSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    entry->outputSample.notify = onCardBufferOutputSample;
    wl_signal_add(&buffer->events.output_sample, &entry->outputSample);
    entry->frameDone.notify = onCardBufferFrameDone;
    wl_signal_add(&buffer->events.frame_done, &entry->frameDone);
    syncCardBuffer(*entry);
    card->surfaces.push_back(std::move(entry));
    if (card->border != nullptr) {
      wlr_scene_node_raise_to_top(&card->border->node);
    }
  }

  void Overview::syncCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    for (const auto& entry : card->surfaces) {
      if (entry->surface == surface) {
        entry->sx = sx;
        entry->sy = sy;
        syncCardBuffer(*entry);
        return;
      }
    }
    addCardSurface(surface, sx, sy, data);
  }

  void Overview::onCardSurfaceCommit(wl_listener* listener, void* /*data*/) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, commit);
    Card* card = entry->card;
    Overview* self = card->overview;
    if (!self->m_active || card->owner == nullptr) {
      return;
    }
    // The source scene surface reconfigures on every commit. Refresh the
    // passive buffer mirrors, then re-derive their overview crop and scale.
    wlr_surface_for_each_surface(card->view->toplevel()->base->surface, syncCardSurface, card);
    RowMetrics metrics{};
    if (rowMetrics(*card->owner, *self->m_server, self->zoom(), metrics)) {
      self->layoutCard(*card, metrics, card->owner->rowScroll);
      wlr_output_schedule_frame(card->owner->output->wlr());
    }
  }

  void Overview::onCardSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, destroy);
    Card* card = entry->card;
    wl_list_remove(&entry->commit.link);
    wl_list_remove(&entry->destroy.link);
    wl_list_remove(&entry->outputSample.link);
    wl_list_remove(&entry->frameDone.link);
    if (entry->buffer != nullptr) {
      wlr_scene_node_destroy(&entry->buffer->node);
      entry->buffer = nullptr;
    }
    std::erase_if(card->surfaces, [entry](const std::unique_ptr<CardSurface>& candidate) {
      return candidate.get() == entry;
    });
  }

  void Overview::onCardBufferOutputSample(wl_listener* listener, void* data) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, outputSample);
    auto* event = static_cast<wlr_scene_output_sample_event*>(data);
    wlr_output* output = event->output->output;
    if (event->direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(entry->surface, output);
    } else {
      wlr_presentation_surface_textured_on_output(entry->surface, output);
    }
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(entry->surface);
        sync != nullptr && event->release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event->release_timeline, event->release_point, output->event_loop
      );
    }
  }

  void Overview::onCardBufferFrameDone(wl_listener* listener, void* data) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, frameDone);
    auto* event = static_cast<wlr_scene_frame_done_event*>(data);
    wlr_surface_send_frame_done(entry->surface, &event->when);
  }

  Overview::Card* Overview::createCard(OutputState& state, View* view, size_t row) {
    wlr_surface* surface = view->toplevel()->base->surface;
    if (surface == nullptr) {
      return nullptr;
    }
    auto card = std::make_unique<Card>();
    card->overview = this;
    card->owner = &state;
    card->view = view;
    card->row = row;
    card->tree = wlr_scene_tree_create(state.tree);
    if (card->tree == nullptr) {
      return nullptr;
    }
    const std::array<float, 4> innerColor = tint(config().appearance.borderUnfocused, 1.0);
    const std::array<float, 4> outerColor = tint(config().appearance.outerBorderColor, 1.0);
    card->border = wlr_scene_border_create(card->tree, innerColor.data(), outerColor.data());
    if (card->border == nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      return nullptr;
    }
    Card* raw = card.get();
    state.cards.push_back(std::move(card));

    wlr_surface_for_each_surface(surface, addCardSurface, raw);

    // Animation schedules the first output frame. The passive card buffers then
    // pace clients from frames where their content was actually sampled.
    return raw;
  }

  void Overview::snapshotCardForClose(Card& card) {
    if (card.tree == nullptr || card.owner == nullptr || card.owner->output == nullptr || card.view == nullptr) {
      return;
    }

    wlr_scene_tree* snapshot = wlr_scene_tree_create(m_tree);
    if (snapshot == nullptr) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), card.owner->output->wlr(), &outputBox);
    if (outputBox.width > 0 && outputBox.height > 0) {
      wlr_scene_tree_set_clip(snapshot, &outputBox);
    }

    int buffersCopied = 0;
    for (const auto& entry : card.surfaces) {
      wlr_scene_buffer* source = entry->buffer;
      if (source == nullptr || source->buffer == nullptr || !source->node.enabled) {
        continue;
      }
      wlr_scene_buffer* copy = wlr_scene_buffer_create(snapshot, source->buffer);
      if (copy == nullptr) {
        continue;
      }
      wlr_scene_node_set_position(&copy->node, card.tree->node.x + source->node.x, card.tree->node.y + source->node.y);
      if (source->dst_width > 0 && source->dst_height > 0) {
        wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
      }
      if (source->src_box.width > 0 && source->src_box.height > 0) {
        wlr_scene_buffer_set_source_box(copy, &source->src_box);
      }
      wlr_scene_buffer_set_transform(copy, source->transform);
      wlr_scene_buffer_set_corner_radii(copy, source->corners);
      wlr_scene_buffer_set_opacity(copy, source->opacity);
      wlr_scene_buffer_set_transfer_function(copy, source->transfer_function);
      wlr_scene_buffer_set_primaries(copy, source->primaries);
      wlr_scene_buffer_set_luminance_multiplier(copy, source->luminance_multiplier);
      wlr_scene_buffer_set_color_encoding(copy, source->color_encoding);
      wlr_scene_buffer_set_color_range(copy, source->color_range);
      wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
      ++buffersCopied;
    }

    std::vector<BorderSnapshot> borders;
    if (card.border != nullptr && card.border->node.enabled) {
      wlr_scene_border* copy = wlr_scene_border_create(snapshot, card.border->inner_color, card.border->outer_color);
      if (copy != nullptr) {
        wlr_scene_border_set_geometry(
            copy, card.border->width, card.border->height, card.border->inner_width, card.border->outer_width,
            card.border->clipped_region, card.border->seam_corners, card.border->outer_corners
        );
        wlr_scene_node_set_position(
            &copy->node, card.tree->node.x + card.border->node.x, card.tree->node.y + card.border->node.y
        );
        const Workspace* workspace = card.view->workspace();
        const bool focused = workspace != nullptr && workspace->focusedView() == card.view;
        std::array<float, 4> innerColor =
            focused ? config().appearance.borderFocused : config().appearance.borderUnfocused;
        std::array<float, 4> outerColor = config().appearance.outerBorderColor;
        const float presentedOpacity = card.view->presentedOpacity();
        innerColor[3] *= presentedOpacity;
        outerColor[3] *= presentedOpacity;
        borders.push_back(
            BorderSnapshot{
                .node = copy,
                .innerColor = innerColor,
                .outerColor = outerColor,
            }
        );
      }
    }

    if (buffersCopied == 0 && borders.empty()) {
      wlr_scene_node_destroy(&snapshot->node);
      return;
    }
    m_server->animateCloseSnapshot(card.owner->output, snapshot, std::move(borders));
    wlr_output_schedule_frame(card.owner->output->wlr());
  }

  void Overview::destroyCard(Card* card) {
    for (const auto& entry : card->surfaces) {
      wl_list_remove(&entry->commit.link);
      wl_list_remove(&entry->destroy.link);
      wl_list_remove(&entry->outputSample.link);
      wl_list_remove(&entry->frameDone.link);
    }
    card->surfaces.clear();
    if (card->tree != nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      card->tree = nullptr;
    }
    card->border = nullptr;
  }

  void Overview::dropCard(View* view) {
    for (const auto& state : m_outputs) {
      const auto it =
          std::ranges::find_if(state->cards, [view](const std::unique_ptr<Card>& card) { return card->view == view; });
      if (it == state->cards.end()) {
        continue;
      }
      destroyCard(it->get());
      state->cards.erase(it);
      return;
    }
  }

  void Overview::rebuildCard(View* view) {
    dropCard(view);
    Workspace* workspace = view->workspace();
    if (workspace == nullptr || !view->mapped()) {
      return;
    }
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr) {
      return;
    }
    createCard(*state, view, workspace->index());
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
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

  Overview::Card* Overview::findCard(const View* view) {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  void Overview::populateCards(OutputState& state) {
    WorkspaceGroup* group = state.output->workspaceGroup();
    if (group == nullptr) {
      return;
    }
    for (size_t row = 0; row < group->workspaceCount(); ++row) {
      Workspace* workspace = group->workspaceAt(row);
      if (workspace == nullptr) {
        continue;
      }
      // Tiled, then floating, then fullscreen: mirrors the per-workspace scene
      // layer split so overlapping cards stack the way the real windows do.
      for (int pass = 0; pass < 3; ++pass) {
        for (View* view : workspace->allViews()) {
          if (view == nullptr || !view->mapped()) {
            continue;
          }
          const bool fullscreen = view->toplevel()->current.fullscreen;
          const int layer = fullscreen ? 2 : (view->tiled() ? 0 : 1);
          if (layer == pass) {
            createCard(state, view, row);
          }
        }
      }
    }
  }

  void Overview::buildState() {
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
      const std::array<float, 4> backgroundTint = tint(config().overview.backgroundTint, 0.0);
      state->backgroundTint = wlr_scene_rect_create(state->tree, 1, 1, backgroundTint.data());
      wlr_scene_rect_set_corner_radius(state->backgroundTint, 0);
      state->workspaceBackgrounds.reserve(group->workspaceCount());
      const std::array<float, 4> backgroundColor = tint(config().overview.workspaceBackground, 0.0);
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->workspaceBackgrounds.push_back(wlr_scene_rect_create(state->tree, 1, 1, backgroundColor.data()));
      }
      state->rowScroll = group->active() != nullptr ? static_cast<double>(group->active()->index()) : 0.0;
      state->rowFrom = state->rowScroll;
      state->rowTo = state->rowScroll;
      OutputState* raw = state.get();
      m_outputs.push_back(std::move(state));
      populateCards(*raw);
    }
  }

  // -: open/close

  bool Overview::beginPresentation() {
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

    buildState();
    if (m_outputs.empty()) {
      return false;
    }

    m_active = true;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pendingFocus = nullptr;
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

    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, false);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, false);
    wlr_scene_node_set_enabled(&m_tree->node, true);

    m_server->clearKeyboardFocus();
    wlr_seat_pointer_clear_focus(m_server->seat()->wlr());
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
    if (m_active) {
      m_closing = false;
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

  void Overview::closeToWorkspace(Workspace* workspace, View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->active() != workspace) {
      // No slide: the real trees are hidden, the filmstrip is the transition.
      workspace->group()->activate(workspace, false);
    }
    if (focus != nullptr && focus->mapped()) {
      // Overview focus keeps keyboard input withheld, but it updates the workspace and starts any scrolling-column
      // reveal. Begin it before the closing zoom so both animations receive their first tick together. finishAnimation
      // repeats the focus after teardown to deliver keyboard focus once the real trees own input again.
      m_server->focusView(focus, FocusReason::PointerPress);
    }
    beginClose(focus);
  }

  void Overview::beginClose(View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    hideDropHint();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_pendingFocus = focus;
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      state->rowFrom = state->rowScroll;
      state->rowTo = (group != nullptr && group->active() != nullptr) ? static_cast<double>(group->active()->index())
                                                                      : state->rowScroll;
    }
    startAnimation(0.0, true);
  }

  void Overview::forceClose() {
    if (!m_active) {
      return;
    }
    m_anim.snap(0.0);
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    m_server->refocus();
  }

  void Overview::startAnimation(double target, bool closing) {
    m_closing = closing;
    m_targetProgress = target;
    m_progressFrom = m_progress;
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      m_anim.snap(1.0);
      m_progress = target;
      for (const auto& state : m_outputs) {
        state->rowScroll = state->rowTo;
      }
      finishAnimation();
      return;
    }
    m_anim.snap(0.0);
    m_anim.retarget(1.0, overview.durationMs, overview.curve);
    // Animations only tick from an output frame; kick one so the zoom starts on
    // an idle desktop (the value itself clocks from its first tick).
    scheduleFrames();
  }

  bool Overview::tickAnimations(uint64_t nowMsec) {
    bool active = m_dropHint != nullptr && m_dropHint->tickAnimations(nowMsec);
    if (m_anim.tick(nowMsec)) {
      const double value = m_anim.current();
      m_progress = m_progressFrom + (m_targetProgress - m_progressFrom) * value;
      for (const auto& state : m_outputs) {
        state->rowScroll = state->rowFrom + (state->rowTo - state->rowFrom) * value;
      }
      applyProgress();
      if (!m_anim.animating()) {
        // May tear down m_outputs; safe now that the loop above is done.
        finishAnimation();
      }
      active = m_anim.animating() || active;
    } else if (m_cardPresentationDirty) {
      applyProgress();
    }
    m_cardPresentationDirty = false;
    return active;
  }

  bool Overview::hasActiveAnimations() const {
    return m_anim.animating() || (m_dropHint != nullptr && m_dropHint->hasActiveAnimations());
  }

  void Overview::finishAnimation() {
    if (!m_closing) {
      applyProgress();
      return;
    }
    View* focus = m_pendingFocus;
    m_pendingFocus = nullptr;
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
    if (focus != nullptr && focus->mapped()) {
      m_server->focusView(focus, FocusReason::PointerPress);
    } else {
      m_server->refocus();
    }
  }

  void Overview::teardown() {
    if (m_dropHint != nullptr) {
      m_dropHint->hideImmediate();
    }
    for (const auto& state : m_outputs) {
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

    m_active = false;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_server->notifyOverviewChanged();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_cardPresentationDirty = false;
    m_gestureOpenedHere = false;
    m_server->reconcileDynamicWorkspaces();
  }

  // -: gesture

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
    }
    m_anim.snap(0.0);
    m_closing = false;
    m_progress = progress;
    m_targetProgress = progress;
    applyProgress();
  }

  void Overview::gestureEnd(bool commitOpen) {
    if (!m_active) {
      return;
    }
    m_gestureOpenedHere = false;
    if (commitOpen) {
      startAnimation(1.0, false);
      return;
    }
    beginClose(nullptr);
  }

  // -: hooks

  void Overview::onViewMapped(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr || findCard(view) != nullptr) {
      return;
    }
    createCard(*state, view, workspace->index());
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
  }

  void Overview::onViewUnmapped(View* view) {
    if (!m_active || view == nullptr) {
      return;
    }
    if (m_pendingFocus == view) {
      m_pendingFocus = nullptr;
    }
    if (m_pressCard != nullptr && m_pressCard->view == view) {
      m_pressCard = nullptr;
    }
    if (m_drop.view == view) {
      m_drop.view = nullptr;
      m_drop.edge = 0;
      hideDropHint();
    }
    if (m_dragCard != nullptr && m_dragCard->view == view) {
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
      m_dropWorkspaceGroup = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (Card* card = findCard(view)) {
      snapshotCardForClose(*card);
    }
    dropCard(view);
    // The closed window may have been the focused one. The overview keeps the focus chrome while it owns the seat, so
    // reassign to the nearest survivor now rather than leaving the workspace focused on a dead view until zoom-out (or
    // a later destroy) happens to refocus. Ask before layout detachment so the closing view still identifies its row
    // and column, preferring its predecessor and using the next neighbor only at the leading edge.
    if (workspace != nullptr && workspace->focusedView() == view) {
      View* replacement = workspace->focusReplacementForRemoval(view);
      if (replacement != nullptr) {
        if (workspace->active()) {
          m_server->focusView(replacement, FocusReason::Startup);
        } else {
          workspace->setFocusedView(replacement);
        }
      } else {
        workspace->setFocusedView(nullptr);
      }
    }
    if (state != nullptr) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onViewWorkspaceChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }

    Card* card = findCard(view);
    Workspace* workspace = view->workspace();
    OutputState* target = stateForWorkspace(workspace);
    if (card == nullptr) {
      if (target != nullptr) {
        createCard(*target, view, workspace->index());
        layoutOutput(*target);
        wlr_output_schedule_frame(target->output->wlr());
      }
      return;
    }

    OutputState* source = card->owner;
    if (target == nullptr) {
      if (m_pressCard == card) {
        m_pressCard = nullptr;
      }
      dropCard(view);
      if (source != nullptr) {
        layoutOutput(*source);
        wlr_output_schedule_frame(source->output->wlr());
      }
      return;
    }

    card->row = workspace->index();
    if (source == target) {
      layoutOutput(*target);
      wlr_output_schedule_frame(target->output->wlr());
      return;
    }
    if (source == nullptr) {
      rebuildCard(view);
      return;
    }

    const auto it = std::ranges::find_if(source->cards, [card](const std::unique_ptr<Card>& candidate) {
      return candidate.get() == card;
    });
    if (it == source->cards.end()) {
      rebuildCard(view);
      return;
    }

    std::unique_ptr<Card> moved = std::move(*it);
    source->cards.erase(it);
    moved->owner = target;
    wlr_scene_node_reparent(&moved->tree->node, target->tree);
    target->cards.push_back(std::move(moved));

    layoutOutput(*source);
    layoutOutput(*target);
    wlr_output_schedule_frame(source->output->wlr());
    wlr_output_schedule_frame(target->output->wlr());
  }

  void Overview::onWorkspaceActivated(WorkspaceGroup* group) {
    if (!m_active || m_closing || group == nullptr || group->active() == nullptr) {
      return;
    }
    OutputState* target = stateFor(group->output());
    if (target == nullptr) {
      return;
    }
    const auto row = static_cast<double>(group->active()->index());
    if (std::abs(target->rowTo - row) < 0.001 && m_anim.animating()) {
      return;
    }
    for (const auto& state : m_outputs) {
      state->rowFrom = state->rowScroll;
      state->rowTo = state->rowScroll;
    }
    target->rowTo = row;
    startAnimation(m_targetProgress, false);
  }

  void Overview::onWorkspaceArranged(Workspace* workspace) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateForWorkspace(workspace)) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onWorkspaceInventoryChanged(WorkspaceGroup* group) {
    if (!m_active || group == nullptr) {
      return;
    }
    OutputState* state = stateFor(group->output());
    if (state == nullptr) {
      return;
    }
    // Renumbering rows on one side of the active workspace must shift the filmstrip scroll by the same amount,
    // otherwise every surviving card visibly jumps even though its workspace identity did not move.
    if (Workspace* active = group->active()) {
      const double delta = static_cast<double>(active->index()) - state->rowTo;
      state->rowScroll += delta;
      state->rowFrom += delta;
      state->rowTo += delta;
    }
    syncWorkspaceRows(*state, *group);
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
  }

  void Overview::onFocusChanged() {
    if (m_active) {
      applyProgress();
    }
  }

  void Overview::onViewPresentationChanged(View* view) {
    if (m_active && view != nullptr && stateForWorkspace(view->workspace()) != nullptr) {
      m_cardPresentationDirty = true;
    }
  }

  void Overview::onOutputRemoved(Output* output) {
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
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
      m_dropWorkspaceGroup = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    if (m_pressCard != nullptr && m_pressCard->owner == it->get()) {
      m_pressCard = nullptr;
    }
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
    }
  }

  // -: hit testing

  Overview::Card* Overview::cardAt(double lx, double ly) {
    // Topmost first: later outputs and later cards paint over earlier ones.
    // Hit testing uses the same output and workspace-row clips as rendering.
    // The dragged card is reparented to the unclipped root and hits everywhere.
    for (const auto& state : std::views::reverse(m_outputs)) {
      RowMetrics metrics{};
      if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      for (const auto& card : std::views::reverse(state->cards)) {
        if (card->tree == nullptr || !card->tree->node.enabled) {
          continue;
        }
        wlr_box hit = card->box;
        if (card.get() != m_dragCard) {
          const bool clipToRow = card->view != nullptr
              && card->view->workspace() != nullptr
              && card->view->workspace()->scrollingVertical();
          if (clipToRow) {
            const wlr_box rowBox{
                .x = metrics.rowX,
                .y = rowTop(metrics, state->rowScroll, card->row),
                .width = metrics.rowW,
                .height = metrics.rowH,
            };
            wlr_box rowHit{};
            if (!wlr_box_intersection(&rowHit, &card->box, &rowBox)
                || !wlr_box_intersection(&hit, &rowHit, &metrics.outputBox)) {
              continue;
            }
          } else if (!wlr_box_intersection(&hit, &card->box, &metrics.outputBox)) {
            continue;
          }
        }
        if (boxContains(hit, lx, ly)) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Workspace* Overview::rowAt(double lx, double ly, OutputState** outState, size_t* outRow, bool extendHorizontal) {
    for (const auto& state : m_outputs) {
      RowMetrics metrics{};
      if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      for (size_t row = 0; row < state->workspaceBackgrounds.size(); ++row) {
        Workspace* workspace = group->workspaceAt(row);
        if (workspace == nullptr) {
          continue;
        }
        // Horizontal cards intentionally form one output-wide filmstrip even
        // when they overhang the centered workspace preview. Drag targeting
        // must cover that same visible area or the extreme gaps become dead
        // zones. Background clicks retain the narrower preview hitbox.
        const bool fullWidth =
            extendHorizontal && workspace->scrollingLayout() != nullptr && !workspace->scrollingVertical();
        const wlr_box box{
            fullWidth ? metrics.outputBox.x : metrics.rowX,
            rowTop(metrics, state->rowScroll, row),
            fullWidth ? metrics.outputBox.width : metrics.rowW,
            metrics.rowH,
        };
        if (!boxContains(box, lx, ly)) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outRow != nullptr) {
          *outRow = row;
        }
        return workspace;
      }
    }
    return nullptr;
  }

  WorkspaceGroup*
  Overview::workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox) {
    for (const auto& state : m_outputs) {
      RowMetrics metrics{};
      if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !group->dynamic()) {
        continue;
      }
      const size_t rowCount = std::min(group->workspaceCount(), state->workspaceBackgrounds.size());
      for (size_t index = 0; index < rowCount; ++index) {
        // Index zero has no preceding preview. Its insertion gap begins at the output edge, so dynamic workspaces can
        // also be inserted before the first preview.
        const int upperBottom =
            index == 0 ? metrics.outputBox.y : rowTop(metrics, state->rowScroll, index - 1) + metrics.rowH;
        const int lowerTop = rowTop(metrics, state->rowScroll, index);
        if (lowerTop <= upperBottom) {
          continue;
        }
        const wlr_box gap{metrics.rowX, upperBottom, metrics.rowW, lowerTop - upperBottom};
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
          const int height = std::clamp(visible.height / 3, 4, 18);
          *outHintBox = {
              .x = visible.x,
              .y = visible.y + (visible.height - height) / 2,
              .width = visible.width,
              .height = height,
          };
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

  // -: input

  bool Overview::handleButton(uint32_t button, bool pressed, double lx, double ly) {
    if (!interactive()) {
      return true; // Swallow everything while zooming back in.
    }

    if (!pressed) {
      if (button != BTN_LEFT) {
        return true;
      }
      if (m_dragCard != nullptr) {
        endDrag(true);
        return true;
      }
      Card* card = m_pressCard;
      Workspace* workspace = m_pressWorkspace;
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        closeToWorkspace(card->view->workspace(), card->view);
      } else if (workspace != nullptr) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    }

    Card* card = cardAt(lx, ly);
    if (button == BTN_MIDDLE) {
      if (card != nullptr && card->view != nullptr) {
        wlr_xdg_toplevel_send_close(card->view->toplevel());
      }
      return true;
    }
    if (button != BTN_LEFT) {
      return true;
    }

    m_pressCard = card;
    m_pressWorkspace = nullptr;
    m_pressX = lx;
    m_pressY = ly;
    if (card == nullptr) {
      m_pressWorkspace = rowAt(lx, ly, nullptr, nullptr, false);
    }
    return true;
  }

  void Overview::handleMotion(double lx, double ly) {
    if (!interactive()) {
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
    // select() lands in onWorkspaceActivated, which animates rowScroll onto the
    // new row, so the filmstrip follows without the caller arranging anything.
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    if (!interactive() || !vertical) {
      return true;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), lx, ly);
    selectRelativeWorkspace(direction < 0 ? -1 : 1, m_server->outputFromWlr(wlrOutput));
    return true;
  }

  bool Overview::focusAdjacent(int direction) {
    if (!interactive()) {
      return false;
    }
    Workspace* workspace = preferredWorkspace();
    if (workspace == nullptr) {
      return false;
    }
    View* target = workspace->focusAdjacent(direction);
    if (target == nullptr) {
      return false;
    }
    m_server->focusView(target, FocusReason::Directional);
    return true;
  }

  bool Overview::handleFallbackKey(uint32_t keysym) {
    if (!interactive()) {
      return false;
    }
    switch (keysym) {
    case XKB_KEY_Escape:
      close();
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      if (Workspace* workspace = preferredWorkspace()) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    case XKB_KEY_Left:
    case XKB_KEY_Right:
      focusAdjacent(keysym == XKB_KEY_Left ? -1 : 1);
      return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
      selectRelativeWorkspace(keysym == XKB_KEY_Up ? -1 : 1, nullptr);
      return true;
    default:
      return false;
    }
  }

  // -: drag

  void Overview::beginDrag() {
    Card* card = m_pressCard;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      return;
    }
    View* view = card->view;
    m_dragCard = card;
    m_dragOffsetX = m_pressX - card->box.x;
    m_dragOffsetY = m_pressY - card->box.y;
    m_dragSourceWorkspace = view->workspace();
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      m_dragSourceWidth = captureDropColumnWidth(*m_dragSourceWorkspace, view);
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->tree->node, m_tree);
    wlr_scene_node_raise_to_top(&card->tree->node);
    RowMetrics metrics{};
    if (card->owner != nullptr && rowMetrics(*card->owner, *m_server, zoom(), metrics)) {
      layoutCard(*card, metrics, card->owner->rowScroll);
    }
    m_server->cursor()->overrideCursor("grabbing");
  }

  void Overview::updateDrag(double lx, double ly) {
    Card* card = m_dragCard;
    card->box.x = static_cast<int>(std::lround(lx - m_dragOffsetX));
    card->box.y = static_cast<int>(std::lround(ly - m_dragOffsetY));
    wlr_scene_node_set_position(&card->tree->node, card->box.x, card->box.y);

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

    size_t row = 0;
    Workspace* workspace = rowAt(lx, ly, &state, &row, true);
    m_drop = {.workspace = workspace};
    if (workspace == nullptr || state == nullptr) {
      hideDropHint();
      scheduleFrames();
      return;
    }

    RowMetrics metrics{};
    if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
      hideDropHint();
      return;
    }
    // Map the pointer out of the thumbnail and back into workspace world space.
    const double worldX = metrics.outputBox.x + (lx - metrics.rowX) / metrics.zoom;
    const double worldY = metrics.outputBox.y + (ly - rowTop(metrics, state->rowScroll, row)) / metrics.zoom;

    if (card->view->tiled()) {
      // The overview applies its own projection after target selection. Keep
      // horizontal hints attached to content that is outside the normal
      // viewport but visible in the overview margin. Other layouts retain
      // their normal usable-area bounds.
      const bool horizontalScrolling = workspace->scrollingLayout() != nullptr && !workspace->scrollingVertical();
      m_drop = computeDropTarget(
          *workspace, worldX, worldY, card->view,
          DropTargetOptions{
              .clipHintToUsable = !horizontalScrolling,
              .reserveScrollingViewportEdges = false,
              .endpointGapsOutsideColumns = horizontalScrolling,
          }
      );
    } else {
      m_drop = {
          .workspace = workspace,
          .column = static_cast<int>(workspace->layout().columns().size()),
      };
    }
    if (m_drop.hintBox.width > 0 && m_drop.hintBox.height > 0) {
      showDropHint(m_drop.hintBox, metrics, state->rowScroll, row, state->output);
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
    View* view = card->view;
    WorkspaceGroup* insertionGroup = drop ? m_dropWorkspaceGroup : nullptr;
    Workspace* target = drop ? m_drop.workspace : nullptr;
    DropTarget targetDrop = m_drop;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;

    m_dragCard = nullptr;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    hideDropHint();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    bool insertedWorkspace = false;
    if (insertionGroup != nullptr) {
      target = insertionGroup->insertDynamicWorkspace(m_dropWorkspaceIndex);
      dropState = target != nullptr ? stateForWorkspace(target) : nullptr;
      if (target != nullptr && dropState != nullptr) {
        targetDrop = {
            .workspace = target,
            .column = 0,
        };
        insertedWorkspace = true;
      }
    }

    if (target != nullptr && view->tiled()) {
      applyDrop(
          *m_server, *view, *target, targetDrop, m_dragSourceWidth.has_value() ? &*m_dragSourceWidth : nullptr,
          /*animate=*/false
      );
    } else if (target != nullptr && dropState != nullptr && insertedWorkspace) {
      view->rememberFloatingPosition();
      if (view->workspace() != target) {
        view->moveToWorkspace(target, /*attachToLayout=*/false);
      }
      view->restoreFloatingPosition();
      m_server->focusView(view, FocusReason::DragDrop);
    } else if (target != nullptr && dropState != nullptr) {
      // Floating: map the card origin back out of the thumbnail.
      RowMetrics metrics{};
      size_t targetRow = target->index();
      if (rowMetrics(*dropState, *m_server, zoom(), metrics)) {
        const int x = metrics.outputBox.x + static_cast<int>(std::lround((cardBox.x - metrics.rowX) / metrics.zoom));
        const int y = metrics.outputBox.y
            + static_cast<int>(
                          std::lround((cardBox.y - rowTop(metrics, dropState->rowScroll, targetRow)) / metrics.zoom)
            );
        if (view->workspace() != target) {
          view->moveToWorkspace(target, /*attachToLayout=*/false);
        }
        view->setPosition(x, y);
      }
    } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
      // Cancelled or dropped on nothing: put the tile back where it came from.
      if (m_dragSourceWidth.has_value()) {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
        const int column = m_dragSourceWorkspace->layout().columnOf(view);
        m_dragSourceWorkspace->layout().setWidthFraction(column, m_dragSourceWidth->fraction);
        if (m_dragSourceWidth->fullWidth) {
          m_dragSourceWorkspace->layout().toggleFullWidth(column);
        }
        wlr_xdg_toplevel_set_maximized(view->toplevel(), m_dragSourceWidth->fullWidth);
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

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::syncWorkspaceRows(OutputState& state, WorkspaceGroup& group) {
    while (state.workspaceBackgrounds.size() > group.workspaceCount()) {
      wlr_scene_rect* background = state.workspaceBackgrounds.back();
      state.workspaceBackgrounds.pop_back();
      if (background != nullptr) {
        wlr_scene_node_destroy(&background->node);
      }
    }
    while (state.workspaceBackgrounds.size() < group.workspaceCount()) {
      const std::array<float, 4> color = tint(config().overview.workspaceBackground, m_progress);
      wlr_scene_rect* background = wlr_scene_rect_create(state.tree, 1, 1, color.data());
      if (background != nullptr) {
        // Backgrounds stay below every card even when created after the
        // overview scene was populated.
        wlr_scene_node_place_above(&background->node, &state.backgroundTint->node);
      }
      state.workspaceBackgrounds.push_back(background);
    }
    for (const auto& card : state.cards) {
      if (card->view != nullptr && card->view->workspace() != nullptr) {
        card->row = card->view->workspace()->index();
      }
    }
  }

  void Overview::showDropHint(
      const wlr_box& worldBox, const RowMetrics& metrics, double rowScroll, size_t row, Output* output
  ) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    const double z = metrics.zoom;
    const wlr_box mappedBox{
        .x = metrics.rowX + static_cast<int>(std::lround((worldBox.x - metrics.outputBox.x) * z)),
        .y = rowTop(metrics, rowScroll, row) + static_cast<int>(std::lround((worldBox.y - metrics.outputBox.y) * z)),
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
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
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
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      m_dropHint->hide();
    }
  }

} // namespace umbriel
