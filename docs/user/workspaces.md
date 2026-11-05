# Workspaces

Each output has its own workspaces. Choose a dynamic or fixed model, then use workspace rules to customize individual entries.

## Choose a workspace model

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`:

```toml
[output.DP-1]
workspaces = "dynamic"
min_workspaces = 3
```

A dynamic output keeps an empty workspace at the end and creates another when that workspace gains a window. Empty inactive anonymous workspaces are removed again. `min_workspaces` sets a floor, not a maximum.

Closing the last window keeps the current workspace active. Cleanup runs after you leave it and the workspace transition finishes. A client that keeps its closed toplevel alive does not prevent normal empty-workspace cleanup.

An empty anonymous workspace reserved for an application that is still starting disappears from the visible list after you leave it. Umbriel retains its destination privately and restores the workspace when the application's toplevel claims it. An unused startup token does not leave a selectable empty workspace behind.

Set `empty_above = true` under the global [`[workspaces]`](#global-workspace-settings) section to keep an additional empty workspace at the beginning.

#### Persistent names in a dynamic inventory

A name-based `[[workspace]]` entry creates a persistent named workspace:

```toml
[[workspace]]
name = "CHAT"
layout.mode = "master"

[[workspace]]
name = "STATS"
output = "DP-1"
layout.mode = "dwindle"
```

Names are case-sensitive and local to an output. An entry without `output` applies independently to every dynamic output. Named workspaces remain when empty while anonymous neighbors continue their normal dynamic lifecycle.

Each output supports up to 64 workspaces.

### Static workspaces

Use an integer for a fixed count or a string array for an ordered list of names:

```toml
[output.DP-1]
workspaces = 5

[output.DP-2]
workspaces = ["WEB", "CHAT", "VIDEO"]
```

A count creates anonymous positions. A string list creates exact names, so position 3 and the name `"3"` remain different selectors. Static workspaces are never pruned.

### Mirror another output's workspace

An entry of a name list written `"<name>@<output>"` is a mirror: it shows the workspace `<name>` of `<output>`, with that workspace's real windows, on the output that lists it.

```toml
[output.DP-1]
workspaces = ["WEB", "CHAT"]

[output.HDMI-A-1]
workspaces = ["TERM", "WEB@DP-1", "CHAT@DP-1"]
```

A mirror is a workspace of the output that lists it. It keeps the place it has in the list, appears in `umbriel workspaces` and in the ext-workspace protocol, and is selected like any other workspace, by position, by name, or from a bar. It occupies the output only while it is active, and the wallpaper stays visible around the windows.

- The windows are the originals, drawn live at the ratio of the two outputs' widths: 0.75 when a 2560x1440 output is shown on a 1920x1080 one. When the formats differ the copy is centered. Nothing is drawn twice by the client, so the copy is a scaled picture and can look soft.
- Pointer, keyboard, scrolling, dragging, moving and resizing act on the real window, at the coordinates of the original. Menus and popups open inside the mirror. Acting on a mirror gives the real window focus without changing the workspace the origin output shows.
- Window and layout actions address the source layout while a mirror is active, even when the origin is hidden: focus, reordering, maximize, fullscreen, resizing, floating and the layout's own actions act on the real windows without selecting the origin output. Workspace navigation still uses the receiving output; it does not switch the origin.
- Moving a window or column to a mirror inserts it into that mirror's source layout and selects the receiving copy, without switching the origin output. Moving to a copy of the same source is a no-op. A copy without a resolved source cannot receive a transfer.
- The copy stays current while the original is hidden, while the origin output is off, and while the output that shows the copy is off. An output that shows an unchanged copy is not redrawn.
- A fullscreen window covering the receiving output draws above its top-layer panels, including while the origin is hidden. Leaving fullscreen, switching away from the copy, or closing the window reveals the panels again. Overlay-layer surfaces keep their normal priority.
- New windows launched from a mirror join its source workspace and appear in the mirror, without switching the origin output. Explicit placement rules take priority. Pinned windows and scratchpads keep their local placement. An output needs at least one workspace of its own, used as the fallback while a mirror has no source.
- The mirror follows the workspace it shows. When `workspace-move-to-output` sends that workspace to another output the mirror keeps showing it, and when the origin output goes away it follows the refuge its windows were evacuated to. A workspace cannot be moved onto an output that already holds a workspace of the same name, a mirror included.
- A mirror cannot show a workspace of its own output, or another mirror. `umbriel config validate` rejects an unknown output, a name the origin output does not list, and these two cases. The origin output must use a name list.

`umbriel workspaces --json` marks a mirror with `"mirror": true` and reports the `id` and the output of the workspace it currently shows in `mirror_of` and `mirror_output`, or `null` while there is none.

### Change workspaces on reload

Workspace changes apply on a valid configuration reload. Umbriel preserves matching names and positions where possible. Windows from a removed workspace move to the nearest remaining workspace.

Changing to a dynamic model keeps active and populated workspaces, then restores the required empty workspace.

## Workspace axis

Set the workspace arrangement per output:

```toml
[output.DP-1]
workspace_axis = "horizontal"
```

| Value          | Behavior                                                             |
| -------------- | -------------------------------------------------------------------- |
| `"vertical"`   | Workspaces stack top to bottom; scrolling layouts run left to right. |
| `"horizontal"` | Workspaces sit side by side; scrolling layouts run top to bottom.    |

A three-finger swipe along the axis changes workspaces. A swipe across it moves the scrolling strip. The overview follows the same arrangement.

## Workspace selectors

Actions such as `workspace-switch` and `window-move-to-workspace` use these forms:

- Bare digits from `1` to `64` select a one-based position.
- Other text selects an exact, case-sensitive name.
- `--name <name>` forces name lookup, including for an all-digit name.
- `--output <output>` restricts selection to that output. A named selector can also use its unavailable original output to find an evacuated workspace.

```text
workspace-switch 3
workspace-switch 3 --output DP-2
workspace-switch CHAT --output DP-2
workspace-switch --name 3 --output DP-2
```

Duplicate names resolve on the pointer-preferred output when possible. Otherwise the selector is ambiguous. On a dynamic output, a numeric position beyond the current count selects the last workspace.

Window move bindings also work on a focused scratchpad window: they remove it from the scratchpad and restore its saved tiled or floating state on the destination workspace. Silent variants keep the current workspace active. See [Moving scratchpad windows](scratchpad.md#moving-scratchpad-windows).

Evacuated named workspaces keep their names even when appended at different positions on another output. Within one output, a native workspace takes precedence over evacuated workspaces with the same name. Multiple evacuated workspaces with the same name are ambiguous unless qualified by their original output. For example, `workspace-switch --name 4 --output DP-1` follows the named workspace `"4"` while `DP-1` is unavailable; `workspace-switch 4 --output DP-1` remains a position on a live `DP-1`. Disabled outputs are excluded from global name lookup.

## Moving a workspace between outputs

`workspace-move-to-output-next` and `workspace-move-to-output-previous` move the active workspace through the same output order as `output-focus-next` and `output-focus-previous`, wrapping at either end. The `left`, `right`, `up`, and `down` variants select an adjacent output.

`workspace-move-to-output <destination> [<workspace>] [--output <output>]` selects the destination by connector or monitor identity, or uses `current` for the pointer-preferred output. Omit the workspace to move the active one. A supplied workspace uses the same position/name selection as `workspace-switch`; `--output` qualifies the source, and `--name` selects an explicit name, including numeric names.

```sh
umbriel msg workspace-move-to-output DP-2
umbriel msg workspace-move-to-output DP-2 WEB --output DP-1
umbriel msg workspace-move-to-output current WEB --output DP-1
umbriel msg workspace-move-to-output DP-2 --name 3 --output DP-1
```

Moving an inactive workspace leaves its source output's active workspace selected. Moving a workspace to its existing output succeeds without changing selection or order. Unknown, disabled, or ambiguous destination outputs are rejected.

The workspace keeps its stable ID, explicit name, layout, and its windows, including floating windows. Pinned windows stay on their output, even when the workspace they were pinned on moves. The workspace is inserted immediately after the destination's active workspace and becomes active there. Keyboard focus follows it. When its active workspace moves, the source shows the preceding workspace, or the following one if the first workspace moved. An output whose only workspace moves receives an empty replacement.

Moves also work for empty workspaces and fixed inventories. They change the runtime inventory without editing configuration. Dynamic outputs maintain their empty workspace sentinels and minimum count. Explicit names stay with transferred workspaces; anonymous labels follow their new positions. A move is rejected if the destination already has the same explicit name or has reached the 64-workspace limit.

The transferred workspace retains its layout settings until a relevant configuration reload applies layout rules again. Changing workspace inventory configuration reapplies the configured inventories, including named workspace declarations. Output qualifiers in selectors refer to the workspace's current output. Its ID remains stable, so scripts should read the `output` field rather than infer the current output from the ID.

## Inspect workspace state

Run `umbriel workspaces` to list workspaces and their effective layout:

```text
* DP-1: 1 [scrolling] (focused)
  DP-1: 2 [dwindle]
* DP-2: WEB [master]
```

Use `umbriel workspaces --json` for scripts. Each entry includes its stable `id`, display `name`, one-based `index`, `output`, active and focused states, and effective `layout`.

## Global workspace settings

```toml
[workspaces]
back_and_forth = true
empty_above = false
```

| Key              | Default | Description                                                                |
| ---------------- | ------- | -------------------------------------------------------------------------- |
| `back_and_forth` | `false` | Selecting the active workspace returns to the previous one on that output. |
| `empty_above`    | `false` | Keep a leading empty workspace on dynamic outputs.                         |

## Workspace rules

`[[workspace]]` entries customize one workspace. Select it with exactly one of `name` or `index`, and optionally restrict it to an output:

```toml
[[workspace]]
name = "VIDEO"
output = "DP-2"
layout.mode = "dwindle"
layout.gap = 4
```

On dynamic outputs, `name` also creates a persistent named workspace. `index` follows whichever workspace currently occupies that position. On static outputs, rules only customize members already present in the configured inventory.

### How settings are combined

Settings apply from least to most specific:

1. Global `[layout]`
2. Matching output layout defaults (`layout.mode`, `layout.scrolling.default_extent_fraction`)
3. Matching workspace rule without `output`
4. Matching workspace rule with `output`

Later values replace earlier ones. Strut edges are resolved independently, so a rule can override only `layout.struts.top`.

### Available fields

| Key                                          | Description                                                                                                                                 |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `name`                                       | Select an exact name and create it on matching dynamic outputs.                                                                             |
| `index`                                      | Select a current one-based position from 1 to 64.                                                                                           |
| `output`                                     | Restrict the rule to a connector or monitor name.                                                                                           |
| `layout.mode`                                | Use `"scrolling"`, `"dwindle"`, `"master"`, or `"stacking"`.                                                                                |
| `layout.gap`                                 | Set the window gap.                                                                                                                         |
| `layout.struts.{left,right,top,bottom}`      | Reserve signed logical pixels at each edge.                                                                                                 |
| `layout.extent_presets`                      | Set extent-cycle fractions.                                                                                                                 |
| `layout.new_exits_fullscreen`                | Choose which arriving windows exit fullscreen; see [Leaving fullscreen](layout.md#leaving-fullscreen).                                      |
| `layout.client_maximize`                     | Choose `"column"` or `"edges"` for a tiled window's own maximize request; see [Maximize and fullscreen](layout.md#maximize-and-fullscreen). |
| `layout.stacking.drop_zones`                 | Enable edge and corner drag previews and placement in stacking; defaults to `true`.                                                         |
| `layout.scrolling.default_extent_fraction`   | Set the initial scrolling-column extent.                                                                                                    |
| `layout.scrolling.center_underfull_strip`    | Center or start-align an underfull strip.                                                                                                   |
| `layout.scrolling.center_focused`            | Control when focus changes center a column.                                                                                                 |
| `layout.scrolling.center_on_overview_select` | Center the column picked in the overview.                                                                                                   |
| `layout.master.position`                     | Place the master area left, right, or center.                                                                                               |
| `layout.master.default_width_fraction`       | Set the master-area fraction.                                                                                                               |
| `layout.master.new_on_top`                   | Place new stack windows at the top or bottom.                                                                                               |
| `layout.master.new_becomes_master`           | Give the master slot to new windows.                                                                                                        |
| `layout.dwindle.preserve_split`              | Keep split directions fixed.                                                                                                                |
| `layout.dwindle.new_toward_cursor`           | Open new windows on the pointer's side of the split.                                                                                        |

### Examples

```toml
[[workspace]]
output = "HDMI-A-1"
name = "CHAT"
layout.mode = "scrolling"
layout.scrolling.center_focused = "always"

[[workspace]]
output = "HDMI-A-1"
name = "STATS"
layout.mode = "dwindle"

[[workspace]]
index = 4
output = "DP-1"
layout.gap = 0
layout.struts.top = 24
layout.scrolling.default_extent_fraction = 0.667
```
