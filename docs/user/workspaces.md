# Workspaces

Choose a workspace model, inspect its state, and customize layout per workspace.

## Choose a workspace model

Each output can use dynamic or static workspaces, up to 64 including empty ones.
At the limit, a workspace transfer reuses an empty destination on a dynamic
output and is rejected when it would require another workspace.

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`. Without any named entries, the
output starts with one empty anonymous workspace displayed as `"1"`. With
`empty_above = true`, it starts with distinct leading and trailing anonymous
workspaces displayed as `"1"` and `"2"`.

When the last workspace gains a window, Umbriel adds another empty workspace
while the output remains below the 64-workspace limit. With
`empty_above = true`, it likewise adds a new leading empty workspace when the
first workspace gains a window and capacity remains.

After you leave any other empty anonymous workspace, Umbriel removes it unless
it is still active. The remaining anonymous workspaces are renamed and
reindexed from `1` in their current order. If you switch to a workspace number
beyond the current count, Umbriel uses the last workspace.

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

#### Persistent names in a dynamic inventory

A `[[workspace]]` entry selected by `name` materializes a persistent named
workspace on every matching dynamic output. An unscoped entry applies
independently to every dynamic output, while `output` limits it to one matching
connector or monitor name:

```toml
# Every dynamic output gets its own case-sensitive CHAT workspace.
[[workspace]]
name = "CHAT"
layout.mode = "master"

# Only DP-1 gets STATS.
[[workspace]]
name = "STATS"
output = "DP-1"
layout.mode = "dwindle"
```

Names are local to an output and case-sensitive, so separate outputs can each
contain `"CHAT"`, while `"chat"` is a different name. Named workspaces keep
their name and identity as anonymous workspaces around them are pruned and
renumbered. They also remain when empty. Below the output limit, a trailing
empty anonymous workspace is still kept after them, and `empty_above = true`
keeps a distinct leading empty anonymous workspace before them. At the limit,
an occupied edge remains without a replacement sentinel until a slot becomes
available.

The 64-workspace output limit includes those sentinels. A dynamic output can
therefore declare at most 63 distinct names, or 62 when `empty_above = true`.
If a live output is already full of occupied workspaces, a newly added name
waits until an ordinary anonymous workspace becomes empty. Umbriel does not
repurpose an occupied workspace or either empty sentinel.

A connector-scoped rule and a monitor-scoped rule can both match the same live
output. Keep their combined distinct names within the same limit. If their
combined inventory exceeds it, Umbriel preserves the sentinels, omits the last
matching declarations on that output, and logs an error naming the count.

Below the output limit, new declarations added during a live reload enter near
the top, after the leading empty workspace when one is enabled. At the limit,
a declaration can instead claim an eligible empty anonymous workspace in
place. Existing named workspaces retain their identity and relative order, so
merely reordering declarations does not move them. Their numeric positions can
still shift when a new workspace is inserted before them. Removing the last
matching declaration for a name clears it. The workspace then resumes ordinary
dynamic lifecycle: a populated or active workspace remains anonymous, while
another empty inactive workspace can be pruned.

### Static workspaces

Set `workspaces` to a number or an ordered list of names. Umbriel creates
exactly those workspaces and keeps them when they are empty.

A number creates anonymous fixed positions with generated numeric labels. A
string list creates explicitly named workspaces. For example, `workspaces = 5`
has no workspace named `"3"`, while `workspaces = ["3"]` does.

Actions use typed name and position selectors independently of the inventory
model; see [Workspace selectors](#workspace-selectors).
On a static output, `[[workspace]]` entries only customize members already in
this exact inventory. They do not add or remove static workspaces.

When `workspace-move-to-output-*` targets a static output, it reuses the
highest empty configured workspace without changing that workspace's identity
or name. The action is rejected if every configured workspace is occupied, so
the fixed inventory never grows or merges two occupied workspaces.

```toml
[output.DP-1]
workspaces = 5

[output.DP-2]
workspaces = ["WEB", "CHAT", "VIDEO"]
```

### Change workspaces on reload

Workspace changes apply when you save a valid configuration. For static
name lists, Umbriel first matches existing workspaces by name and then by
position. Static counts match by position. Windows from a removed workspace
move to the nearest remaining one.

For dynamic workspaces, Umbriel reconciles persistent names as described above,
then prunes and renumbers anonymous workspaces. Switching to dynamic workspaces
keeps populated and active workspaces, materializes matching named entries, and
ensures an empty anonymous workspace at the end while capacity permits. If 64
occupied survivors fill the output, a missing edge sentinel waits until a slot
becomes available.

Other output and layout settings are refreshed during a reload as well.

## Workspace axis

Each output arranges its workspaces along one axis. `workspace_axis` selects it
per output and accepts `"vertical"` (the default) or `"horizontal"`:

```toml
[output.DP-1]
workspace_axis = "horizontal"
```

Vertical workspaces stack top to bottom: switching slides the previous workspace
down and the next one up, and the overview shows one vertical filmstrip. With
`"horizontal"`, workspaces sit side by side, switching slides them left and
right, and the overview filmstrip runs horizontally.

The axis also selects the scrolling strip axis, which is always perpendicular to
it. Vertical workspaces scroll horizontally and horizontal workspaces scroll
vertically; see [Scrolling layout](layout.md#scrolling-layout). Dwindle and
master workspaces follow their output's axis for switching and the overview,
like every other layout.

A three-finger swipe along the axis switches workspaces, and a swipe across it
scrolls the strip. In the overview, wheel moves vertically and Shift+wheel moves
horizontally in discrete steps. Along the workspace axis it switches workspaces;
across that axis it selects and reveals scrolling columns in the hovered preview.

An axis change on reload applies to every workspace on the output. Any live
swipe, strip drag or tiled resize is settled first, and an open overview closes.

## Workspace selectors

Actions that take a workspace argument, such as `workspace-switch`,
`window-move-to-workspace`, and `column-move-to-workspace`, resolve it with
these rules:

- Bare ASCII digits from `1` to `64` always select that 1-based position on the
  pointer-preferred output. They are never interpreted as a name.
- Other non-empty text selects an exact, case-sensitive name. A unique name can
  select its workspace on any output.
- Double-quote a selector to force name lookup. This is required for an
  all-digit name, so `"3"` means the exact name `"3"`.
- Duplicate names resolve on the pointer-preferred output when it contains that
  name. Otherwise the action reports an ambiguous selector.
- On a dynamic output, a number beyond the current workspace list selects the
  last workspace.
- Add `/output` after either form to resolve only within that output.
- A generated numeric label on an anonymous workspace is not a name.

```text
workspace-switch:3                 # Position 3 on the output under the pointer
workspace-switch:3/DP-2            # Position 3 on DP-2
workspace-switch:CHAT/DP-2         # Exact name CHAT on DP-2
workspace-switch:"3"/DP-2          # Exact name "3" on DP-2
```

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
workspace `id`, `name`, `named`, one-based `index`, `output`, `active`,
`focused`, and `layout`. `named` is true for members of a static string list
and for persistent named members of a dynamic inventory. It is false for
anonymous positions created by a static count or dynamic inventory. `active`
is per output, so more than one workspace can be active. `focused` is true for
at most one workspace. The `layout` value is the current effective mode,
including an override made with `workspace-set-layout`.

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

`[[workspace]]` entries customize workspace layout. On a dynamic output, a
`name` entry also declares a persistent named member, while an `index` entry
only follows an anonymous or named workspace currently at that position. On a
static output, `index` works with either inventory form, while `name` matches
only a member from an explicit string list.

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer from 1 to 64). These TOML types remain distinct. `name = "2"`
declares or selects the workspace literally named `"2"`, while `index = 2`
follows the workspace at position 2. Workspace names are case-sensitive. An
optional `output` restricts the rule to a case-insensitive connector or monitor
name from `umbriel outputs`.

### How settings are combined

Workspace layout settings are applied in this order:

1. The base `[layout]` settings.
2. The matching output's
   `layout.scrolling.default_width_fraction`, when configured.
3. A matching `[[workspace]]` rule without an `output`.
4. A matching `[[workspace]]` rule for the selected output.

Later steps take precedence. An unscoped name entry materializes one independent
workspace on every dynamic output, and an output-scoped entry can override its
layout on the matching output. Index rules are resolved again after anonymous
workspaces are created, removed, or renumbered. The output step applies only to
`default_width_fraction`; other layout fields pass directly from the global
settings to workspace rules.

Strut edges are resolved independently. A rule that sets only
`layout.struts.top` inherits the other three edges from earlier steps.

### Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by case-sensitive workspace name. Also materializes a persistent member on matching dynamic outputs (mutually exclusive with `index`). |
| `index` | int | Select by current 1-based position from 1 to 64 without creating a workspace (mutually exclusive with `name`). |
| `output` | string | Restrict to a connector or monitor name. |
| `layout.mode` | string | `"scrolling"`, `"dwindle"`, or `"master"`. |
| `layout.gap` | int | Gap in pixels (0-500). |
| `layout.struts.{left,right,top,bottom}` | int | Signed logical pixels reserved at each edge of the normal tiled layout (-65535 to 65535). Positive values shrink the area and negative values expand it. |
| `layout.width_presets` | float array | Fractions used by the width-cycle and height-cycle actions in every layout. |
| `layout.scrolling.default_width_fraction` | float | Optional initial scrolling lane extent (0.1-1.0). It overrides the global and matching output values. When omitted at every level, the client chooses its initial logical extent. Reloading a default does not resize existing columns. |
| `layout.scrolling.center_underfull_strip` | bool | Center the complete strip whenever it is narrower than the viewport. Disable to left-align underfull strips. |
| `layout.scrolling.center_focused` | string | When a focus change centers the newly focused column. `"never"` only scrolls far enough to reveal it, `"always"` centers it, and `"on_overflow"` centers it when it cannot share the viewport with the neighboring column on the side focus came from. |
| `layout.master.position` | string | Side occupied by the master area: `"left"`, `"right"`, or `"center"` between two stacks. |
| `layout.master.default_width_fraction` | float | Master area fraction when both areas exist (0.1-0.9). |
| `layout.master.new_on_top` | bool | Place newly opened windows at the top of the stack. Disable to place them at the bottom. |
| `layout.master.new_becomes_master` | bool | Give the master slot to each new window and move the last master row to the stack top. |
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
layout.scrolling.center_focused = "always"

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
