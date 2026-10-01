#include "scene/workspace_sources.h"

#include "output/output.h"
#include "scene/effect_registry.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

extern "C" {
#include "../../umbrielfx/internal/render/fx_renderer/scene_program.h"
#include "../../umbrielfx/internal/render/fx_renderer/scene_resources.h"
#include "../../umbrielfx/internal/types/scene_source.h"
#include "../../umbrielfx/internal/types/wlr_scene.h"
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace umbriel {
  fx_scene_resource_pool& presentationAggregatePool() {
    static fx_scene_resource_pool pool{.limit = FX_SCENE_TOTAL_BUDGET, .used = 0};
    return pool;
  }

  bool Output::registerWorkspaceSources(WorkspaceSources* sources) {
    if (m_activeWorkspaceSources != nullptr && m_activeWorkspaceSources != sources) {
      return false;
    }
    m_activeWorkspaceSources = sources;
    return true;
  }
  void Output::unregisterWorkspaceSources(WorkspaceSources* sources) {
    if (m_activeWorkspaceSources == sources) {
      m_activeWorkspaceSources = nullptr;
    }
  }
  void Output::notePresentationSourceContent() {
    if (m_activeWorkspaceSources) {
      m_activeWorkspaceSources->contentChanged();
    }
  }
  void Output::notePresentationViewMapped(View& view) {
    if (m_activeWorkspaceSources) {
      m_activeWorkspaceSources->viewMapped(view);
    }
  }
  void Output::notePresentationViewUnmapping(View& view) {
    if (m_activeWorkspaceSources) {
      m_activeWorkspaceSources->viewWillUnmap(view);
    }
  }
  struct WorkspaceSources::State {
    struct History {
      std::string identity;
      fx_scene_reservation reservation{};
      fx_scene_source_session* session = nullptr;
      bool pending = false;
      void finish(bool submitted) {
        if (pending) {
          fx_scene_source_session_finish_frame_for_test(session, submitted);
          pending = false;
        }
      }
      ~History() {
        finish(false);
        fx_scene_source_session_destroy_for_test(session);
        fx_scene_release(&reservation);
      }
    };
    struct Scratch {
      std::unique_ptr<fx_scene_scratch, decltype(&fx_scene_source_scratch_destroy)> buffers{
          nullptr, fx_scene_source_scratch_destroy
      };
      fx_scene_reservation reservation{};
      unsigned signature = 0;
      ~Scratch() {
        buffers.reset();
        fx_scene_release(&reservation);
      }
    };
    struct Sources {
      fx_scene_reservation reservation{};
      std::vector<fx_scene_source_pair_for_test> pairs;
      ~Sources() {

        for (auto& pair : pairs) {
          fx_scene_source_pair_finish_for_test(&pair);
        }
        fx_scene_release(&reservation);
      }
    };
    struct SurfaceWatch {
      State* owner;
      wlr_surface* surface;
      wl_listener commit{};
      wl_listener destroy{};
      SurfaceWatch(State& state, wlr_surface* watched) : owner(&state), surface(watched) {
        commit.notify = [](wl_listener* listener, void*) {
          SurfaceWatch* watch;
          watch = wl_container_of(listener, watch, commit);
          ++watch->owner->revision;
          wlr_output_schedule_frame(watch->owner->output.wlr());
        };
        destroy.notify = [](wl_listener* listener, void*) {
          SurfaceWatch* watch;
          watch = wl_container_of(listener, watch, destroy);
          wl_list_remove(&watch->commit.link);
          wl_list_init(&watch->commit.link);
          wl_list_remove(&watch->destroy.link);
          wl_list_init(&watch->destroy.link);
          watch->surface = nullptr;
          ++watch->owner->revision;
          wlr_output_schedule_frame(watch->owner->output.wlr());
        };
        wl_signal_add(&surface->events.commit, &commit);
        wl_signal_add(&surface->events.destroy, &destroy);
      }
      ~SurfaceWatch() {
        wl_list_remove(&commit.link);
        wl_list_remove(&destroy.link);
      }
    };
    struct Descriptor {
      std::vector<fx_scene_source_root_override> roots;
      fx_scene_source_view view{};
      fx_scene_source_view_plan plan{};
      std::unique_ptr<fx_scene_capture_plan, decltype(&fx_scene_source_plan_destroy)> prepared{
          nullptr, fx_scene_source_plan_destroy
      };
      History* history = nullptr;
    };
    Server& server;
    Output& output;
    fx_scene_resource_pool pool{.limit = FX_SCENE_OUTPUT_BUDGET, .used = 0};
    bool opened = false;
    std::vector<View*> owners;
    std::vector<std::unique_ptr<SurfaceWatch>> watches;
    std::unique_ptr<Sources> current;
    std::unique_ptr<Sources> spare;
    std::unique_ptr<Scratch> scratch;
    std::unique_ptr<Sources> candidate;
    std::unique_ptr<Sources> frozen;
    // Histories survive image refreshes until the native transition ends.
    std::vector<std::unique_ptr<History>> histories;
    WorkspaceSourceFace frozenMetadata;
    wlr_box box{};
    std::vector<std::string> visible;
    std::vector<std::string> captureIdentities;
    std::vector<WorkspaceSourceFace> metadata;
    uint64_t revision = 1;
    uint64_t capturedRevision = 0;
    uint64_t committedRevision = 0;
    bool pending = false;
    bool callbacksPending = false;
    bool renderLocked = false;
    PresentationFallback fallback = PresentationFallback::None;

    void registerOccurrences() {
      std::vector<View*> contributing;
      for (View* view : owners) {
        if (view->pinned()
            || (view->workspace() != nullptr && std::ranges::contains(visible, view->workspace()->id()))) {
          contributing.push_back(view);
        }
      }
      server.effects().setSourceOccurrences(this, &output, contributing, true);
    }
    void watchSurfaces() {
      // Re-enumeration discovers new subsurfaces/popups after a parent commit.
      std::unordered_set<wlr_surface*> seen;
      struct Iteration {
        State& state;
        std::unordered_set<wlr_surface*>& seen;
      } iteration{*this, seen};
      for (View* view : owners) {
        if (frozen
            && !view->pinned()
            && (view->workspace() == nullptr || !std::ranges::contains(visible, view->workspace()->id()))) {
          continue;
        }
        view->forEachSurface(
            [](wlr_surface* surface, int, int, void* data) {
              auto& iteration = *static_cast<Iteration*>(data);
              if (!iteration.seen.insert(surface).second) {
                return;
              }
              if (std::ranges::none_of(iteration.state.watches, [surface](const auto& watch) {
                    return watch->surface == surface;
                  })) {
                iteration.state.watches.push_back(std::make_unique<SurfaceWatch>(iteration.state, surface));
              }
            },
            &iteration
        );
      }
      for (uint32_t layer = 0; layer < 3; ++layer) {
        wlr_scene_node_for_each_buffer(
            &server.shellLayerTree(layer)->node,
            [](wlr_scene_buffer* buffer, int, int, void* data) {
              auto& iteration = *static_cast<Iteration*>(data);
              auto* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
              if (sceneSurface == nullptr || !iteration.seen.insert(sceneSurface->surface).second) {
                return;
              }
              if (std::ranges::none_of(iteration.state.watches, [&](const auto& watch) {
                    return watch->surface == sceneSurface->surface;
                  })) {
                iteration.state.watches.push_back(
                    std::make_unique<SurfaceWatch>(iteration.state, sceneSurface->surface)
                );
              }
            },
            &iteration
        );
      }
      std::erase_if(watches, [&](const auto& watch) { return !seen.contains(watch->surface); });
    }
    size_t indexOf(std::string_view id) const {
      auto& group = *output.workspaceGroup();
      for (size_t i = 0; i < group.workspaceCount(); ++i) {
        if (group.workspaceAt(i)->id() == id)
          return i;
      }
      return group.workspaceCount();
    }
    Descriptor describe(size_t index, float scale) {
      Descriptor result;
      auto& group = *output.workspaceGroup();
      auto* workspace = group.workspaceAt(index);
      auto* root = &server.scene()->tree;
      wlr_scene_node* first;
      first = wl_container_of(root->children.next, first, link);
      result.view = {
          .first = first,
          .last = &server.pinnedTree()->node,
          .plan = nullptr,
          .scratch = nullptr,
          .roots = nullptr,
          .root_count = 0,
          .extent = box,
          .scale = scale,
          .session = nullptr
      };
      const auto add = [&](wlr_scene_node* node, fx_scene_source_visibility visibility) {
        result.roots.push_back({.root = node, .visibility = visibility, .offset_x = 0, .offset_y = 0});
      };
      // Preserve shared desktop layers in native output coordinates.
      add(first, FX_SCENE_SOURCE_INHERIT);
      for (uint32_t layer = 0; layer < 3; ++layer) {
        auto* node = &server.shellLayerTree(layer)->node;
        if (node != first) {
          add(node, FX_SCENE_SOURCE_INHERIT);
        }
      }
      add(&server.pinnedTree()->node, FX_SCENE_SOURCE_INHERIT);
      for (const auto& other : server.outputs()) {
        if (other.get() != &output) {
          add(&other->viewRoot()->node, FX_SCENE_SOURCE_HIDDEN);
          add(&other->fullscreenRoot()->node, FX_SCENE_SOURCE_HIDDEN);
          add(&other->pinnedRoot()->node, FX_SCENE_SOURCE_HIDDEN);
        }
      }
      add(&output.viewRoot()->node, FX_SCENE_SOURCE_VISIBLE);
      add(&output.fullscreenRoot()->node, FX_SCENE_SOURCE_VISIBLE);
      for (size_t i = 0; i < group.workspaceCount(); ++i) {
        auto* item = group.workspaceAt(i);
        auto* itemTree = item->tileShadowLayer()->node.parent;
        const auto visibility = item == workspace ? FX_SCENE_SOURCE_VISIBLE : FX_SCENE_SOURCE_HIDDEN;
        add(&itemTree->node, visibility);
        result.roots.back().offset_x = -itemTree->node.x;
        result.roots.back().offset_y = -itemTree->node.y;
        add(&item->fullscreenTree()->node, visibility);
        result.roots.back().offset_x = -item->fullscreenTree()->node.x;
        result.roots.back().offset_y = -item->fullscreenTree()->node.y;
      }
      for (View* view : owners) {
        if (!view->pinned()) {
          add(&view->sceneTree()->node,
              view->workspace() == workspace ? FX_SCENE_SOURCE_VISIBLE : FX_SCENE_SOURCE_HIDDEN);
        }
      }
      return result;
    }
    bool refresh() {
      watchSurfaces();
      std::vector<Descriptor> descriptors;
      uint64_t retained = 0;
      uint64_t peak = 0;
      uint64_t newHistoryBytes = 0;
      // Capture both faces at native resolution, charging retained images throughout.

      const size_t sourceCount = captureIdentities.size();
      descriptors.reserve(sourceCount);
      bool valid = true;
      for (size_t i = 0; i < sourceCount; ++i) {
        if (i == 0 && frozen) {
          descriptors.emplace_back();
          continue;
        }
        const auto& identity = captureIdentities[i];
        const auto sourceIndex = indexOf(identity);
        if (sourceIndex == output.workspaceGroup()->workspaceCount())
          return false;
        auto& descriptor = descriptors.emplace_back(describe(sourceIndex, output.wlr()->scale));
        descriptor.view.roots = descriptor.roots.data();
        descriptor.view.root_count = descriptor.roots.size();
        descriptor.prepared.reset(fx_scene_source_prepare(output.sceneOutput(), &descriptor.view, &descriptor.plan));
        descriptor.view.plan = descriptor.prepared.get();
        if (!descriptor.prepared || descriptor.plan.retained_bytes > std::numeric_limits<uint64_t>::max() - retained) {
          valid = false;
          break;
        }
        retained += descriptor.plan.retained_bytes;

        // The destination reuses one scratch set across both back buffers.
        if (descriptor.plan.scratch_bytes == 0) {
          scratch.reset();
        } else if (
            !scratch
            || scratch->signature != descriptor.plan.scratch_signature
            || scratch->reservation.bytes != descriptor.plan.scratch_bytes
        ) {
          scratch.reset();
          auto nextScratch = std::make_unique<Scratch>();
          if (!fx_scene_reserve(
                  &nextScratch->reservation, &pool, &presentationAggregatePool(), descriptor.plan.scratch_bytes
              )) {
            fallback = PresentationFallback::ResourceBudget;
            return false;
          }
          nextScratch->buffers.reset(fx_scene_source_scratch_create(output.sceneOutput()));
          if (!nextScratch->buffers)
            return false;
          nextScratch->signature = descriptor.plan.scratch_signature;
          scratch = std::move(nextScratch);
        }
        descriptor.view.scratch = scratch ? scratch->buffers.get() : nullptr;
        peak = std::max(peak, descriptor.plan.capture_bytes - descriptor.plan.scratch_bytes);
        auto existing =
            std::ranges::find_if(histories, [&](const auto& history) { return history->identity == identity; });
        if (existing != histories.end()
            && (descriptor.plan.history_bytes == 0
                || !fx_scene_source_view_session_matches(
                    output.sceneOutput(), &descriptor.view, (*existing)->session
                ))) {
          histories.erase(existing);
          existing = histories.end();
        }
        if (existing != histories.end()) {
          descriptor.history = existing->get();
          descriptor.view.session = descriptor.history->session;
        } else {
          if (descriptor.plan.history_bytes > std::numeric_limits<uint64_t>::max() - newHistoryBytes) {
            valid = false;
            break;
          }
          newHistoryBytes += descriptor.plan.history_bytes;
        }
      }
      if (!valid) {
        return false;
      }
      if (newHistoryBytes > std::numeric_limits<uint64_t>::max() - peak
          || retained > std::numeric_limits<uint64_t>::max() - peak - newHistoryBytes) {
        return false;
      }
      auto next = spare ? std::move(spare) : std::make_unique<Sources>();
      fx_scene_release(&next->reservation);
      const uint64_t imageBytes = retained + peak;
      if (!fx_scene_reserve(&next->reservation, &pool, &presentationAggregatePool(), imageBytes + newHistoryBytes)) {
        fallback = PresentationFallback::ResourceBudget;
        return false;
      }
      // Full image + history admission precedes every allocation. Split the
      // reservation in this single-threaded pool so histories can survive
      // releasing transient capture images and the old displayed revision.
      fx_scene_release(&next->reservation);
      if (!fx_scene_reserve(&next->reservation, &pool, &presentationAggregatePool(), imageBytes)) {
        return false;
      }
      for (size_t i = 0; i < descriptors.size(); ++i) {
        auto& descriptor = descriptors[i];
        if (descriptor.plan.history_bytes == 0 || descriptor.history != nullptr) {
          continue;
        }
        auto history = std::make_unique<History>();
        history->identity = captureIdentities[i];
        if (!fx_scene_reserve(
                &history->reservation, &pool, &presentationAggregatePool(), descriptor.plan.history_bytes
            )) {
          return false;
        }
        history->session =
            fx_scene_source_view_session_create(output.sceneOutput(), &descriptor.view, descriptor.plan.history_bytes);
        if (history->session == nullptr) {
          return false;
        }
        descriptor.history = history.get();
        descriptor.view.session = history->session;
        histories.push_back(std::move(history));
      }
      next->pairs.resize(descriptors.size());

      server.effects().bindSourceTime(this);
      for (size_t i = 0; i < descriptors.size(); ++i) {
        if (i == 0 && frozen) {
          continue;
        }
        if (auto* history = descriptors[i].history) {
          if (!fx_scene_source_session_begin_frame_for_test(history->session)) {
            return false;
          }
          history->pending = true;
        }
        if (!fx_scene_source_view_pair_capture_for_test(
                output.sceneOutput(), &descriptors[i].view, descriptors[i].plan.total_bytes, &next->pairs[i]
            )) {
          return false;
        }
      }

      // Release transient capture overhead while retaining the source images.
      fx_scene_release(&next->reservation);
      const bool reserved = fx_scene_reserve(&next->reservation, &pool, &presentationAggregatePool(), retained);
      if (!reserved) {
        return false; // Shrinking cannot fail in this single-threaded reservation pool.
      }
      candidate = std::move(next);
      capturedRevision = revision;
      metadata.clear();
      metadata.reserve(candidate->pairs.size());
      for (size_t faceIndex = 0; faceIndex < candidate->pairs.size(); ++faceIndex) {
        const auto& facePair = candidate->pairs[faceIndex];
        auto& face = metadata.emplace_back();
        if (faceIndex == 0 && frozen) {
          face = frozenMetadata;
          continue;
        }
        face.display = facePair.display;
        face.unfiltered = facePair.unfiltered;
      }

      pending = false;
      return true;
    }
  };

  WorkspaceSources::WorkspaceSources(Server& server, Output& output)
      : m_state(std::make_unique<State>(server, output)) {}
  WorkspaceSources::~WorkspaceSources() { cancel(PresentationFallback::OutputRemoved); }
  bool WorkspaceSources::open(std::string_view from, std::string_view to) {
    auto& state = *m_state;
    if (state.opened) {
      return false;
    }
    if (!state.output.registerWorkspaceSources(this)) {
      return false;
    }
    state.opened = true;
    state.captureIdentities = {std::string(from), std::string(to)};
    state.visible = state.captureIdentities;
    state.box = state.output.layoutBox();
    state.fallback = PresentationFallback::None;
    state.pending = true;
    ++state.revision;
    for (const auto& view : state.server.views()) {
      if (view->mapped()
          && view->animatesOn(&state.output)
          && (view->pinned()
              || (view->workspace() && std::ranges::contains(state.captureIdentities, view->workspace()->id())))) {
        state.owners.push_back(view.get());
      }
    }
    state.watchSurfaces();
    state.registerOccurrences();
    wlr_output_lock_attach_render(state.output.wlr(), true);
    state.renderLocked = true;
    wlr_output_schedule_frame(state.output.wlr());
    return true;
  }

  bool WorkspaceSources::beginPair(std::string_view from, std::string_view to) {
    if (from == to || !open(from, to)) {
      return false;
    }
    auto& state = *m_state;
    if (state.indexOf(to) == state.output.workspaceGroup()->workspaceCount()) {
      cancel(PresentationFallback::SourceUnavailable);
      return false;
    }
    return true;
  }

  bool WorkspaceSources::freezeOutgoing() {
    auto& state = *m_state;
    if (state.frozen || !state.opened) {
      return false;
    }
    const auto& identity = state.captureIdentities.front();
    const size_t index = state.indexOf(identity);
    auto descriptor = state.describe(index, state.output.wlr()->scale);
    descriptor.view.roots = descriptor.roots.data();
    descriptor.view.root_count = descriptor.roots.size();
    if (!fx_scene_source_view_plan_for_test(state.output.sceneOutput(), &descriptor.view, &descriptor.plan)) {
      return false;
    }
    // The outgoing endpoint is already the authoritative native workspace.
    // Replay its committed feedback provenance; a fresh virtual history would
    // visibly restart it at progress zero before the workspace even moves.
    const bool replayHistory = descriptor.plan.history_bytes != 0;
    const uint64_t captureBytes = replayHistory
        ? fx_scene_source_frozen_pair_bytes(state.output.sceneOutput(), &descriptor.view)
        : descriptor.plan.total_bytes;
    if (captureBytes == 0) {
      state.fallback = PresentationFallback::ResourceBudget;
      return false;
    }
    auto frozen = std::make_unique<State::Sources>();
    if (!fx_scene_reserve(&frozen->reservation, &state.pool, &presentationAggregatePool(), captureBytes)) {
      state.fallback = PresentationFallback::ResourceBudget;
      return false;
    }
    frozen->pairs.resize(1);
    state.server.effects().bindSourceTime(&state);
    const bool captured = replayHistory
        ? fx_scene_source_pair_capture_for_test(
              state.output.sceneOutput(), &descriptor.view, captureBytes, &frozen->pairs.front()
          )
        : fx_scene_source_view_pair_capture_for_test(
              state.output.sceneOutput(), &descriptor.view, captureBytes, &frozen->pairs.front()
          );
    if (!captured) {
      return false;
    }
    const auto& pair = frozen->pairs.front();
    const uint64_t retainedBytes = replayHistory ? static_cast<uint64_t>(pair.display->width)
                * pair.display->height
                * (pair.floating_point ? 8 : 4)
                * (pair.display == pair.unfiltered ? 1 : 2)
            + 4096
                                                 : descriptor.plan.retained_bytes;
    fx_scene_release(&frozen->reservation);
    if (!fx_scene_reserve(&frozen->reservation, &state.pool, &presentationAggregatePool(), retainedBytes)) {
      return false;
    }
    state.frozenMetadata = {
        .display = pair.display,
        .unfiltered = pair.unfiltered,
    };
    state.frozen = std::move(frozen);
    state.visible = {state.captureIdentities.back()};
    ++state.revision;
    state.registerOccurrences();
    return true;
  }

  WorkspaceSourceResult WorkspaceSources::prepareFrame(bool animate) {
    tick(animate);
    if (!m_state->opened) {
      return WorkspaceSourceResult::Failed;
    }
    return m_state->pending ? WorkspaceSourceResult::Preparing : WorkspaceSourceResult::Ready;
  }
  std::span<const WorkspaceSourceFace> WorkspaceSources::faces() const { return m_state->metadata; }

  void WorkspaceSources::frameSubmitted(bool success) {
    if (success) {
      frameCommitted();
    }
  }
  fx_scene_resource_pool& WorkspaceSources::resourcePool() { return m_state->pool; }
  uint64_t WorkspaceSources::reservedBytes() const { return m_state->pool.used; }
  uint64_t WorkspaceSources::revision() const { return m_state->capturedRevision; }
  PresentationFallback WorkspaceSources::lastFallback() const { return m_state->fallback; }
  void WorkspaceSources::cancel(PresentationFallback reason) {
    auto& state = *m_state;
    if (!state.opened) {
      return;
    }
    state.fallback = reason;

    state.pending = false;
    state.visible.clear();
    state.captureIdentities.clear();
    state.metadata.clear();
    state.server.effects().clearSourceOccurrences(&state);

    state.watches.clear();

    state.owners.clear();
    state.candidate.reset();
    state.current.reset();
    state.spare.reset();
    state.scratch.reset();
    state.frozen.reset();
    state.histories.clear();
    state.frozenMetadata = {};
    state.opened = false;
    state.callbacksPending = false;
    if (state.renderLocked) {
      wlr_output_lock_attach_render(state.output.wlr(), false);
      state.renderLocked = false;
    }
    state.output.unregisterWorkspaceSources(this);
    wlr_output_schedule_frame(state.output.wlr());
  }
  void WorkspaceSources::viewMapped(View& view) {
    auto& state = *m_state;
    if (!state.opened
        || !view.mapped()
        || !view.animatesOn(&state.output)
        || (!view.pinned()
            && (!view.workspace() || !std::ranges::contains(state.captureIdentities, view.workspace()->id())))
        || std::ranges::contains(state.owners, &view)) {
      return;
    }
    state.owners.push_back(&view);
    state.watchSurfaces();
    state.registerOccurrences();
    contentChanged();
  }
  void WorkspaceSources::viewWillUnmap(View& view) {
    auto& state = *m_state;
    if (!std::ranges::contains(state.owners, &view)) {
      return;
    }
    std::erase(state.owners, &view);
    // Drop every listener before the native unmap can destroy a surface; the
    // retained paired images own their buffers independently of this View.
    state.watchSurfaces();
    state.registerOccurrences();
    contentChanged();
  }
  void WorkspaceSources::contentChanged() {
    if (m_state->opened) {
      ++m_state->revision;
      wlr_output_schedule_frame(m_state->output.wlr());
    }
  }
  void WorkspaceSources::tick(bool animate) {
    auto& state = *m_state;
    if (!state.opened) {
      return;
    }
    const auto box = state.output.layoutBox();
    if (state.server.sessionLocked()
        || box.x != state.box.x
        || box.y != state.box.y
        || box.width != state.box.width
        || box.height != state.box.height) {
      cancel(state.server.sessionLocked() ? PresentationFallback::Locked : PresentationFallback::TopologyChanged);
      return;
    }
    if (state.candidate) {
      return; // Retry precisely the captured revision until successful output submission.
    }
    for (const auto* view : state.owners) {
      if (!view->pinned()
          && (view->workspace() == nullptr || !std::ranges::contains(state.visible, view->workspace()->id()))) {
        continue;
      }
      if (const auto* toplevel = view->toplevel()) {
        const auto* surface = toplevel->base;
        if (surface->configure_idle != nullptr || surface->current.configure_serial != surface->scheduled_serial)
          return;
      }
    }

    if (animate) {
      ++state.revision;
    }
    if (state.pending || state.revision != state.committedRevision) {
      if (!state.refresh()) {
        cancel(
            state.fallback == PresentationFallback::ResourceBudget ? state.fallback
                                                                   : PresentationFallback::SourceUnavailable
        );
      }
    }
  }
  void WorkspaceSources::frameCommitted() {
    auto& state = *m_state;
    if (state.candidate) {
      for (auto& history : state.histories) {
        history->finish(true);
      }
      state.spare = std::move(state.current);
      state.current = std::move(state.candidate);
      state.committedRevision = state.capturedRevision;
      state.callbacksPending = true;
      if (state.revision != state.committedRevision) {
        wlr_output_schedule_frame(state.output.wlr());
      }
    }
  }
  void WorkspaceSources::sendFrameDone(const timespec& when) {
    auto& state = *m_state;
    if (!state.callbacksPending || !state.opened) {
      return;
    }
    state.callbacksPending = false;
    std::unordered_set<wlr_surface*> sent;
    struct Delivery {
      const timespec& when;
      std::unordered_set<wlr_surface*>& sent;
    } delivery{when, sent};
    for (View* view : state.owners) {
      if (!view->pinned() && !std::ranges::contains(state.visible, view->workspace()->id())) {
        continue; // Only contributors to the displayed face are paced.
      }
      view->forEachSurface(
          [](wlr_surface* surface, int, int, void* data) {
            auto& delivery = *static_cast<Delivery*>(data);
            if (delivery.sent.insert(surface).second && !wl_list_empty(&surface->current.frame_callback_list)) {
              wlr_surface_send_frame_done(surface, &delivery.when);
            }
          },
          &delivery
      );
    }
    // Shared captured layer bands have native membership, but their ordinary
    // callbacks are suppressed by replacement just like workspace windows.
    wlr_scene_frame_done_event event{.output = state.output.sceneOutput(), .when = when};
    for (uint32_t layer = 0; layer < 3; ++layer) {
      wlr_scene_node_for_each_buffer(
          &state.server.shellLayerTree(layer)->node,
          [](wlr_scene_buffer* buffer, int, int, void* data) {
            wlr_scene_buffer_send_frame_done(buffer, static_cast<wlr_scene_frame_done_event*>(data));
          },
          &event
      );
    }
  }

  bool WorkspaceSources::active() const { return m_state->opened && (m_state->candidate || m_state->current); }

  bool WorkspaceSources::renderLocked() const { return m_state->renderLocked; }

} // namespace umbriel
