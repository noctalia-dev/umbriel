# Security

## Session locking

Umbriel keeps the current desktop visible while an `ext-session-lock-v1` client prepares a mapped lock surface for every active output. The client has up to three seconds to provide those surfaces. Umbriel then switches every output to the lock scene and reports the session as locked only after each active output has presented a secure frame.

If a client stalls or omits an output, the handoff continues after the deadline with an opaque compositor-owned blank. Its RGB color comes from `[colors].backdrop`; its alpha is always fully opaque. A client that exits after the secure handoff leaves this blank in place, so the desktop cannot be exposed by a crashed locker.

## X11 desktop input

X11 applications can request control of the pointer and keyboard through XTest. Steam uses this for its desktop controller mappings. Umbriel receives these requests through its native EIS backend and asks for permission before allowing desktop input.

Press Enter to allow the requesting application for the current Umbriel session. Any other non-modifier key or pointer click denies it. Allow and deny decisions last until Umbriel restarts. Input received through this XTest emulation path cannot approve another permission prompt.

While the session is locked, Umbriel releases held emulated keys and buttons and discards further emulated input. That input is not replayed after unlock. Physical input devices continue to work on the lock screen.

This requires Xwayland built with libei support. No Steam preload library, device permission change, or RemoteDesktop portal implementation is required for this XTest input path.

## Input capture

Umbriel and a matching `xdg-desktop-portal-umbriel` support version 2 of `org.freedesktop.portal.InputCapture`. Applications can request capture for keyboard and pointer sharing, then activate it by crossing an approved screen-edge barrier. Actual input events use the standard EI transport.

| Feature                                                 | Support                                                                   |
| ------------------------------------------------------- | ------------------------------------------------------------------------- |
| Keyboard events and modifiers                           | Supported                                                                 |
| Pointer motion, buttons, and scrolling                  | Supported                                                                 |
| Keyboard-only or pointer-only sessions                  | Supported; capability masks `1`, `2`, or `3`                              |
| Logical screen zones and exterior edge barriers         | Supported, including negative output positions and scaled outputs         |
| `CreateSession2`, `Start`, enable, disable, and release | Supported                                                                 |
| Permission persistence and restore tokens               | Optional explicit approval, for the application lifetime or until revoked |
| Clipboard sharing through the capture session           | Not supported                                                             |
| Touchscreen, tablet, and touchpad gesture capture       | Not supported                                                             |
| RemoteDesktop portal input injection                    | Not provided by InputCapture                                              |

When an application starts a new capture session, Umbriel displays its own permission prompt. Press Enter on a backend keyboard to allow that session once. If the application requests permission persistence, press **R** to remember approval for the lifetime shown in the prompt: while the application runs, or until revoked. Any other non-modifier key or pointer click denies it. Wayland virtual keyboards and emulated EI input cannot approve or remember capture. The application label is supplied by the requester; its name alone never restores permission.

Remembered approval is available to any application using the standard restore-token flow, including applications with an unidentified label. Applications must save and supply the token returned by the portal. Invalid, expired, or revoked tokens and requests for additional capabilities require fresh approval. Applications that do not request persistence continue to ask for each new session. Remembering input permission does not grant clipboard access.

Inspect and revoke grants with:

```sh
umbriel input-capture-permissions
umbriel input-capture-revoke <id>
umbriel input-capture-revoke all
```

Revocation stops sessions using the grant. Application-lifetime grants expire when the application's D-Bus connection disappears. Durable grants survive restarts and are stored privately in `$XDG_STATE_HOME/umbriel/input-capture-permissions.json`, defaulting to `~/.local/state/umbriel/input-capture-permissions.json`. Treat restore tokens as private credentials; the permission listing never exposes them.

Enabling a session arms its barriers. Capture begins only when Umbriel detects a permitted crossing. While a capability is captured, its events go to the approved EI receiver instead of local clients. Press **Ctrl+Alt+Escape** on a backend keyboard to stop capture and restore local control. The application must enable the session again before another crossing can activate capture.

Locking, VT/session deactivation, transport failure, and session closure stop capture. Output geometry changes disable capture and invalidate its barriers. Touchscreen and tablet input also restore local control. Pointer constraints, held pointer buttons, and compositor grabs prevent a barrier from taking over an existing interaction. Held input is released during handoff so it cannot remain pressed after capture ends. Lock-screen input is never exported.

Install matching versions of Umbriel and the portal backend. The portal frontend must support InputCapture; version 2 calls require a frontend exposing version 2. The integration checks exercise the real `xdg-desktop-portal` 1.22.1 frontend. No configuration option or additional libei package is needed in Umbriel.

For Deskflow with multiple monitors, use version 1.27 or newer. Deskflow 1.26 can submit duplicate barrier IDs and fail to arm capture.

## Sandboxed Wayland clients

Umbriel supports version 1 of the [Wayland security-context protocol](https://wayland.app/protocols/security-context-v1). It is always enabled.

A sandbox engine can create a restricted Wayland connection and label it with the sandbox engine, application, and instance. Umbriel then limits the protocols available through that connection. This does not create the sandbox or display permission prompts.

### Restricted capabilities

Restricted clients retain the protocols needed for ordinary windows, rendering, focused input, clipboard use, output discovery, idle inhibition, and activation. Umbriel withholds compositor-wide authority, including:

- Screen and window capture
- Direct access to the input-capture bridge
- Virtual input and input-method ownership
- Clipboard-manager access
- Layer shell and session locking
- Gamma and output configuration
- Global workspace and window control
- Creation of nested security contexts

Trusted host services such as xdg-desktop-portal can mediate privileged operations for a sandboxed application.

New protocols remain hidden from restricted clients until they receive a security review.

### Per-application grants

Use `[[security_context_rule]]` when a sandboxed application genuinely needs a protocol without a portal equivalent:

```toml
[[security_context_rule]]
match.sandbox_engine = 'org\.flatpak'
match.app_id = 'org\.example\.ClipboardManager'
allow_globals = [
  "ext_data_control_manager_v1",
  "zwlr_data_control_manager_v1",
]
```

Umbriel supports both data-control variants. Existing clipboard managers often use the `zwlr_` variant, so grant both unless the application is known to use only `ext_`.

| Selector               | Description                                                |
| ---------------------- | ---------------------------------------------------------- |
| `match.sandbox_engine` | Exact regular-expression match against the sandbox engine. |
| `match.app_id`         | Exact regular-expression match against the application ID. |

`allow_globals` lists additional Wayland globals to expose. Every matching rule contributes its values.

Selectors are optional. A rule without selectors applies to every restricted client, so avoid broad grants. Patterns match the complete value rather than a substring. Either selector takes one pattern or an array of them, and any one of them matching is enough.

Rules are additive and cannot remove the base protocol set. `wp_security_context_manager_v1` remains blocked even if listed. Changes apply only to new connections, so restart an application after changing its grants.

Invalid entries are ignored with a configuration warning.

### Security boundary

The protocol restricts one Wayland connection. It does not restrict files, processes, devices, networking, D-Bus, X11, or other host interfaces.

For the restriction to matter, the sandbox must expose only its restricted Wayland socket. It must also control access to:

- Umbriel IPC through `$UMBRIEL_SOCKET`
- Host D-Bus services
- X11 through `$DISPLAY`
- Devices and other host resources

The sandbox engine supplies the application metadata, so a grant is only as trustworthy as that engine. Clients connected through the ordinary Wayland socket remain unrestricted.

Treat security contexts as one part of a sandbox boundary, not as complete application isolation.
