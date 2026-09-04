# Workspaces

Choose a workspace model, inspect its state, and customize layout per workspace.

## Choose a workspace model

Each output can use dynamic or static workspaces.

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`. By default, the output starts with
one empty workspace named `"1"`. With `empty_above = true`, it starts with
distinct leading and trailing empty workspaces named `"1"` and `"2"`.

When the last workspace gains a window, Umbriel adds another empty workspace.
With `empty_above = true`, it also adds a new leading empty workspace when the
first workspace gains a window.

After you leave any other empty workspace, Umbriel removes it unless it is still
active. The remaining workspaces are renumbered. If you switch to a workspace
number beyond the current count, Umbriel uses the last workspace.

`min_workspaces` sets a floor on the count for one output, so a bar or overview
can always show that many entries:

```toml
[output.DP-1]
min_workspaces = 3
```

The floor is per output and is not a ceiling. DP-1 keeps three workspaces while
all of them are empty, and a window on the third still adds a fourth. Pruning
stops once three remain. The key belongs to a dynamic output: setting it
alongside a static `workspaces` inventory is a configuration error.

### Static workspaces

Set `workspaces` to a number or an ordered list of names. Umbriel creates
exactly those workspaces and keeps them when they are empty.

Actions that take a workspace argument resolve names and numbered positions the
same way here as everywhere else; see [Workspace selectors](#workspace-selectors).

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

## Workspace selectors

Actions that take a workspace argument, such as `workspace-switch`,
`window-move-to-workspace`, and `column-move-to-workspace`, resolve it with
these rules:

- Exact names resolve globally first, including numeric names.
- A unique name selects its workspace on any output.
- Duplicate names resolve on the preferred output.
- When no exact numeric name exists, the number selects that 1-based position
  on the preferred output.
- On a dynamic output, a number beyond the current workspace list selects the
  last workspace.
- Add `/output` to target another output explicitly.

For example, `workspace-switch:3` selects a workspace named `3` when one
exists and otherwise the third workspace on the preferred output, while
`workspace-switch:CHAT/HDMI-A-1` selects `CHAT` on `HDMI-A-1`.

When `workspace-switch` targets another monitor, the cursor warps to that
monitor's center so subsequent actions continue there. Window and column moves
do the same by default. With `input.cursor.follows_focus` enabled, they warp to
the moved focused window's visible center instead.

## Inspect workspace state

Run `umbriel workspaces` to list every workspace with its output and effective
layout mode. An asterisk marks the active workspace on each output, while
`(focused)` identifies the active workspace on the output Umbriel currently
targets for actions.

```text
* DP-1: 1 [scrolling] (focused)
  DP-1: 2 [dwindle]
* DP-2: WEB [master]
```

Use `umbriel workspaces --json` for structured output. Each entry contains the
workspace `id`, `name`, one-based `index`, `output`, `active`, `focused`, and
`layout`. `active` is per output, so more than one workspace can be active.
`focused` is true for at most one workspace. The `layout` value is the current
effective mode, including an override made with `workspace-set-layout`.

For example, this prints the layout on the workspace currently targeted by
workspace actions:

```sh
umbriel workspaces --json | jq -r '.[] | select(.focused).layout'
```

## Global workspace settings

```toml
[workspaces]
back_and_forth = true
empty_above = false
```

| Key              | Type | Default | Description                                                                                     |
| ---------------- | ---- | ------- | ----------------------------------------------------------------------------------------------- |
| `back_and_forth` | bool | `false` | Re-selecting the active workspace jumps back to the previously active workspace on that output. |
| `empty_above`    | bool | `false` | Add an empty workspace at the start, in addition to the workspace at the end.                    |

Output workspaces are dynamic by default. `workspaces` and `min_workspaces` are
per-output keys documented in [Outputs](outputs.md#settings). The workspace
models and rules are documented below.


## Workspace rules

`[[workspace]]` entries customize static workspaces or numbered positions on a
dynamic output. They change layout settings but do not create workspaces.

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer from 1 to 64). An optional `output` restricts the rule to a
case-insensitive connector or monitor name from `umbriel outputs`.

### How settings are combined

Workspace layout settings are applied in this order:

1. The base `[layout]` settings.
2. The matching output's
   `layout.scrolling.default_width_fraction`, when configured.
3. A matching `[[workspace]]` rule without an `output`.
4. A matching `[[workspace]]` rule for the selected output.

Later steps take precedence. On dynamic outputs, rules match workspace names
and numbered positions as those workspaces are created or removed. The output
step applies only to `default_width_fraction`; other layout fields pass directly
from the global settings to workspace rules.

Strut edges are resolved independently. A rule that sets only
`layout.struts.top` inherits the other three edges from earlier steps.

### Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by workspace name (mutually exclusive with `index`). |
| `index` | int | Select by 1-based position from 1 to 64 (mutually exclusive with `name`). |
| `output` | string | Restrict to a connector or monitor name. |
| `layout.mode` | string | `"scrolling"`, `"dwindle"`, or `"master"`. |
| `layout.gap` | int | Gap in pixels (0-500). |
| `layout.struts.{left,right,top,bottom}` | int | Signed logical pixels reserved at each edge of the normal tiled layout (-65535 to 65535). Positive values shrink the area and negative values expand it. |
| `layout.width_presets` | float array | Fractions used by the width-cycle and height-cycle actions in every layout. |
| `layout.scrolling.default_width_fraction` | float | Optional initial scrolling lane extent (0.1-1.0). It overrides the global and matching output values. When omitted at every level, the client chooses its initial logical extent. Reloading a default does not resize existing columns. |
| `layout.scrolling.center_underfull_strip` | bool | Center the complete strip whenever it is narrower than the viewport. Disable to left-align underfull strips. |
| `layout.scrolling.center_focused` | bool | Always center the focused column, including when the setting changes on config reload. |
| `layout.scrolling.direction` | string | `"horizontal"` or `"vertical"` scroll axis. |
| `layout.scrolling.expand_single_column` | bool | Fill the viewport for a workspace's lone tiled column, subject to client size hints and viewport bounds. Disable to keep the configured/default width. |
| `layout.master.position` | string | Side occupied by the master area: `"left"` or `"right"`. |
| `layout.master.default_width_fraction` | float | Master area fraction when both areas exist (0.1-0.9). |
| `layout.master.new_on_top` | bool | Place newly opened windows at the top of the stack. Disable to place them at the bottom. |
| `layout.dwindle.preserve_split` | bool | Keep each Dwindle split direction fixed after it is created when true. |

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
[output.DP-1.layout.scrolling]
default_width_fraction = 0.5

[[workspace]]
index = 4
output = "DP-1"
layout.gap = 0
layout.struts.top = 24
layout.scrolling.default_width_fraction = 0.667
```

In the last example, new columns on DP-1 use `0.5` except on workspace position
4, where the more specific workspace rule uses `0.667`.
