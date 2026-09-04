# Layer Rules

Layer rules match layer-shell surfaces such as bars, launchers, and
notifications. The `match.namespace` selector uses an ECMAScript regular
expression.

Run `umbriel layers` to list the namespaces currently in use.

```toml
[[layer_rule]]
match.namespace = "^noctalia-(bar-[^\"]+|notification|dock|panel|attached-panel|osd|desktop-widget-[^\"]*)$"
blur = true
blur_ignore_alpha = 0.5
blur_popups = true
```

## Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.namespace` | regex | Match the layer surface namespace. |

Regular expressions match any part of a namespace. Use `^` and `$` to match
the entire namespace. A layer surface names itself when it is created, so
`match.namespace = "^$"` selects the surfaces that named themselves with an
empty string.

## Effects

| Key | Type | Description |
|-----|------|-------------|
| `blur` | bool | Enable/disable blur for the layer surface. |
| `blur_popups` | bool | Enable/disable blur for descendant XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). `0.0` blurs the entire rectangle; higher values leave transparent regions unblurred. |
| `blur_optimized` | bool | Override `appearance.blur.optimized`. A `true` value keeps the cached background blur alive on every output even when the global switch is off. |

Layer-shell blur is off by default. As with window rules, every matching rule
contributes its settings, and later values take precedence.
