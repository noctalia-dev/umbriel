# Animation

Animation settings live in the top-level `animation` section. `duration_ms` and
`curve` set defaults for every event when present; a nested event can override
either value. The master switch makes every transition instant. Each event also
has its own switch.

```toml
[animation]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.windows_in]
enabled = true
duration_ms = 150
curve = "easeout"
style = "popin"       # "popin", "zoom", "slide", "fade", or "none"
scale = 0.85          # 0.1-1.0, used by "popin"

[animation.windows_out]
enabled = true
duration_ms = 150
curve = "easeout"
style = "fade"        # "fade" or "slide"

[animation.windows_move]
enabled = true
duration_ms = 250
curve = "snappy"

[animation.workspaces]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.overview]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.scratchpad]
enabled = false
duration_ms = 250
curve = "easeout"
direction = "top"         # "top", "bottom", "left", "right"
style = "slide"           # "slide", "slidefade", "popin", "fade"
dim = 0.5                 # 0.0-1.0
blur = false              # requires appearance.blur.enabled
scale = 0.0               # 0 preserves geometry; 0.1-1.0 sizes and centers on entry
maximize = false          # maximize to edges on entry
maximize_to_edges = false # fill usable area edge-to-edge
fullscreen = false        # fullscreen on entry
suspend_hidden = true     # suspend client frame scheduling while closed

[animation.border]
enabled = false
duration_ms = 250
curve = "easeout"

[animation.dim_unfocused]
enabled = false
duration_ms = 250
curve = "easeout"
dim = 0.0             # 0.0-1.0

[animation.layers]
enabled = false
duration_ms = 250
curve = "easeout"
```

## Defaults

| Key           | Type   | Default   | Description                                                                 |
| ------------- | ------ | --------- | --------------------------------------------------------------------------- |
| `enabled`     | bool   | `true`    | Master switch. When false, every animation transition is instant.           |
| `duration_ms` | int    | `250`     | Default duration for all events when explicitly set (1-10000 milliseconds). |
| `curve`       | string | `easeout` | Default curve for all events when explicitly set.                           |

## Event tables

Each event table accepts `enabled`, `duration_ms`, and `curve`. The following
fields are specific to individual event tables:

| Table                      | Additional fields                                                                                                  | Transition                                          |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------- |
| `[animation.windows_in]`   | `style` (`popin`, `zoom`, `slide`, `fade`, or `none`); `scale` (0.1-1.0, for `popin`)                            | Window open.                                        |
| `[animation.windows_out]`  | `style` (`fade` or `slide`)                                                                                       | Window close, using a scene snapshot.               |
| `[animation.windows_move]` | None                                                                                                               | Window move and resize.                             |
| `[animation.workspaces]`   | None                                                                                                               | Workspace switch.                                   |
| `[animation.overview]`     | None                                                                                                               | Overview open, close, and row settling.             |
| `[animation.scratchpad]`   | `direction` (`top`, `bottom`, `left`, `right`); `style` (`slide`, `slidefade`, `popin`, `fade`); `dim` (0.0-1.0); `blur`; `scale` (0.0-1.0); `maximize`; `maximize_to_edges`; `fullscreen`; `suspend_hidden` | Scratchpad show, hide, and backdrop. |
| `[animation.border]`       | None                                                                                                               | Focus-ring color transition in OkLab color space.   |
| `[animation.dim_unfocused]` | `dim` (0.0-1.0)                                                                                                 | Unfocused-window opacity. `dim = 0` disables it.    |
| `[animation.layers]`       | None                                                                                                               | Layer-shell surface map and unmap fades.            |

An event's `enabled = false` makes only that transition instant. Scratchpad
`dim` and `blur` remain active, without a fade, when animation is disabled.
Scratchpad `scale`, `maximize`, `maximize_to_edges`, and `fullscreen` apply when a window enters the
scratchpad.

## Curves

Each curve accepts a built-in name such as `linear`, `ease`, `easeout`,
`snappy`, `bounce`, or `elastic`; a cubic bezier string
`"x1,y1,x2,y2"`; or a spring string `"spring: damping,stiffness"`. Bezier x
coordinates must be between 0 and 1. Spring damping must be between 0.01 and 5,
and stiffness between 1 and 1000.

Custom named curves can be registered once and reused by name:

```toml
[animation.beziers]
myBezier = [0.05, 0.9, 0.1, 1.05]

[animation.springs]
myBounce = { damping = 0.5, stiffness = 200 }
```

Then reference them as `curve = "myBezier"` or `curve = "myBounce"` in any
event section.
