# IPC

Umbriel exposes a local UNIX socket for queries, actions, and event
subscriptions. Most users should use the `umbriel` command rather than connect
to the socket directly.

`UMBRIEL_SOCKET` contains the socket path. Without it, use
`$XDG_RUNTIME_DIR/umbriel-$WAYLAND_DISPLAY.sock`.

Each request and reply is one JSON object per line:

```sh
printf '{"cmd":"workspaces"}\n' | socat -t 5 STDIO "$UMBRIEL_SOCKET"
```

Replies use `{"ok": ...}` or `{"err": "..."}`.

## Queries

| Request | CLI |
| --- | --- |
| `{"cmd":"effects"}` | `umbriel effects --json` |
| `{"cmd":"windows"}` | `umbriel windows --json` |
| `{"cmd":"workspaces"}` | `umbriel workspaces --json` |
| `{"cmd":"submap"}` | `umbriel submap --json` |
| `{"cmd":"layers"}` | `umbriel layers --json` |
| `{"cmd":"outputs"}` | `umbriel outputs --json` |
| `{"cmd":"msg","arg":"<action>"}` | `umbriel msg <action>` |

Window entries include IDs, application identity, process ID, geometry,
output, workspace, and scratchpad membership. X11 windows report the process ID
they publish as `_NET_WM_PID`, or `-1` when they publish none. A member of a
[tab group](layout.md#tab-groups) reports `tabbed`, its place among the group's
tabs as `tab_index` (from 0, `-1` outside a group), and `tab_hidden` while its
group shows another tab; the plain listing marks it `tab` instead of
`tile`.

Window state follows what Umbriel has decided, ahead of the client's redraw, the
same way geometry reports layout targets:

| Field | Meaning |
| --- | --- |
| `floating` | Outside the tiled layout |
| `fullscreen` | Fullscreen |
| `maximized` | Maximized by `window-toggle-maximize` (a full-width column for a tiled window) or by maximize-to-edges |
| `maximized_to_edges` | Maximized without gaps, struts, or borders |
| `pinned` | Pinned above other windows on every workspace of its output |
| `visible` | On its output's active workspace (or pinned), not a hidden tab, and not in a hidden scratchpad; it may still be scrolled out of view |
| `parent` | ID of the window the client named as its parent, or `""` |
| `column`, `row` | Place in a scrolling strip, from 0; `-1` for floating windows and other layouts |

Workspace entries include a stable ID, display name, index, output, layout,
occupancy, and active and focused states. Use the `named` boolean instead of
guessing from the display name; an explicitly named workspace may still be
called `"2"`. `window_count` counts its mapped windows, `focused_window` is the
ID of the window it last focused (`""` when none), `urgent` is set while any of
its windows is urgent, and `layout_override` is set while a runtime
`workspace-set-layout` replaces the configured layout.

Layer entries describe layer-shell surfaces such as panels, launchers, and
wallpapers: `layer`, `namespace`, `output`, `mapped`, `pid`,
`keyboard_interactivity` (`none`, `exclusive`, or `on_demand`), `focused` while
the surface holds the keyboard, `exclusive_zone`, `anchor` (the anchored edges),
`margin`, and the arranged geometry `x`, `y`, `w`, and `h` in layout
coordinates.

Output entries list every connected output, including disabled ones: `name`,
`description`, `make`, `model`, `serial`, `config_name` (the
[monitor identity](outputs.md), or `null` when the display reports none),
`physical_size`, `enabled` (part of the desktop), `powered` (enabled and not
switched off by DPMS), `focused`, `active_workspace` (its ID), `position`,
`logical_size`, `usable_area` (the part layer-shell exclusive zones leave, in
layout coordinates), `transform`, `scale`, `adaptive_sync` (`null` when the
output cannot use variable refresh), `hdr_active`, and `modes`, each with
`width`, `height`, `refresh_mhz`, `preferred`, and `current`. A disabled output
reports a zero `logical_size` and `usable_area`.

## Effect inspection

`umbriel effects` prints tables; `--json` exposes `presets`, `pools`, `cursor`,
and `owners`. Presets and pools keep their configuration declaration order,
including includes. Each preset has `name`, `kind`, and current-source `state`:
`inert` means missing or empty source, `unreferenced` means no registry entry,
`failed` means compilation failed, and `compiled` means a usable program.
Border presets also report their configured `overlay`.

Pools have `name`, `kind`, `policy`, and ordered `members`, each with `name`
and `held`. Holds count unsuppressed mapped windows, present outputs (including
disabled outputs), and the session cursor, independently of visibility or focus.
Owners list mapped windows, sorted by stable `id`, followed by present outputs,
sorted by `name`. Window owners have `type`, `id`, `app_id`, and `slots` with
`border` and `window`; output owners have `type`, `name`, and `slots.screen`.
The single cursor appears separately in `cursor`.

Each slot reports `name` (the underlying selected preset), `pool` (empty for a
plain preset), `source` (`default`, `rule`, or `runtime`), and `suppressed`.
Border slots also have `overlay`. An empty pool retains its pool name with an
empty selected name. Suppressed and failed selections retain their names.
`windows --json` and `subscribe windows` expose these same slot shapes as
`border_effect` and `window_effect`; the window text table is unchanged.

Inspection never picks, compiles, or binds an effect. Use `effects`, not
`outputs`, for screen selections.
See [Effects](effects.md) and [Actions](actions.md) for configuration and runtime
changes.

## Event stream

Subscribe with:

```json
{"cmd":"subscribe","events":["workspaces","windows"]}
```

The connection first receives the current state of each family, then a new
snapshot whenever that family changes:

```json
{"event":"workspaces","data":[]}
```

| Family | Changes reported |
| --- | --- |
| `theme` | Colors and corner radius |
| `overview` | Overview opened, or started closing |
| `keyboard_layout` | Active keyboard layout |
| `windows` | Window identity, geometry, focus, state, visibility, parent, workspace, scratchpad, or effect selection |
| `workspaces` | Inventory, layout, activity, occupancy, window count, focused window, urgency, output, or focus |
| `submap` | Active keybind submap |
| `screencast` | Manual target and focus-following screencast commands |
| `layers` | Layer-shell surfaces appearing, mapping, closing, moving, resizing, or taking and losing the keyboard |
| `outputs` | Outputs appearing or leaving, enablement, power, mode, position, scale, transform, variable refresh, HDR, usable area, focus, or active workspace |

Payloads are full snapshots rather than deltas. Replace local state with the
newest event instead of trying to merge increments. Identical consecutive
payloads are omitted.

An unknown family returns an error and closes the subscription.

### Screencast payload

The `screencast` event carries a monotonically increasing serial and one of six commands:

```json
{"event":"screencast","data":{"serial":12,"kind":"window","identifier":"window-id"}}
{"event":"screencast","data":{"serial":13,"kind":"output","output":"DP-1"}}
{"event":"screencast","data":{"serial":14,"kind":"follow_window"}}
{"event":"screencast","data":{"serial":15,"kind":"follow_output"}}
{"event":"screencast","data":{"serial":16,"kind":"follow_stop"}}
{"event":"screencast","data":{"serial":17,"kind":"clear"}}
```

Every command advances the serial, even when it selects the same source or mode again. Portal backends use that edge
so a newly authorized manual stream stays empty until the user performs another target action.

### Theme payload

The `theme` event mirrors `[colors]`, `[colors.border]`,
`[colors.overview]`, and `appearance.corner_radius`:

```json
{"event":"theme","data":{
  "background":"#141419FF",
  "text_primary":"#E8E8EAFF",
  "text_muted":"#8A8A92FF",
  "accent_primary":"#7AA3FFFF",
  "accent_secondary":"#F5C96BFF",
  "warning":"#F5C96BFF",
  "error":"#FF6B6BFF",
  "insert_hint":"#7FC8FF80",
  "backdrop":"#000000FF",
  "shadow":"#0000007F",
  "border":{
    "focused":"#7AA3FFFF",
    "unfocused":"#292933FF",
    "outer":"#1A1A1FFF"
  },
  "overview":{
    "background_tint":"#10101430",
    "workspace_background":"#00000044",
    "badge":"#7AA3FFFF"
  },
  "corner_radius":10
}}
```

See [Appearance](appearance.md#colors) for the meaning of each value.

### From the command line

The CLI exposes the same event stream:

```sh
umbriel subscribe workspaces
umbriel subscribe workspaces,windows
umbriel subscribe submap
umbriel subscribe screencast
```

It writes one JSON line per event until Umbriel exits or the reader closes:

```sh
umbriel subscribe workspaces |
  jq -r '.data[] | select(.focused) | "\(.output) \(.name) \(.layout)"'
```

## Audio input

The bundled PipeWire helper starts automatically for visible audio effects (see
[Audio input](effects.md#audio-input)). An external analyser can override it and
supply one shared level to [effect shaders](effects.md#audio-input).
Keep a dedicated connection open and send complete measurements:

```json
{"cmd":"effect-audio","version":1,"level":0.65}
```

`version` must be integer `1`; `level` must be a finite number from 0 to 1.
No other fields are accepted. Each request, including its newline, is limited
to 256 bytes. The first external measurement replaces the automatic helper and
claims the feed; another external producer receives an error while that
connection owns it. Audio connections accept only
`effect-audio` requests.

Wait for `{"ok":true}` before sending another measurement. Send at most 60 per
second and replace unsent measurements with the latest value. An acknowledgment
means accepted, not displayed. Keep sending fresh measurements during silence;
zero means silent and available. Gain, smoothing, and source selection belong
to the analyser. Umbriel does not start custom producers; its automatic helper
runs separately.

Disconnect, malformed or oversized input, or 250 ms without a measurement closes
the feed and clears the shader input. Lock and inactive sessions also disconnect
the producer and reject audio until active and unlocked. Reconnect with a fresh
measurement to resume. Use a dedicated socket client; `umbriel msg` does not
provide this streaming interface.

The bundled `umbriel-audio` is built when PipeWire is found.
Started by hand it connects through `$UMBRIEL_SOCKET` and exits when Umbriel
drops the connection, for example on lock.

## Inspection commands

`umbriel outputs`, `umbriel color`, `umbriel tearing`, `umbriel layers`, and
`umbriel keyboard-layouts` print human-readable state. Each accepts `--json`.

## Config commands

`umbriel config validate` checks a configuration and `umbriel config schema`
lists the keys it accepts, both without a running compositor.

`umbriel config-replace <path>` loads and watches a different configuration file
in the running compositor. The path must be absolute. On success, the new file
and its includes replace the previous watch set, and later reloads use the new
file. A missing, unreadable, or invalid file returns an error and leaves the
active configuration and watch set unchanged. Warnings do not reject a replacement.

## Virtual outputs

| Request | CLI |
| --- | --- |
| `{"cmd":"output-create","arg":"<name> [mode]"}` | `umbriel output-create <name> [mode]` |
| `{"cmd":"output-destroy","arg":"<name>"}` | `umbriel output-destroy <name>` |

`output-create` replies with the new output's name. Its optional mode is in the
`WIDTHxHEIGHT[@HZ]` form. Both commands reply with an error for a name that is
invalid, already taken, or unknown, and `output-destroy` refuses outputs backed
by a real display. See [Virtual outputs](outputs.md#virtual-outputs).
