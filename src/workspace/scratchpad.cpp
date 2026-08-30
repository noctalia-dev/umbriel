#include "workspace/scratchpad.h"

#include "config/resolve.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <ranges>
#include <sstream>
#include <utility>

namespace umbriel {

  namespace {
    void safeSetSuspended(View* view, bool suspended) {
      if (view == nullptr || !view->mapped() || view->toplevel() == nullptr) {
        return;
      }
      if (view->toplevel()->base == nullptr || !view->toplevel()->base->initialized) {
        return;
      }
      if (view->toplevel()->resource == nullptr) {
        return;
      }
      if (wl_resource_get_version(view->toplevel()->resource) < 6) {
        return;
      }
      wlr_xdg_toplevel_set_suspended(view->toplevel(), suspended);
    }

    // A slot\x27s on_empty command is only allowed to claim the window it actually
    // started, so a capture waits for a client whose process descends from it.
    constexpr int kCaptureTimeoutSec = 30;
    constexpr int kMaxAncestorWalk = 16;

    pid_t viewPid(const View* view) {
      const wlr_xdg_toplevel* toplevel = view != nullptr ? view->toplevel() : nullptr;
      if (toplevel == nullptr || toplevel->base == nullptr || toplevel->base->client == nullptr) {
        return -1;
      }
      wl_client* client = toplevel->base->client->client;
      if (client == nullptr) {
        return -1;
      }
      pid_t pid = -1;
      wl_client_get_credentials(client, &pid, nullptr, nullptr);
      return pid;
    }

    pid_t parentPid(pid_t pid) {
      const std::string path = "/proc/" + std::to_string(pid) + "/stat";
      std::ifstream stat(path);
      if (!stat.is_open()) {
        return -1;
      }
      std::string line;
      std::getline(stat, line);
      // comm is parenthesised and may itself contain spaces, so ppid is the
      // second field after the closing parenthesis.
      const size_t close = line.rfind(')');
      if (close == std::string::npos) {
        return -1;
      }
      std::istringstream rest(line.substr(close + 1));
      std::string state;
      pid_t ppid = -1;
      if (!(rest >> state >> ppid)) {
        return -1;
      }
      return ppid;
    }

    // Off-screen origin a window slides from (and back to), clear of the edge it
    // enters through.
    constexpr int kSlideClearancePx = 50;

    std::pair<int, int> slideOffset(std::string_view direction, const wlr_box& area, const wlr_box& target) {
      if (direction == "bottom") {
        return {target.x, area.y + area.height + kSlideClearancePx};
      }
      if (direction == "left") {
        return {area.x - target.width - kSlideClearancePx, target.y};
      }
      if (direction == "right") {
        return {area.x + area.width + kSlideClearancePx, target.y};
      }
      return {target.x, area.y - target.height - kSlideClearancePx};
    }

    bool isDescendantOf(pid_t pid, pid_t ancestor) {
      for (int depth = 0; depth < kMaxAncestorWalk && pid > 1; ++depth) {
        if (pid == ancestor) {
          return true;
        }
        pid = parentPid(pid);
        if (pid <= 0) {
          break;
        }
      }
      return false;
    }

    LayoutConstraints viewLayoutConstraints(const View* view) {
      const wlr_xdg_toplevel* toplevel = view != nullptr ? view->toplevel() : nullptr;
      const XdgSizeHints hints = xdgSizeHints(toplevel);
      return {
          .minWidth = hints.minWidth,
          .minHeight = hints.minHeight,
          .maxWidth = hints.maxWidth,
          .maxHeight = hints.maxHeight,
          .fullscreen = view != nullptr && view->layoutFullscreen(),
          .maximizedToEdges = view != nullptr && view->maximizedToEdges(),
      };
    }
  } // namespace

  ScratchpadManager::ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot)
      : m_server(&server), m_root(root), m_shadowRoot(shadowRoot) {
    m_server->registerAnimatable(this);
  }

  ScratchpadManager::~ScratchpadManager() {
    m_server->unregisterAnimatable(this);
    for (auto& [output, rect] : m_dimRects) {
      wlr_scene_node_destroy(&rect->node);
    }
    for (auto& [output, blur] : m_blurNodes) {
      wlr_scene_node_destroy(&blur->node);
    }
  }

  bool ScratchpadManager::tickAnimations(uint64_t nowMsec) {
    // Scratchpad views have no workspace, so the usual per-frame crop refresh (Workspace::syncViewPresentation)
    // never runs for them; drive it here instead while a popin zoom is animating.
    for (Entry& entry : m_entries) {
      if (entry.view != nullptr && entry.view->sizeAnimating()) {
        const wlr_box current{
            entry.view->sceneTree()->node.x,
            entry.view->sceneTree()->node.y,
            entry.view->presentation().width(),
            entry.view->presentation().height(),
        };
        entry.view->applyPresentation(current);
      }
    }

    bool movedBackdrop = false;
    for (auto& [output, fade] : m_backdropFades) {
      if (fade.tick(nowMsec)) {
        updateDimAndBlur(output);
        movedBackdrop = true;
      }
    }

    if (m_hidingViews.empty()) {
      return movedBackdrop;
    }
    const auto& spCfg = config().animation.scratchpad;
    std::erase_if(m_hidingViews, [this, &spCfg](View* view) {
      if (view == nullptr || !view->mapped()) {
        return true;
      }
      if (view->hasActiveAnimations()) {
        return false;
      }
      view->setFadeAlpha(0.0F);
      view->setNodeEnabled(false);
      std::string_view slotName;
      const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
      if (it != m_entries.end()) {
        slotName = it->name;
      }
      const bool suspend = effectiveConfig(slotName).suspendHidden.value_or(spCfg.suspendHidden);
      if (suspend) {
        safeSetSuspended(view, true);
      }
      return true;
    });
    return movedBackdrop || !m_hidingViews.empty();
  }

  bool ScratchpadManager::hasActiveAnimations() const {
    return !m_hidingViews.empty()
        || std::ranges::any_of(m_backdropFades, [](const auto& entry) { return entry.second.animating(); })
        || std::ranges::any_of(m_entries, [](const Entry& entry) {
             return entry.view != nullptr && entry.view->hasActiveAnimations();
           });
  }

  bool ScratchpadManager::animatesOn(const Output* output) const {
    const auto fade =
        std::ranges::find_if(m_backdropFades, [output](const auto& entry) { return entry.first == output; });
    if (fade != m_backdropFades.end() && fade->second.animating()) {
      return true;
    }
    return std::ranges::any_of(m_entries, [output](const Entry& entry) {
      return entry.output == output && entry.view != nullptr && entry.view->hasActiveAnimations();
    });
  }

  bool ScratchpadManager::contains(const View* view) const {
    return std::ranges::any_of(m_entries, [view](const Entry& entry) { return entry.view == view; });
  }

  // An open slot stays open while empty, so it can be toggled shut again and so
  // an on_empty command has somewhere to land.
  bool ScratchpadManager::isSlotVisible(Output* output, std::string_view name) const {
    return std::ranges::any_of(m_visibleSlots, [output, name](const VisibleSlot& slot) {
      return slot.output == output && slot.name == name;
    });
  }

  bool ScratchpadManager::isOutputVisible(Output* output) const {
    return std::ranges::any_of(m_visibleSlots, [output](const VisibleSlot& slot) { return slot.output == output; });
  }

  std::optional<std::string> ScratchpadManager::visibleSlotName(Output* output) const {
    const auto slot = std::ranges::find_if(m_visibleSlots, [output](const VisibleSlot& candidate) {
      return candidate.output == output;
    });
    if (slot == m_visibleSlots.end()) {
      return std::nullopt;
    }
    return slot->name;
  }

  // Returned by value because every caller hides slots as it iterates, which
  // erases the entries it would otherwise be walking.
  std::vector<std::string> ScratchpadManager::visibleSlotsOn(Output* output) const {
    std::vector<std::string> names;
    for (const VisibleSlot& slot : m_visibleSlots) {
      if (slot.output == output) {
        names.push_back(slot.name);
      }
    }
    return names;
  }

  std::vector<std::string> ScratchpadManager::otherVisibleSlots(Output* output, std::string_view keep) const {
    std::vector<std::string> names = visibleSlotsOn(output);
    std::erase(names, keep);
    return names;
  }

  bool ScratchpadManager::slotHasWindows(Output* output, std::string_view name) const {
    return std::ranges::any_of(m_entries, [output, name](const Entry& entry) {
      return entry.output == output && entry.name == name && entry.view != nullptr;
    });
  }

  bool ScratchpadManager::moveToScratchpad(View* view, Output* output, std::string_view name) {
    if (output == nullptr || m_server == nullptr || m_root == nullptr || m_shadowRoot == nullptr) {
      return false;
    }
    if (view == nullptr || !view->mapped() || contains(view)) {
      return false;
    }

    Entry entry{
        .view = view,
        .output = output,
        .name = std::string(name),
        .returnOutput = {},
        .displacedOutput = {},
        .displacedPosition = std::nullopt,
        .returnWorkspace = {},
        .returnTiled = view->tiled(),
    };
    Output* sourceOutput = nullptr;
    if (Workspace* previous = view->workspace()) {
      entry.returnWorkspace = previous->name();
      if (previous->group() != nullptr && previous->group()->output() != nullptr) {
        sourceOutput = previous->group()->output();
        entry.returnOutput = sourceOutput->wlr()->name;
      }
    }
    if (view->toplevel()->scheduled.fullscreen || view->toplevel()->current.fullscreen) {
      view->toggleFullscreen();
    }
    if (view->pinned()) {
      view->togglePinned();
    }
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false);
    }
    if (view->toplevel()->scheduled.maximized || view->toplevel()->current.maximized) {
      view->setMaximized(false);
    }
    view->setFloating(true);
    view->cancelPositionAnimation();
    view->cancelFadeAnimation();

    wlr_box targetArea = output->usableArea();
    if (targetArea.width <= 0 || targetArea.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &targetArea);
    }
    const auto slotCfg = effectiveConfig(name);
    const bool isScaled = slotCfg.scale.has_value() ? (*slotCfg.scale > 0.0 && *slotCfg.scale <= 1.0)
                                                    : (config().animation.scratchpad.scale > 0.0);
    if (slotCfg.fullscreen.value_or(false)) {
      if (!view->toplevel()->scheduled.fullscreen && !view->toplevel()->current.fullscreen) {
        view->toggleFullscreen();
      }
    } else if (slotCfg.maximizeToEdges.value_or(false)) {
      view->setMaximizedToEdges(true);
    } else if (slotCfg.maximize.value_or(false)) {
      const int gap = slotCfg.gap.value_or(config().layoutGap());
      if (gap >= 0 && targetArea.width > 2 * gap && targetArea.height > 2 * gap) {
        view->requestFloatingSize(targetArea.width - 2 * gap, targetArea.height - 2 * gap);
        view->setPosition(targetArea.x + gap, targetArea.y + gap);
      } else {
        view->requestFloatingSize(targetArea.width, targetArea.height);
        view->setPosition(targetArea.x, targetArea.y);
      }
    } else if (isScaled && targetArea.width > 0 && targetArea.height > 0) {
      const double scale = slotCfg.scale.has_value() ? *slotCfg.scale : config().animation.scratchpad.scale;
      const int targetW = std::max(100, static_cast<int>(std::lround(targetArea.width * scale)));
      const int targetH = std::max(100, static_cast<int>(std::lround(targetArea.height * scale)));
      wlr_xdg_toplevel_set_size(view->toplevel(), targetW, targetH);
      const int newX = targetArea.x + std::max(0, (targetArea.width - targetW) / 2);
      const int newY = targetArea.y + std::max(0, (targetArea.height - targetH) / 2);
      view->setPosition(newX, newY);
    } else {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      view->setPosition(x, y);
    }

    view->moveToWorkspace(nullptr);
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setScratchpadBorder(true);

    const bool wasVisible = isSlotVisible(output, name);
    m_entries.push_back(std::move(entry));

    if (wasVisible) {
      arrangeSlot(output, name, true);
    } else {
      view->setNodeEnabled(false);
      view->setOnActiveWorkspace(false);
      view->setFadeAlpha(0.0F);
      if (config().animation.scratchpad.suspendHidden) {
        safeSetSuspended(view, true);
      }
    }

    m_server->refocus(sourceOutput);
    return true;
  }

  ScratchpadSlotConfig ScratchpadManager::effectiveConfig(std::string_view name) const {
    const auto& global = config().animation.scratchpad;
    ScratchpadSlotConfig resolved{
        .name = std::string(name),
        .scale = global.scale > 0.0 ? std::make_optional(global.scale) : std::nullopt,
        .direction = name.empty() ? (global.scale <= 0.0 ? "" : global.direction)
                                  : (global.direction.empty() ? "top" : global.direction),
        .style = global.style.empty() ? std::nullopt : std::make_optional(global.style),
        .durationMs = global.durationMs,
        .curve = global.curve,
        .dim = global.dim,
        .blur = global.blur,
        .maximize = global.maximize,
        .maximizeToEdges = global.maximizeToEdges,
        .fullscreen = global.fullscreen,
        .suspendHidden = global.suspendHidden,
        .onEmpty = std::nullopt,
        .layout = std::nullopt,
        .gap = std::nullopt,
    };

    for (const auto& slotCfg : config().scratchpadRules) {
      if (slotCfg.name == name) {
        if (slotCfg.scale.has_value())
          resolved.scale = slotCfg.scale;
        if (slotCfg.direction.has_value())
          resolved.direction = slotCfg.direction;
        if (slotCfg.style.has_value())
          resolved.style = slotCfg.style;
        if (slotCfg.durationMs.has_value())
          resolved.durationMs = slotCfg.durationMs;
        if (slotCfg.curve.has_value())
          resolved.curve = slotCfg.curve;
        if (slotCfg.dim.has_value())
          resolved.dim = slotCfg.dim;
        if (slotCfg.blur.has_value())
          resolved.blur = slotCfg.blur;
        if (slotCfg.maximize.has_value())
          resolved.maximize = slotCfg.maximize;
        if (slotCfg.maximizeToEdges.has_value())
          resolved.maximizeToEdges = slotCfg.maximizeToEdges;
        if (slotCfg.fullscreen.has_value())
          resolved.fullscreen = slotCfg.fullscreen;
        if (slotCfg.suspendHidden.has_value())
          resolved.suspendHidden = slotCfg.suspendHidden;
        if (slotCfg.onEmpty.has_value())
          resolved.onEmpty = slotCfg.onEmpty;
        if (slotCfg.layout.has_value())
          resolved.layout = slotCfg.layout;
        if (slotCfg.gap.has_value())
          resolved.gap = slotCfg.gap;
        break;
      }
    }
    return resolved;
  }

  void ScratchpadManager::recordPendingCapture(Output* output, std::string_view slotName, pid_t pid) {
    if (pid <= 0) {
      return;
    }
    m_pendingCaptures.push_back(
        PendingCapture{
            .output = output,
            .slotName = std::string(slotName),
            .pid = pid,
            .timestamp = std::chrono::steady_clock::now(),
        }
    );
  }

  std::optional<ScratchpadManager::PendingCapture> ScratchpadManager::peekPendingCapture(const View* view) const {
    if (m_pendingCaptures.empty() || view == nullptr) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    const pid_t clientPid = viewPid(view);
    if (clientPid <= 0) {
      return std::nullopt;
    }
    const auto match = std::ranges::find_if(m_pendingCaptures, [clientPid, &now](const PendingCapture& pending) {
      if (std::chrono::duration_cast<std::chrono::seconds>(now - pending.timestamp).count() > kCaptureTimeoutSec) {
        return false;
      }
      return isDescendantOf(clientPid, pending.pid);
    });
    if (match == m_pendingCaptures.end()) {
      return std::nullopt;
    }
    return *match;
  }

  std::optional<std::string> ScratchpadManager::slotForView(const View* view) const {
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it != m_entries.end()) {
      return it->name;
    }
    return std::nullopt;
  }

  std::optional<ScratchpadManager::PendingCapture> ScratchpadManager::consumePendingCapture(const View* view) {
    if (m_pendingCaptures.empty() || view == nullptr) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_pendingCaptures, [&now](const PendingCapture& pending) {
      return std::chrono::duration_cast<std::chrono::seconds>(now - pending.timestamp).count() > kCaptureTimeoutSec;
    });

    const pid_t clientPid = viewPid(view);
    if (clientPid <= 0) {
      return std::nullopt;
    }
    const auto match = std::ranges::find_if(m_pendingCaptures, [clientPid](const PendingCapture& pending) {
      return isDescendantOf(clientPid, pending.pid);
    });
    if (match == m_pendingCaptures.end()) {
      return std::nullopt;
    }
    PendingCapture capture = std::move(*match);
    m_pendingCaptures.erase(match);
    return capture;
  }

  void ScratchpadManager::retargetBackdrop(Output* output, bool visible, bool animateTransition) {
    if (output == nullptr) {
      return;
    }
    const auto& animation = config().animation;
    auto fadeIt = m_backdropFades.try_emplace(output, 0.0).first;
    AnimatedValue& backdropFade = fadeIt->second;
    const double fadeTarget = visible ? 1.0 : 0.0;

    int durationMs = animation.scratchpad.durationMs;
    AnimationCurve curve = animation.scratchpad.curve;
    for (const auto& slot : m_visibleSlots) {
      if (slot.output == output) {
        const auto slotCfg = effectiveConfig(slot.name);
        durationMs = slotCfg.durationMs.value_or(durationMs);
        curve = slotCfg.curve.value_or(curve);
        break;
      }
    }

    if (animateTransition
        && animation.enabled
        && (backdropFade.animating() || backdropFade.current() != fadeTarget)) {
      backdropFade.retarget(fadeTarget, durationMs, curve);
    } else {
      backdropFade.snap(fadeTarget);
    }
    updateDimAndBlur(output);
  }

  ScratchpadManager::SlotLayout&
  ScratchpadManager::layoutForSlot(std::string_view name, std::span<Entry* const> entries) {
    const auto slotCfg = effectiveConfig(name);
    ResolvedLayoutConfig wanted = resolveGlobalLayout(config());
    wanted.mode = slotCfg.layout.value_or(wanted.mode);
    if (slotCfg.gap.has_value()) {
      const int delta = *slotCfg.gap - wanted.gap;
      wanted.gap = *slotCfg.gap;
      wanted.totalGap += delta;
      wanted.edgePad += delta;
    }

    auto [it, inserted] = m_slotLayouts.try_emplace(std::string(name));
    SlotLayout& slot = it->second;
    // A mode change has to rebuild the layout; anything else is just new config.
    if (slot.layout == nullptr || slot.config.mode != wanted.mode) {
      slot.layout = createLayout(wanted.mode);
      slot.members.clear();
    }
    slot.config = std::move(wanted);
    slot.layout->setConfig(&slot.config);
    slot.layout->setConstraints(&viewLayoutConstraints);

    std::vector<View*> wantedMembers;
    wantedMembers.reserve(entries.size());
    for (const Entry* entry : entries) {
      if (entry->view != nullptr && entry->view->mapped()) {
        wantedMembers.push_back(entry->view);
      }
    }
    for (View* member : slot.members) {
      if (std::ranges::find(wantedMembers, member) == wantedMembers.end()) {
        slot.layout->removeView(member);
      }
    }
    for (View* member : wantedMembers) {
      if (std::ranges::find(slot.members, member) == slot.members.end()) {
        slot.layout->insertView(member, static_cast<int>(slot.layout->columns().size()));
      }
    }
    slot.members = std::move(wantedMembers);
    return slot;
  }

  void ScratchpadManager::arrangeSlot(Output* output, std::string_view name, bool animate) {
    if (output == nullptr) {
      return;
    }
    wlr_box targetArea = output->usableArea();
    if (targetArea.width <= 0 || targetArea.height <= 0) {
      wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &targetArea);
    }

    const auto slotCfg = effectiveConfig(name);
    const auto& animation = config().animation;

    wlr_box fullOutputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &fullOutputBox);

    int scaledW = 0;
    int scaledH = 0;
    int baseX = 0;
    int baseY = 0;

    const bool isScaled = slotCfg.scale.has_value() ? (*slotCfg.scale > 0.0 && *slotCfg.scale <= 1.0)
                                                    : (config().animation.scratchpad.scale > 0.0);
    const bool isFullscreen = slotCfg.fullscreen.value_or(animation.scratchpad.fullscreen);
    const bool isMaximizeToEdges = slotCfg.maximizeToEdges.value_or(animation.scratchpad.maximizeToEdges);
    const bool isMaximize = slotCfg.maximize.value_or(animation.scratchpad.maximize);
    const bool preserveFloating =
        !isFullscreen && !isMaximizeToEdges && !isMaximize && !isScaled && !slotCfg.layout.has_value();

    if (isFullscreen || isMaximizeToEdges) {
      scaledW = fullOutputBox.width;
      scaledH = fullOutputBox.height;
      baseX = fullOutputBox.x;
      baseY = fullOutputBox.y;
    } else if (isMaximize) {
      const int gap = slotCfg.gap.value_or(config().layoutGap());
      if (gap >= 0 && targetArea.width > 2 * gap && targetArea.height > 2 * gap) {
        scaledW = targetArea.width - 2 * gap;
        scaledH = targetArea.height - 2 * gap;
        baseX = targetArea.x + gap;
        baseY = targetArea.y + gap;
      } else {
        scaledW = targetArea.width;
        scaledH = targetArea.height;
        baseX = targetArea.x;
        baseY = targetArea.y;
      }
    } else if (isScaled && targetArea.width > 0 && targetArea.height > 0) {
      const double scale = slotCfg.scale.has_value() ? *slotCfg.scale : animation.scratchpad.scale;
      scaledW = std::max(100, static_cast<int>(std::lround(targetArea.width * scale)));
      scaledH = std::max(100, static_cast<int>(std::lround(targetArea.height * scale)));
      baseX = targetArea.x + (targetArea.width - scaledW) / 2;
      baseY = targetArea.y + (targetArea.height - scaledH) / 2;
    }

    std::vector<Entry*> slotEntries;
    for (Entry& entry : m_entries) {
      if (entry.output == output && entry.name == name) {
        slotEntries.push_back(&entry);
      }
    }

    const bool visible = isSlotVisible(output, name);
    const int durationMs = slotCfg.durationMs.value_or(animation.scratchpad.durationMs);
    const AnimationCurve curve = slotCfg.curve.value_or(animation.scratchpad.curve);
    const std::string dir = slotCfg.direction.value_or(animation.scratchpad.direction);
    // Unscaled slots always fade in place; their geometry is already whatever the
    // window had before, so sliding is visually jarring.
    const std::string style = preserveFloating
        ? "fade"
        : slotCfg.style.value_or(
              animation.scratchpad.style.empty() ? std::string{"slide"} : animation.scratchpad.style
          );
    const bool suspendHidden = slotCfg.suspendHidden.value_or(animation.scratchpad.suspendHidden);

    if (preserveFloating) {
      for (Entry* entry : slotEntries) {
        View* view = entry->view;
        if (view == nullptr || !view->mapped()) {
          continue;
        }
        if (visible) {
          view->setOnActiveWorkspace(true);
          view->enterForeignOutput(output);
          std::erase(m_hidingViews, view);
          view->cancelPositionAnimation();
          view->cancelFadeAnimation();
          view->setNodeEnabled(true);
          if (suspendHidden) {
            safeSetSuspended(view, false);
          }
          if (animate && animation.enabled) {
            view->setFadeAlpha(0.0F);
            view->animateFadeTo(1.0F, durationMs, curve);
          } else {
            view->setFadeAlpha(1.0F);
          }
        } else {
          view->setOnActiveWorkspace(false);
          if (animate && animation.enabled) {
            view->setNodeEnabled(true);
            view->animateFadeTo(0.0F, durationMs, curve);
            if (std::ranges::find(m_hidingViews, view) == m_hidingViews.end()) {
              m_hidingViews.push_back(view);
            }
          } else {
            view->setFadeAlpha(0.0F);
            view->setNodeEnabled(false);
            if (suspendHidden) {
              safeSetSuspended(view, true);
            }
          }
        }
      }
      return;
    }

    // The slot\x27s own layout tiles inside the scaled box, so a scratchpad obeys
    // the same layout rules as a workspace.
    const wlr_box slotBox{baseX, baseY, scaledW, scaledH};
    SlotLayout& slotLayout = layoutForSlot(name, slotEntries);
    slotLayout.layout->arrange(slotBox);

    for (Entry* entry : slotEntries) {
      View* view = entry->view;
      if (view == nullptr || !view->mapped()) {
        continue;
      }
      const wlr_box target = slotLayout.layout->targetBox(view);
      if (target.width <= 0 || target.height <= 0) {
        continue;
      }
      const auto [offX, offY] = slideOffset(dir, targetArea, target);

      constexpr double kPopinScale = 0.8;
      const int shrunkW = std::max(1, static_cast<int>(std::lround(target.width * kPopinScale)));
      const int shrunkH = std::max(1, static_cast<int>(std::lround(target.height * kPopinScale)));
      const wlr_box shrunk{
          target.x + (target.width - shrunkW) / 2,
          target.y + (target.height - shrunkH) / 2,
          shrunkW,
          shrunkH,
      };
      const bool popin = style == "popin";
      const bool fadeWithMotion = popin || style == "slidefade";

      // Request the size before presenting it, so the clip derives from the geometry the client is
      // being asked for rather than the one it is about to leave.
      if (view->toplevel() != nullptr) {
        wlr_xdg_toplevel_set_size(view->toplevel(), target.width, target.height);
      }
      view->applyPresentation(target);

      if (visible) {
        view->setOnActiveWorkspace(true);
        view->enterForeignOutput(output);
        std::erase(m_hidingViews, view);
        view->cancelPositionAnimation();
        view->cancelFadeAnimation();
        view->setNodeEnabled(true);
        if (suspendHidden) {
          safeSetSuspended(view, false);
        }
        if (animate && animation.enabled) {
          if (fadeWithMotion) {
            view->setFadeAlpha(0.0F);
            view->animateFadeTo(1.0F, durationMs, curve);
          } else {
            view->setFadeAlpha(1.0F);
          }
          if (popin) {
            view->beginZoomAnimation(shrunk, target, durationMs, curve);
          } else {
            view->setPosition(offX, offY);
            view->animateTo(target.x, target.y, durationMs, curve);
          }
        } else {
          view->cancelFadeAnimation();
          view->cancelPositionAnimation();
          view->setPosition(target.x, target.y);
          view->setFadeAlpha(1.0F);
        }
      } else {
        view->setOnActiveWorkspace(false);
        if (animate && animation.enabled) {
          view->setNodeEnabled(true);
          if (fadeWithMotion) {
            view->animateFadeTo(0.0F, durationMs, curve);
          } else {
            view->setFadeAlpha(1.0F);
          }
          if (popin) {
            view->beginZoomAnimation(target, shrunk, durationMs, curve);
          } else {
            view->animateTo(offX, offY, durationMs, curve);
          }
          if (std::ranges::find(m_hidingViews, view) == m_hidingViews.end()) {
            m_hidingViews.push_back(view);
          }
        } else {
          std::erase(m_hidingViews, view);
          view->cancelFadeAnimation();
          view->cancelPositionAnimation();
          view->setFadeAlpha(0.0F);
          view->setNodeEnabled(false);
          if (suspendHidden) {
            safeSetSuspended(view, true);
          }
        }
      }
    }
  }

  void ScratchpadManager::setSlotVisible(Output* output, std::string_view name, bool visible, bool animateTransition) {
    if (output == nullptr) {
      return;
    }
    const VisibleSlot slot{.output = output, .name = std::string(name)};
    if (visible) {
      if (std::ranges::find(m_visibleSlots, slot) == m_visibleSlots.end()) {
        m_visibleSlots.push_back(slot);
      }
    } else {
      std::erase(m_visibleSlots, slot);
    }
    retargetBackdrop(output, isOutputVisible(output), animateTransition);
    arrangeSlot(output, name, animateTransition);
    updateDimAndBlur(output);
  }

  wlr_scene_rect* ScratchpadManager::dimRectFor(Output* output) {
    if (const auto it = m_dimRects.find(output); it != m_dimRects.end()) {
      return it->second;
    }
    static constexpr float kBlack[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    wlr_scene_rect* rect = wlr_scene_rect_create(m_root, 1, 1, kBlack);
    wlr_scene_node_lower_to_bottom(&rect->node);
    wlr_scene_node_set_enabled(&rect->node, false);
    m_dimRects.emplace(output, rect);
    return rect;
  }

  wlr_scene_blur* ScratchpadManager::blurNodeFor(Output* output) {
    if (const auto it = m_blurNodes.find(output); it != m_blurNodes.end()) {
      return it->second;
    }
    wlr_scene_blur* blur = wlr_scene_blur_create(m_root, 1, 1);
    if (blur != nullptr) {
      wlr_scene_node_lower_to_bottom(&blur->node);
      wlr_scene_node_set_enabled(&blur->node, false);
      m_blurNodes.emplace(output, blur);
    }
    return blur;
  }

  void ScratchpadManager::updateDimAndBlur(Output* output) {
    if (output == nullptr) {
      return;
    }
    const bool visible = isOutputVisible(output);
    double maxDim = 0.0;
    bool blurEnabled = false;
    for (const auto& slot : m_visibleSlots) {
      if (slot.output == output) {
        const auto slotCfg = effectiveConfig(slot.name);
        if (slotCfg.dim.has_value() && *slotCfg.dim > maxDim) {
          maxDim = *slotCfg.dim;
        }
        if (slotCfg.blur.value_or(false)) {
          blurEnabled = true;
        }
      }
    }
    if (!config().appearance.blur.enabled) {
      blurEnabled = false;
    }

    const auto fade = m_backdropFades.find(output);
    const float curAlpha = fade != m_backdropFades.end() ? static_cast<float>(fade->second.current()) : 0.0F;

    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &box);

    if (wlr_scene_rect* rect = dimRectFor(output)) {
      if ((!visible && curAlpha <= 0.001F) || maxDim <= 0.0 || curAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&rect->node, false);
      } else {
        wlr_scene_node_set_position(&rect->node, box.x, box.y);
        wlr_scene_rect_set_size(rect, box.width, box.height);
        const float color[4] = {0.0F, 0.0F, 0.0F, std::clamp(static_cast<float>(maxDim * curAlpha), 0.0F, 1.0F)};
        wlr_scene_rect_set_color(rect, color);
        wlr_scene_node_set_enabled(&rect->node, true);
      }
    }

    if (wlr_scene_blur* blur = blurNodeFor(output)) {
      if ((!visible && curAlpha <= 0.001F) || !blurEnabled || curAlpha <= 0.001F) {
        wlr_scene_node_set_enabled(&blur->node, false);
      } else {
        wlr_scene_node_set_position(&blur->node, box.x, box.y);
        wlr_scene_blur_set_size(blur, box.width, box.height);
        wlr_scene_blur_set_corner_radius(blur, 0);
        wlr_scene_blur_set_alpha(blur, std::clamp(curAlpha, 0.0F, 1.0F));
        wlr_scene_node_set_enabled(&blur->node, true);
      }
    }
  }

  void ScratchpadManager::releaseOutput(Output* output) {
    std::erase_if(m_visibleSlots, [output](const VisibleSlot& slot) { return slot.output == output; });
    m_backdropFades.erase(output);
    if (const auto it = m_dimRects.find(output); it != m_dimRects.end()) {
      wlr_scene_node_destroy(&it->second->node);
      m_dimRects.erase(it);
    }
    if (const auto it = m_blurNodes.find(output); it != m_blurNodes.end()) {
      wlr_scene_node_destroy(&it->second->node);
      m_blurNodes.erase(it);
    }
  }

  void ScratchpadManager::applyConfig() {
    const auto& animation = config().animation;
    const bool animate = animation.enabled && animation.scratchpad.enabled;
    if (!animate) {
      for (auto& [output, fade] : m_backdropFades) {
        fade.snap(isOutputVisible(output) ? 1.0 : 0.0);
        updateDimAndBlur(output);
      }
      for (const VisibleSlot& slot : m_visibleSlots) {
        arrangeSlot(slot.output, slot.name, false);
      }
    }
  }

  bool ScratchpadManager::toggle(Output* output, std::string_view name) {
    if (output == nullptr) {
      return false;
    }

    if (!slotHasWindows(output, name)) {
      const auto slotCfg = effectiveConfig(name);
      if (isSlotVisible(output, name)) {
        setSlotVisible(output, name, false);
        m_server->refocus(output);
      } else {
        for (const std::string& oldName : otherVisibleSlots(output, name)) {
          setSlotVisible(output, oldName, false);
        }
        setSlotVisible(output, name, true);
        if (slotCfg.onEmpty.has_value() && !slotCfg.onEmpty->empty()) {
          recordPendingCapture(output, name, m_server->spawn(slotCfg.onEmpty->c_str()));
        }
      }
      return true;
    }

    if (!name.empty()) {
      Output* currentOwner = nullptr;
      for (const Entry& entry : m_entries) {
        if (entry.name == name) {
          currentOwner = entry.output;
          break;
        }
      }

      if (currentOwner != nullptr && currentOwner != output && isSlotVisible(currentOwner, name)) {
        setSlotVisible(currentOwner, name, false);
        for (Entry& entry : m_entries) {
          if (entry.name == name) {
            entry.output = output;
          }
        }
        setSlotVisible(output, name, true);
        if (View* view = focused(output, name)) {
          m_server->focusView(view);
        }
        return true;
      }
    }

    const bool show = !isSlotVisible(output, name);
    if (show) {
      // At most one slot is visible per output; dismiss any other open slot.
      for (const std::string& oldName : otherVisibleSlots(output, name)) {
        setSlotVisible(output, oldName, false);
      }
    }
    setSlotVisible(output, name, show);
    if (show) {
      if (View* view = focused(output, name)) {
        m_server->focusView(view);
      }
    } else {
      m_server->refocus(output);
    }
    return true;
  }

  void ScratchpadManager::hideAll() {
    const std::vector<VisibleSlot> visibleSlots = m_visibleSlots;
    for (const auto& slot : visibleSlots) {
      setSlotVisible(slot.output, slot.name, false, false);
    }
  }

  View* ScratchpadManager::focused(Output* output, std::string_view name) const {
    std::string slotName(name);
    if (slotName.empty()) {
      if (auto openSlot = visibleSlotName(output)) {
        slotName = *openSlot;
      }
    }
    if (!isSlotVisible(output, slotName)) {
      return nullptr;
    }
    if (m_focusedView != nullptr) {
      const auto isCur = std::ranges::find_if(m_entries, [this, output, &slotName](const Entry& entry) {
        return entry.output == output && entry.name == slotName && entry.view == m_focusedView;
      });
      if (isCur != m_entries.end()) {
        return m_focusedView;
      }
    }
    const auto remembered = std::ranges::find_if(m_entries, [output, &slotName](const Entry& entry) {
      return entry.output == output && entry.name == slotName && entry.lastFocused;
    });
    if (remembered != m_entries.end()) {
      return remembered->view;
    }
    for (const Entry& entry : m_entries) {
      if (entry.output == output && entry.name == slotName) {
        return entry.view;
      }
    }
    return nullptr;
  }

  bool ScratchpadManager::hasFocus(Output* output) const {
    if (m_focusedView == nullptr || !isOutputVisible(output)) {
      return false;
    }
    return std::ranges::any_of(m_entries, [this, output](const Entry& entry) {
      return entry.view == m_focusedView && entry.output == output && isSlotVisible(output, entry.name);
    });
  }

  void ScratchpadManager::noteFocus(View* view) {
    m_focusedView = nullptr;
    const auto focusedEntry =
        std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (focusedEntry == m_entries.end()) {
      return;
    }

    m_focusedView = view;
    for (Entry& entry : m_entries) {
      if (entry.output == focusedEntry->output && entry.name == focusedEntry->name) {
        entry.lastFocused = (entry.view == view);
      }
    }
  }

  void ScratchpadManager::finishMove(View* view, Output* output) {
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it == m_entries.end() || view == nullptr) {
      return;
    }
    Output* previous = it->output;
    std::string slotName = it->name;
    if (output != nullptr) {
      it->output = output;
      const VisibleSlot slot{.output = output, .name = slotName};
      if (std::ranges::find(m_visibleSlots, slot) == m_visibleSlots.end()) {
        m_visibleSlots.push_back(slot);
      }
    }
    if (previous != it->output) {
      it->displacedOutput.clear();
      it->displacedPosition.reset();
    }
    if (previous != it->output && it->lastFocused) {
      for (Entry& entry : m_entries) {
        if (&entry != &*it && entry.output == it->output && entry.name == slotName) {
          entry.lastFocused = false;
        }
      }
    }
    if (previous != it->output && std::ranges::none_of(m_entries, [previous, &slotName](const Entry& entry) {
          return entry.output == previous && entry.name == slotName;
        })) {
      std::erase(m_visibleSlots, VisibleSlot{.output = previous, .name = slotName});
    }
    restorePresentation(view);
  }

  void ScratchpadManager::restorePresentation(View* view) {
    const auto entry =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (view == nullptr || entry == m_entries.end()) {
      return;
    }
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setOnActiveWorkspace(true);
    view->enterForeignOutput(entry->output);
    view->setNodeEnabled(true);
  }

  bool ScratchpadManager::focusNext(Output* output, std::string_view name) {
    if (output == nullptr || !isSlotVisible(output, name)) {
      return false;
    }
    std::vector<Entry*> visible;
    for (Entry& entry : m_entries) {
      if (entry.output == output && entry.name == name) {
        visible.push_back(&entry);
      }
    }
    if (visible.empty()) {
      return false;
    }
    if (visible.size() == 1) {
      m_server->focusView(visible.front()->view);
      return true;
    }

    size_t currentIndex = 0;
    for (size_t i = 0; i < visible.size(); ++i) {
      if (visible[i]->view == m_focusedView) {
        currentIndex = i;
        break;
      }
    }
    const size_t nextIndex = (currentIndex + 1) % visible.size();
    m_server->focusView(visible[nextIndex]->view);
    return true;
  }

  bool ScratchpadManager::restoreFocused(Output* output, std::string_view name) {
    if (output == nullptr) {
      return false;
    }
    View* view = focused(output, name);
    if (view == nullptr) {
      return false;
    }
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it == m_entries.end()) {
      return false;
    }
    Entry entry = std::move(*it);
    m_entries.erase(it);
    if (!slotHasWindows(output, name)) {
      setSlotVisible(output, name, false);
    } else if (isSlotVisible(output, name)) {
      arrangeSlot(output, name, true);
    }
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);
    Output* restoreOutput = m_server->outputFromName(entry.returnOutput);
    if (restoreOutput == nullptr) {
      restoreOutput = output;
    }
    Workspace* workspace = restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr
        ? restoreOutput->workspaceGroup()->workspaceNamed(entry.returnWorkspace)
        : nullptr;
    if (workspace == nullptr && restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr) {
      workspace = restoreOutput->workspaceGroup()->active();
    }
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    view->moveToWorkspace(workspace, false);
    if (entry.returnTiled) {
      view->setFloating(false, true, /*preserveSize=*/true);
    } else {
      view->setFloating(true);
      if (restoreOutput != nullptr && restoreOutput != output) {
        const wlr_box usable = restoreOutput->usableArea();
        if (usable.width > 0 && usable.height > 0) {
          const int w = view->toplevel()->current.width;
          const int h = view->toplevel()->current.height;
          view->setPosition(
              std::clamp(view->sceneTree()->node.x, usable.x, usable.x + std::max(0, usable.width - w)),
              std::clamp(view->sceneTree()->node.y, usable.y, usable.y + std::max(0, usable.height - h))
          );
        }
      }
    }
    if (workspace != nullptr) {
      workspace->syncViewPresentation(view);
    }
    m_server->focusView(view);
    return true;
  }

  void ScratchpadManager::remove(View* view) {
    const auto entry =
        std::ranges::find_if(m_entries, [view](const Entry& candidate) { return candidate.view == view; });
    if (entry == m_entries.end()) {
      return;
    }
    Output* entryOutput = entry->output;
    std::string slotName = entry->name;
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase(m_hidingViews, view);
    m_entries.erase(entry);
    if (entryOutput != nullptr) {
      pruneSlot(entryOutput, slotName);
      if (isSlotVisible(entryOutput, slotName)) {
        arrangeSlot(entryOutput, slotName, true);
      }
    }
  }

  // A slot that is hidden and empty has nothing left to address, so it is
  // dropped. A visible one stays: toggling it shut is still meaningful, and
  // on_empty may be about to fill it.
  void ScratchpadManager::pruneSlot(Output* output, std::string_view name) {
    if (output == nullptr || slotHasWindows(output, name) || isSlotVisible(output, name)) {
      return;
    }
    std::erase(m_visibleSlots, VisibleSlot{.output = output, .name = std::string(name)});
    // The name can still be in use on another output after a drag, and that
    // slot\x27s layout state is worth keeping.
    if (std::ranges::none_of(m_entries, [name](const Entry& e) { return e.name == name; })) {
      m_slotLayouts.erase(std::string(name));
    }
    if (!isOutputVisible(output)) {
      retargetBackdrop(output, false);
    }
  }

  void ScratchpadManager::moveOutput(Output* from, Output* to) {
    if (from == to) {
      return;
    }
    const std::vector<std::string> wasVisibleSlots = visibleSlotsOn(from);
    for (const std::string& slotName : wasVisibleSlots) {
      setSlotVisible(from, slotName, false);
    }
    for (Entry& entry : m_entries) {
      if (entry.output == from) {
        if (entry.displacedOutput.empty() && from != nullptr && from->wlr()->name != nullptr) {
          entry.displacedOutput = from->wlr()->name;
          const wlr_box homeArea = from->layoutBox();
          if (entry.view != nullptr && homeArea.width > 0 && homeArea.height > 0) {
            entry.displacedPosition = {{
                static_cast<double>(entry.view->sceneTree()->node.x - homeArea.x) / homeArea.width,
                static_cast<double>(entry.view->sceneTree()->node.y - homeArea.y) / homeArea.height,
            }};
          }
        }
        entry.output = to;
        if (from != nullptr && to != nullptr && entry.view != nullptr) {
          const wlr_box srcArea = from->usableArea();
          const wlr_box dstArea = to->usableArea();
          if (srcArea.width > 0 && srcArea.height > 0 && dstArea.width > 0 && dstArea.height > 0) {
            const double xFrac = static_cast<double>(entry.view->sceneTree()->node.x - srcArea.x) / srcArea.width;
            const double yFrac = static_cast<double>(entry.view->sceneTree()->node.y - srcArea.y) / srcArea.height;
            const int newX = dstArea.x + static_cast<int>(std::lround(xFrac * dstArea.width));
            const int newY = dstArea.y + static_cast<int>(std::lround(yFrac * dstArea.height));
            entry.view->cancelPositionAnimation();
            entry.view->setPosition(
                std::clamp(
                    newX, dstArea.x, dstArea.x + std::max(0, dstArea.width - entry.view->toplevel()->current.width)
                ),
                std::clamp(
                    newY, dstArea.y, dstArea.y + std::max(0, dstArea.height - entry.view->toplevel()->current.height)
                )
            );
          }
        }
      }
    }
    if (!wasVisibleSlots.empty() && to != nullptr) {
      for (const std::string& slotName : wasVisibleSlots) {
        setSlotVisible(to, slotName, true);
      }
    }
  }

  size_t ScratchpadManager::restoreDisplaced(Output* fallback) {
    if (fallback == nullptr || m_server == nullptr) {
      return 0;
    }
    size_t restored = 0;
    for (Entry& entry : m_entries) {
      Output* home = entry.displacedOutput.empty() ? nullptr : m_server->outputFromName(entry.displacedOutput);
      if (home != nullptr) {
        const wlr_box homeArea = home->layoutBox();
        if (entry.view != nullptr && entry.displacedPosition && homeArea.width > 0 && homeArea.height > 0) {
          entry.view->cancelPositionAnimation();
          entry.view->setPosition(
              homeArea.x + static_cast<int>(std::lround((*entry.displacedPosition)[0] * homeArea.width)),
              homeArea.y + static_cast<int>(std::lround((*entry.displacedPosition)[1] * homeArea.height))
          );
        }
        entry.displacedOutput.clear();
        entry.displacedPosition.reset();
      }
      Output* target = home != nullptr ? home : (entry.output == nullptr ? fallback : nullptr);
      if (target == nullptr || target == entry.output) {
        continue;
      }
      Output* source = entry.output;
      const bool wasVisible = source != nullptr && isSlotVisible(source, entry.name);
      if (entry.lastFocused) {
        for (Entry& candidate : m_entries) {
          if (&candidate != &entry && candidate.output == target && candidate.name == entry.name) {
            candidate.lastFocused = false;
          }
        }
      }
      entry.output = target;
      if (wasVisible) {
        if (std::ranges::none_of(m_entries, [source, &entry](const Entry& candidate) {
              return candidate.output == source && candidate.name == entry.name;
            })) {
          setSlotVisible(source, entry.name, false);
        }
        setSlotVisible(target, entry.name, true);
      } else if (entry.view != nullptr) {
        const bool visible = isSlotVisible(target, entry.name);
        entry.view->setOnActiveWorkspace(visible);
        entry.view->setNodeEnabled(visible);
      }
      ++restored;
    }
    return restored;
  }

} // namespace umbriel
