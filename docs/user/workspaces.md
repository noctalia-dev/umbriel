# Workspaces

Each output has its own workspaces. Choose a dynamic or fixed model, then use
workspace rules to customize individual entries.

## Choose a workspace model

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`:

```toml
[output.DP-1]
workspaces = "dynamic"
min_workspaces = 3
```

A dynamic output keeps an empty workspace at the end and creates another when
that workspace gains a window. Empty inactive anonymous workspaces are removed
again. `min_workspaces` sets a floor, not a maximum.

Set `empty_above = true` under the global
[`[workspaces]`](#global-workspace-settings) section to keep an additional empty
workspace at the beginning.

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

Names are case-sensitive and local to an output. An entry without `output`
applies independently to every dynamic output. Named workspaces remain when
empty while anonymous neighbors continue their normal dynamic lifecycle.

Each output supports up to 64 workspaces.

### Static workspaces

Use an integer for a fixed count or a string array for an ordered list of names:

```toml
[output.DP-1]
workspaces = 5

[output.DP-2]
workspaces = ["WEB", "CHAT", "VIDEO"]
```

A count creates anonymous positions. A string list creates exact names, so
position 3 and the name `"3"` remain different selectors. Static workspaces are
never pruned.

### Change workspaces on reload

Workspace changes apply on a valid configuration reload. Umbriel preserves
matching names and positions where possible. Windows from a removed workspace
move to the nearest remaining workspace.

Changing to a dynamic model keeps active and populated workspaces, then restores
the required empty workspace.

## Workspace axis

Set the workspace arrangement per output:

```toml
[output.DP-1]
workspace_axis = "horizontal"
```

| Value | Behavior |
| --- | --- |
| `"vertical"` | Workspaces stack top to bottom; scrolling layouts run left to right. |
| `"horizontal"` | Workspaces sit side by side; scrolling layouts run top to bottom. |

A three-finger swipe along the axis changes workspaces. A swipe across it moves
the scrolling strip. The overview follows the same arrangement.

## Workspace selectors

Actions such as `workspace-switch` and `window-move-to-workspace` use these
forms:

- Bare digits from `1` to `64` select a one-based position.
- Other text selects an exact, case-sensitive name.
- Double quotes force name lookup for an all-digit name.
- `/output` restricts either form to one output.

```text
workspace-switch:3
workspace-switch:3/DP-2
workspace-switch:CHAT/DP-2
workspace-switch:"3"/DP-2
```

Duplicate names resolve on the pointer-preferred output when possible.
Otherwise the selector is ambiguous. On a dynamic output, a numeric position
beyond the current count selects the last workspace.

## Inspect workspace state

Run `umbriel workspaces` to list workspaces and their effective layout:

```text
* DP-1: 1 [scrolling] (focused)
  DP-1: 2 [dwindle]
* DP-2: WEB [master]
```

Use `umbriel workspaces --json` for scripts. Each entry includes its stable
`id`, display `name`, one-based `index`, `output`, active and focused states,
and effective `layout`.

## Global workspace settings

```toml
[workspaces]
back_and_forth = true
empty_above = false
```

| Key | Default | Description |
| --- | --- | --- |
| `back_and_forth` | `false` | Selecting the active workspace returns to the previous one on that output. |
| `empty_above` | `false` | Keep a leading empty workspace on dynamic outputs. |

## Workspace rules

`[[workspace]]` entries customize one workspace. Select it with exactly one of
`name` or `index`, and optionally restrict it to an output:

```toml
[[workspace]]
name = "VIDEO"
output = "DP-2"
layout.mode = "dwindle"
layout.gap = 4
```

On dynamic outputs, `name` also creates a persistent named workspace. `index`
follows whichever workspace currently occupies that position. On static
outputs, rules only customize members already present in the configured
inventory.

### How settings are combined

Settings apply from least to most specific:

1. Global `[layout]`
2. Matching output scrolling extent
3. Matching workspace rule without `output`
4. Matching workspace rule with `output`

Later values replace earlier ones. Strut edges are resolved independently, so a
rule can override only `layout.struts.top`.

### Available fields

| Key | Description |
| --- | --- |
| `name` | Select an exact name and create it on matching dynamic outputs. |
| `index` | Select a current one-based position from 1 to 64. |
| `output` | Restrict the rule to a connector or monitor name. |
| `layout.mode` | Use `"scrolling"`, `"dwindle"`, or `"master"`. |
| `layout.gap` | Set the window gap. |
| `layout.struts.{left,right,top,bottom}` | Reserve signed logical pixels at each edge. |
| `layout.extent_presets` | Set extent-cycle fractions. |
| `layout.new_exits_fullscreen` | Choose which arriving windows exit fullscreen; see [Leaving fullscreen](layout.md#leaving-fullscreen). |
| `layout.scrolling.default_extent_fraction` | Set the initial scrolling-column extent. |
| `layout.scrolling.center_underfull_strip` | Center or start-align an underfull strip. |
| `layout.scrolling.center_focused` | Control when focus changes center a column. |
| `layout.master.position` | Place the master area left, right, or center. |
| `layout.master.default_width_fraction` | Set the master-area fraction. |
| `layout.master.new_on_top` | Place new stack windows at the top or bottom. |
| `layout.master.new_becomes_master` | Give the master slot to new windows. |
| `layout.dwindle.preserve_split` | Keep split directions fixed. |

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
