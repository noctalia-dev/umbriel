# Layer Rules

Layer rules apply visual effects to layer-shell surfaces such as bars,
launchers, and notifications. Run `umbriel layers` to list active namespaces.

```toml
[[layer_rule]]
match.namespace = "^noctalia-bar-"
blur = true
blur_ignore_alpha = 0.5
blur_popups = true
```

## Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.namespace` | regex | Match the layer surface namespace. |

Regular expressions match any part of a namespace. Use `^` and `$` for an
exact match.

## Effects

| Key | Type | Description |
|-----|------|-------------|
| `blur` | bool | Enable/disable blur for the layer surface. |
| `blur_popups` | bool | Enable/disable blur for descendant XDG popups. |
| `blur_ignore_alpha` | float | Skip blur below an alpha threshold. |
| `blur_optimized` | bool | Override the global optimized-blur choice. |

Layer-shell blur is off by default. Every matching rule contributes its
settings, and later values take precedence.

## Keyboard focus

A layer surface declares its own keyboard interactivity through the layer-shell
protocol; no rule overrides it.

| Interactivity | Behavior |
|---------------|----------|
| `none` | Never receives keyboard focus. Clicking the surface leaves the focused window alone. |
| `on_demand` | Takes focus when mapped; clicking a window or using a focus action moves focus away. |
| `exclusive` | Keeps keyboard focus; windows receive no keys and focus actions cannot leave it. |

Launchers and panels with search fields commonly use `on_demand`.

While a fullscreen window is showing on an output, top-layer surfaces on that
output remain underneath it and cannot receive keyboard focus, including with
`exclusive` interactivity. Overlay-layer surfaces can still take focus. Once the
fullscreen window leaves, an `exclusive` top-layer surface takes keyboard focus
back; an `on_demand` one waits to be clicked.
