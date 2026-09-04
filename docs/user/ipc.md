# IPC

Umbriel listens on a single UNIX socket. The same connection answers one-shot
queries and, on request, becomes an event stream. `UMBRIEL_SOCKET` holds the
path; without it, clients derive
`$XDG_RUNTIME_DIR/umbriel-$WAYLAND_DISPLAY.sock`.

Every request is one JSON object on one line. Every reply is one JSON object on
one line: `{"ok": …}` or `{"err": "…"}`. The `umbriel` subcommands in
[Actions](actions.md) and `umbriel --help` are thin clients over this socket, so
anything they do is available to a script.

```sh
printf '{"cmd":"workspaces"}\n' | socat -t 5 STDIO "$UMBRIEL_SOCKET"
```

## Queries

| Request | CLI | Reply |
| ------- | --- | ----- |
| `{"cmd":"windows"}` | `umbriel windows --json` | window list with ids, app ids, titles, geometry, workspace ids |
| `{"cmd":"workspaces"}` | `umbriel workspaces --json` | workspace list with names, indices, outputs, active/focused flags, layout modes |
| `{"cmd":"submap"}` | `umbriel submap --json` | active keybind submap, or `null` |
| `{"cmd":"layers"}` | `umbriel layers --json` | layer-shell surfaces |
| `{"cmd":"msg","arg":"<action>"}` | `umbriel msg <action>` | runs an [action](actions.md) |

A connection with no subscription closes once its replies are written, and a
connection that sends nothing is dropped after a second.

## Event stream

```json
{"cmd":"subscribe","events":["workspaces","windows"]}
```

The connection stays open. Umbriel first writes the current state of every
subscribed family, one line each, then a line whenever that family changes. Each
line is `{"event":"<family>","data":…}`.

| Family | Fires on |
| ------ | -------- |
| `theme` | color or corner-radius changes from a config reload; see [payload](#theme-payload) |
| `overview` | the overview opening or closing |
| `keyboard_layout` | layout switches; skipped in the initial state when no keyboard exists |
| `windows` | window open, close, focus, title, app id, geometry, workspace, floating state |
| `workspaces` | layout mode, activation, names, indices, and workspace or output membership |
| `submap` | the active keybind submap changing; `null` is the default context |

Subscribing to an unknown family answers
`{"err":"unknown subscription event: <name>"}` and closes, so a typo fails
immediately rather than waiting on a stream that never opens.

Each family is a **full snapshot**, not a delta: the payload is the same
structure the matching query returns. A client replaces its state with the
newest line and never reassembles it from increments, so a stream cannot
desync. Window and workspace updates are coalesced per event-loop iteration,
and a payload identical to the last one sent for that family is dropped, so a
resize that ends where it started wakes nobody.

Events are the only way to follow a workspace's **layout mode**: the
`ext-workspace-v1` protocol publishes workspace lists, names, and activation to
any Wayland client, but it has no concept of a layout, and Umbriel selects
scrolling, dwindle, or master per workspace.

### Theme payload

The `theme` payload mirrors the color configuration. Every color is an
`#RRGGBBAA` string, and `corner_radius` is an integer count of logical pixels.
The example is wrapped for readability; the stream writes it as one line.

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
    "scratchpad_focused":"#E5C07BFF",
    "scratchpad_unfocused":"#5C4A2AFF",
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

The flat keys, the `border` object, and the `overview` object carry `[colors]`,
`[colors.border]`, and `[colors.overview]` as written in the configuration, and
`corner_radius` carries `appearance.corner_radius`. See
[Appearance](appearance.md#colors) for what each color paints.

### From the command line

```sh
umbriel subscribe workspaces
umbriel subscribe workspaces,windows
umbriel subscribe submap
```

One JSON line per event on stdout, flushed as it arrives, until the compositor
exits or the reader closes the pipe. This is the same stream as the socket
request, so a bar, a status script, or a plugin can consume it without a socket
library:

```sh
umbriel subscribe workspaces | while read -r line; do
  jq -r '.data[] | select(.focused) | "\(.output) \(.name) \(.layout)"' <<< "$line"
done
```

## Inspection commands

`umbriel outputs`, `umbriel color`, `umbriel tearing`, `umbriel layers`, and
`umbriel keyboard-layouts` print human-readable state; each takes `--json` for
the machine-readable form. `umbriel validate` checks a config file without a
running compositor.
