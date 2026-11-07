# Layer Rules

Layer rules apply visual effects to layer-shell surfaces such as bars, launchers, and notifications. Run `umbriel layers` to list active namespaces.

```toml
[[layer_rule]]
match.namespace = "^noctalia-bar-"
blur = true
blur_ignore_alpha = 0.5
blur_popups = true
```

## Matching

| Selector           | Type  | Description                                                |
| ------------------ | ----- | ---------------------------------------------------------- |
| `match.namespace`  | regex | Match the layer surface namespace.                         |
| `match.layer`      | enum  | Match `"background"`, `"bottom"`, `"top"`, or `"overlay"`. |
| `match.at_startup` | bool  | Match during or after the first 60 seconds.                |

Regular expressions match any part of a namespace. Use `^` and `$` for an exact match. The selector takes either one pattern or an array of them, and any one of them matching is enough.

Selectors are optional: a rule without them matches every layer surface, and every matching rule contributes its values.

## Effects

| Key                 | Type  | Description                                                                |
| ------------------- | ----- | -------------------------------------------------------------------------- |
| `dim_behind`        | float | Darken content below this layer on its output; `0.0`–`1.0`, default `0.0`. |
| `blur`              | bool  | Enable/disable blur for the layer surface.                                 |
| `blur_popups`       | bool  | Enable/disable blur for descendant XDG popups.                             |
| `blur_ignore_alpha` | float | Skip blur below an alpha threshold.                                        |
| `blur_optimized`    | bool  | Override the global optimized-blur choice.                                 |

Layer-shell blur is off by default. Every matching rule contributes its settings, and later values take precedence.

Optimized blur samples the cached wallpaper blur, so a surface that opens over windows shows the wallpaper through it instead of the windows. Set `blur_optimized = false` on such surfaces (panels, docks, notifications) to blur the live content behind them; it costs GPU time on every frame they are visible and something behind them changes. Surfaces that only cover the wallpaper, such as a bar, keep the cheaper optimized blur.

## Keyboard focus

A layer surface declares its own keyboard interactivity through the layer-shell protocol; no rule overrides it.

| Interactivity | Behavior                                                                             |
| ------------- | ------------------------------------------------------------------------------------ |
| `none`        | Never receives keyboard focus. Clicking the surface leaves the focused window alone. |
| `on_demand`   | Takes focus when mapped; clicking a window or using a focus action moves focus away. |
| `exclusive`   | Keeps keyboard focus; windows receive no keys and focus actions cannot leave it.     |

Launchers and panels with search fields commonly use `on_demand`.

While a fullscreen window covers an entire output, top-layer surfaces on that output remain underneath it and cannot receive keyboard focus, including with `exclusive` interactivity. When fullscreen content scrolls partly or completely off-screen, top layers are drawn above it and can receive keyboard input. Overlay-layer surfaces can still take focus over fullscreen. Once a top-layer surface is uncovered, an `exclusive` one takes keyboard focus back; an `on_demand` one waits to be clicked.

## Dim behind

```toml
[[layer_rule]]
match.namespace = "^vicinae$"
dim_behind = 0.5
```

The dim covers the layer's output, below that layer in the scene stack. The layer, its popups, and surfaces above it retain their normal brightness. It follows the layer's opening fade and clears when the layer unmaps. It does not require keyboard focus and does not intercept pointer input. Several dimming surfaces stack their dimming; set `dim_behind = 0.0` in a later matching rule to disable it.
