#pragma once

#include "scene/tab_bar_geometry.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class TabBar;
  class TabLabelCache;
  class View;
  class Workspace;
  struct Column;
  struct TabGroup;
  struct TabBarModel;

  // What a tab bar has under a point: a tab, or one of the buttons that cycle through its tabs, which name the tab on
  // show. `index` counts within the tab's group.
  struct TabHit {
    TabBarPart part = TabBarPart::Tab;
    View* tab = nullptr;
    size_t index = 0;
  };

  // Where a window dropped onto a tab group joins its tabs, as a row of the column, and the hint that shows it.
  struct TabDrop {
    int row = 0;
    const View* member = nullptr;
    wlr_box hint{};
  };

  // Presents a workspace's tab groups: hides every tab a group is not showing and reveals the rest, gives each group's
  // shown tab its bar, and answers the questions only the bars can, such as which tab lies under the pointer. The
  // layout owns which rows form groups and which tab each shows; this only makes the scene agree with it.
  class TabPresenter {
  public:
    explicit TabPresenter(Workspace& workspace);
    ~TabPresenter();
    TabPresenter(const TabPresenter&) = delete;
    TabPresenter& operator=(const TabPresenter&) = delete;

    // Bring every member's tab state in line with the layout. Runs after each arrange; a workspace without tab groups,
    // and without views still marked from one, costs a scan of its columns and nothing else.
    void sync();
    // Focus moved to `view`: show its tab now, so the focused window is never hidden for a frame.
    void focusChanged(View* view);
    // A member's title, app id, or urgency changed: refresh its group's bar.
    void memberChanged(const View* view);
    // `view` left the layout: it is no longer a tab, from this moment.
    void released(View* view);
    // Clear every member's tab state, for a workspace about to go away.
    void releaseAll();

    // The tab whose bar slot lies under the layout point.
    std::optional<TabHit> tabAt(double lx, double ly) const;
    // Where a window dropped at the layout point onto column `column` joins one of its tab groups, or nullopt when the
    // point is on none of them. Over a bar it lands beside the slot under the point; over the middle of a group it
    // lands where new tabs go. Near a group's ends it joins nothing, so the drop can land beside the group instead.
    std::optional<TabDrop> dropSlot(int column, double lx, double ly) const;
    // The tab `direction` steps from the focused one, wrapping at the ends when tabs are set to. Null outside a
    // tab group, at an end that does not wrap, or with a single tab.
    View* stepTarget(int direction) const;
    // The same step from the tab `member`'s group shows, for a bar the pointer is over rather than the focused one.
    View* stepFrom(const View* member, int direction) const;
    // Tab `index` of the focused group, counted from 1, or from the end when negative. Null when there is none.
    View* indexTarget(int index) const;
    // The tab before or after the one `member`'s group shows, wrapping at the ends whatever wrap_focus says: what
    // the bar's cycle buttons select.
    View* cycleFrom(const View* member, int direction) const;

  private:
    // A tab group and the column holding it, both owned by the layout and valid until it next changes.
    struct GroupRef {
      const Column* column = nullptr;
      const TabGroup* group = nullptr;
    };

    // One tab from the one `member`'s group shows, wrapping at the ends when `wrap` is set.
    View* step(const View* member, int direction, bool wrap) const;
    // Hide, reveal, and dress the members of one tab group.
    void presentGroup(const Column& column, const TabGroup& group);
    // Forget `view`'s tab state: reveal it and take its bar.
    void clear(View* view);
    bool barShown(const TabGroup& group) const;
    TabBarModel modelFor(const Column& column, const TabGroup& group) const;
    // The bar the group drew last, so its strip moves on from where it was; null before the first.
    static const TabBarModel* previousModel(const Column& column, const TabGroup& group);
    std::string titleFor(const View* view, size_t index) const;
    std::optional<GroupRef> groupOf(const View* view) const;
    static TabBar* barOf(const View* view);

    Workspace& m_workspace;
    std::shared_ptr<TabLabelCache> m_labels;
    // Views carrying tab state: hidden, or holding a bar. A view that leaves every tab group is cleared from here.
    std::vector<View*> m_members;
  };

} // namespace umbriel
