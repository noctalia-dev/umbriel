# Security

## Session locking

Umbriel keeps the current desktop visible while an `ext-session-lock-v1` client
prepares a mapped lock surface for every active output. The client has up to
three seconds to provide those surfaces. Umbriel then switches every output to
the lock scene and reports the session as locked only after each active output
has presented a secure frame.

If a client stalls or omits an output, the handoff continues after the deadline
with an opaque compositor-owned blank. Its RGB color comes from
`[colors].backdrop`; its alpha is always fully opaque. A client that exits after
the secure handoff leaves this blank in place, so the desktop cannot be exposed
by a crashed locker.

## Sandboxed Wayland clients

Umbriel supports version 1 of the
[Wayland security-context protocol](https://wayland.app/protocols/security-context-v1).
It is always enabled.

A sandbox engine can create a restricted Wayland connection and label it with
the sandbox engine, application, and instance. Umbriel then limits the
protocols available through that connection. This does not create the sandbox
or display permission prompts.

### Restricted capabilities

Restricted clients retain the protocols needed for ordinary windows, rendering,
focused input, clipboard use, output discovery, idle inhibition, and activation.
Umbriel withholds compositor-wide authority, including:

- Screen and window capture
- Virtual input and input-method ownership
- Clipboard-manager access
- Layer shell and session locking
- Gamma and output configuration
- Global workspace and window control
- Creation of nested security contexts

Trusted host services such as xdg-desktop-portal can mediate privileged
operations for a sandboxed application.

New protocols remain hidden from restricted clients until they receive a
security review.

### Per-application grants

Use `[[security_context_rule]]` when a sandboxed application genuinely needs a
protocol without a portal equivalent:

```toml
[[security_context_rule]]
match.sandbox_engine = 'org\.flatpak'
match.app_id = 'org\.example\.ClipboardManager'
allow_globals = [
  "ext_data_control_manager_v1",
  "zwlr_data_control_manager_v1",
]
```

Umbriel supports both data-control variants. Existing clipboard managers often
use the `zwlr_` variant, so grant both unless the application is known to use
only `ext_`.

| Selector | Description |
| --- | --- |
| `match.sandbox_engine` | Exact regular-expression match against the sandbox engine. |
| `match.app_id` | Exact regular-expression match against the application ID. |

`allow_globals` lists additional Wayland globals to expose. Every matching rule
contributes its values.

Selectors are optional. A rule without selectors applies to every restricted
client, so avoid broad grants. Patterns match the complete value rather than a
substring.

Rules are additive and cannot remove the base protocol set.
`wp_security_context_manager_v1` remains blocked even if listed. Changes apply
only to new connections, so restart an application after changing its grants.

Invalid entries are ignored with a configuration warning.

### Security boundary

The protocol restricts one Wayland connection. It does not restrict files,
processes, devices, networking, D-Bus, X11, or other host interfaces.

For the restriction to matter, the sandbox must expose only its restricted
Wayland socket. It must also control access to:

- Umbriel IPC through `$UMBRIEL_SOCKET`
- Host D-Bus services
- X11 through `$DISPLAY`
- Devices and other host resources

The sandbox engine supplies the application metadata, so a grant is only as
trustworthy as that engine. Clients connected through the ordinary Wayland
socket remain unrestricted.

Treat security contexts as one part of a sandbox boundary, not as complete
application isolation.
