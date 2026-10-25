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

| Key                 | Type  | Description                                    |
| ------------------- | ----- | ---------------------------------------------- |
| `blur`              | bool  | Enable/disable blur for the layer surface.     |
| `blur_popups`       | bool  | Enable/disable blur for descendant XDG popups. |
| `blur_ignore_alpha` | float | Skip blur below an alpha threshold.            |
| `blur_optimized`    | bool  | Override the global optimized-blur choice.     |

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

While a fullscreen window is showing on an output, top-layer surfaces on that output remain underneath it and cannot receive keyboard focus, including with `exclusive` interactivity. Overlay-layer surfaces can still take focus. Once the fullscreen window leaves, an `exclusive` top-layer surface takes keyboard focus back; an `on_demand` one waits to be clicked.
