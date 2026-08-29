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

## Security boundary

The protocol labels a new Wayland connection and lets Umbriel filter it. It
does not constrain files, processes, devices, the network, D-Bus, or other host
interfaces. The metadata is supplied by the sandbox engine and is not
independently authenticated by Umbriel, so Umbriel does not use it to grant
extra privileges.

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
