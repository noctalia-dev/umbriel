# Workspaces

This page covers workspace models, reload behavior, and per-workspace layout
rules.

## Global workspace settings

```toml
[workspaces]
back_and_forth = true
```

| Key              | Type | Default | Description                                                                                     |
| ---------------- | ---- | ------- | ----------------------------------------------------------------------------------------------- |
| `back_and_forth` | bool | `false` | Re-selecting the active workspace jumps back to the previously active workspace on that output. |

Output workspaces are dynamic by default. The workspace models and rules are
documented below.

## Choose a workspace model

Each output can use dynamic or static workspaces.

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`. The output starts with one empty
workspace named `"1"`. When the last workspace gains a window, Umbriel adds
another empty workspace.

After you leave an empty workspace, Umbriel removes it unless it is still
active. The remaining workspaces are renumbered. If you switch to a workspace
number beyond the current count, Umbriel uses the last workspace.

### Static workspaces

Set `workspaces` to a number or an ordered list of names. Umbriel creates
exactly those workspaces and keeps them when they are empty.

Workspace actions first resolve exact names globally. When no exact numeric
name exists, `workspace-switch:2` selects the second entry on the focused
output, even when that workspace has a custom name. If an exact name exists on
more than one output, the focused output wins.

```toml
[output.DP-1]
workspaces = 5

[output.DP-2]
workspaces = ["WEB", "CHAT", "VIDEO"]
```

### Change workspaces on reload

Workspace changes apply when you save a valid configuration. For static
workspaces, Umbriel first matches existing workspaces by name and then by
position. Windows from a removed workspace move to the nearest remaining one.

Switching to dynamic workspaces keeps populated and active workspaces,
renumbers them, and adds an empty workspace at the end.

Other output and layout settings are refreshed during a reload as well.

## Workspace rules

`[[workspace]]` entries customize static workspaces or numbered positions on a
dynamic output. They change layout settings but do not create workspaces.

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer from 1 to 64). An optional `output` restricts the rule to that
output.

### How settings are combined

Workspace layout settings are applied in this order:

1. The base `[layout]` settings.
2. A matching `[[workspace]]` rule without an `output`.
3. A matching `[[workspace]]` rule for the selected output.

Later steps take precedence. On dynamic outputs, rules match workspace names
and numbered positions as those workspaces are created or removed.

### Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by workspace name (mutually exclusive with `index`). |
| `index` | int | Select by 1-based position from 1 to 64 (mutually exclusive with `name`). |
| `output` | string | Restrict to this output. |
| `layout.mode` | string | `"scrolling"` or `"dwindle"`. |
| `layout.gap` | int | Gap in pixels (0-500). |
| `layout.width_presets` | float array | Widths used by the width-cycle action in both layouts. |
| `layout.scrolling.default_width_fraction` | float | Optional initial scrolling lane extent (0.1-1.0). When omitted globally and for the workspace, the client chooses its initial logical extent. |
| `layout.scrolling.center_underfull_strip` | bool | Center the complete strip whenever it is narrower than the viewport. Disable to left-align underfull strips. |
| `layout.scrolling.direction` | string | `"horizontal"` or `"vertical"` scroll axis. |

### Examples

```toml
# Dwindle layout for the VIDEO workspace on DP-2
[[workspace]]
output = "DP-2"
name = "VIDEO"
layout.mode = "dwindle"

# Scrolling for CHAT, dwindle for STATS, both on HDMI-A-1
[[workspace]]
output = "HDMI-A-1"
name = "CHAT"
layout.mode = "scrolling"
layout.scrolling.direction = "vertical"

[[workspace]]
output = "HDMI-A-1"
name = "STATS"
layout.mode = "dwindle"

# Customize workspace position 4 on DP-1
[[workspace]]
index = 4
output = "DP-1"
layout.gap = 0
layout.scrolling.default_width_fraction = 0.667
```
