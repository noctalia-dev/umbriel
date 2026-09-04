# Sandboxed Wayland clients

Umbriel supports version 1 of the
[Wayland security-context protocol](https://wayland.app/protocols/security-context-v1).
Support is always active and has no configuration key.

A sandbox engine can give Umbriel a listening Wayland socket, identify the
sandbox engine, application, and running instance, then expose only that socket
inside the sandbox. Umbriel recognizes clients accepted through it as
restricted. This changes what those clients can bind; it does not create the
sandbox or show a permission prompt.

## Restricted capabilities

Restricted clients retain the protocols needed for normal application windows,
rendering, focused input, regular clipboard use, output discovery, idle
inhibition, and activation requests. Umbriel withholds protocols that provide
compositor-wide authority:

- Screen, output, and window capture
- Virtual keyboard and pointer injection, plus input-method ownership
- Clipboard-manager access through data-control
- Layer-shell surfaces and session locking
- Gamma and output configuration
- Logical output topology and system-wide idle observation
- Global window and workspace discovery or control
- The security-context manager itself, which prevents nested contexts

The allowed protocol set is explicit. A protocol added to Umbriel later stays
hidden from restricted clients until it has received a security review.

Trusted host services such as xdg-desktop-portal can mediate capture and other
privileged operations for a sandboxed application.

## Per-application grants

Some protocols have no portal equivalent. A status bar needs layer-shell, a
clipboard manager needs data-control, and an input method needs input-method
ownership. `[[security_context_rule]]` entries widen the allowed set for
individual applications, matched against the metadata the sandbox engine
supplies. Every matching rule contributes its globals.

```toml
[[security_context_rule]]
match.sandbox_engine = 'org\.flatpak'
match.app_id = 'org\.example\.ClipboardManager'
allow_globals = [
  "ext_data_control_manager_v1",
  "zwlr_data_control_manager_v1",
]
```

Umbriel advertises both data-control variants to unrestricted clients. The
`ext_data_control_manager_v1` global is the current protocol, while
`zwlr_data_control_manager_v1` remains available for legacy clients. Both can
observe and replace selections without keyboard focus, so restricted clients
receive neither unless a matching rule grants it. Grant only the variants the
application requires; the example grants both for compatibility.

| Selector | Type | Description |
|----------|------|-------------|
| `match.sandbox_engine` | regex | Match the sandbox engine that labeled the connection. |
| `match.app_id` | regex | Match the application ID the engine supplied. |

| Key | Type | Description |
|-----|------|-------------|
| `allow_globals` | string array | Wayland globals exposed in addition to the base allowed set. |

Every selector is optional. A rule without selectors applies to every
restricted client, and a client whose metadata omits a field never matches a
selector on that field. Unlike window and layer rules, a pattern must match
the entire value: a substring match would let an application choose an ID that
embeds another rule's pattern.

A rule with a mistake is dropped. An unknown key, an empty or non-string
selector, or an empty `allow_globals` rejects the entry with a warning.

Rules are additive only. They cannot remove protocols from the base set, and
`wp_security_context_manager_v1` remains blocked even when listed, so a
granted client can never create a nested context and label itself into another
application's rules. A client's grants are decided when it first reads the
registry and stay fixed for that connection. A reload therefore affects
applications launched afterwards; a running application keeps its grants until
it is restarted.

## Security boundary

The protocol labels a new Wayland connection and lets Umbriel filter it. It
does not constrain files, processes, devices, the network, D-Bus, or other host
interfaces. The metadata is supplied by the sandbox engine and is not
independently authenticated by Umbriel. Restricted clients cannot create
security contexts, so labels always originate from an unrestricted process,
and a per-application grant is only as trustworthy as the sandbox engine that
labeled the connection. Without `[[security_context_rule]]` entries the
metadata grants nothing.

For the restriction to matter, the sandbox must not expose Umbriel's original
Wayland socket. It must separately control Umbriel IPC through
`$UMBRIEL_SOCKET`, host D-Bus services, and X11 through `$DISPLAY`. In
particular, X11 applications run through xwayland-satellite outside this
Wayland security context.

Clients connected to the ordinary Wayland socket are unchanged and can still
bind every privileged global Umbriel normally advertises. This feature is one
part of a sandbox boundary, not a claim that an application is fully isolated.
The protocol is currently a staging protocol, so compatible extensions may be
added in later versions.
