# Xwayland input emulation

Steam's desktop controller mappings generate XTest pointer and keyboard requests. Xwayland translates them into EI events; Umbriel's `EisInput` receives them through libeis and feeds native wlroots keyboard and pointer devices into the existing input paths. This preserves compositor shortcuts, focus, pointer constraints, and input-method handling.

The required server API is `libeis-1.0 >= 1.3`, supplied by libei. The test sender also requires `libei-1.0 >= 1.3`. The Arch CI build and lint jobs install `libei`; the Alpine CI build installs `libei-dev`. These packages supply both pkg-config modules.

## Connection and consent

Umbriel creates an EIS socket in a private runtime directory, opens its inode with `O_PATH | O_CLOEXEC`, and unlinks its pathname. Xwayland inherits `LIBEI_SOCKET=/proc/<compositor-pid>/fd/<descriptor>`. The descriptor remains in the compositor, so no extra descriptor handoff or wlroots change is needed. Normal application and session-service launches do not inherit this endpoint. `LIBEI_SOCKET` is reserved against configuration overrides.

The EIS socket peer must hold the private Wayland socket that Umbriel's wlroots Xwayland server created for that launch. Umbriel captures the socket's device and inode in the server-start callback and matches them against the peer's descriptors through `/proc`. This authenticates the actual Xwayland process despite wlroots' double fork. Merely discovering the EIS endpoint or claiming an allowed application name does not authenticate another process.

Umbriel owns the listening socket and reads `SO_PEERCRED` on each accepted connection before creating a libeis client. Authenticated connections are bridged into libeis' FD backend, which is available in 1.3. This avoids the socket-backend PID API introduced in 1.5. The bridge preserves stream order and `SCM_RIGHTS` descriptors, including keyboard keymaps, with nonblocking backpressure and bounded connection, byte, packet, and descriptor queues. Disconnects release both transport endpoints and queued descriptors.

Each authenticated EI connection supplies an X11 application name. The first request displays a compositor confirmation; pending clients receive no seat or devices. Approval and denial are remembered by claimed name for the compositor session. Names are not verified application identities, and the prompt states that distinction. Emulated events are dropped while any confirmation is visible, so an already authorized client cannot approve a different request. Disconnecting a pending client removes its confirmation.

## Input lifecycle

Each approved connection owns its native keyboard and pointer devices. The keyboard uses the physical layout source when available, otherwise the configured XKB layout. Emulated keyboards retain that keymap and do not become physical layout sources. Physical layout and locked-modifier changes are forwarded to EI clients.

Absolute regions use the existing `XwaylandOutputs` coordinate mapping, including negative compositor origins and native-resolution X11 outputs. Incoming absolute positions and relative deltas are converted back to logical compositor coordinates before entering the cursor pipeline. Device rebuilding for output or keymap changes is coalesced into an idle callback, so an input-triggered configuration action cannot delete its notifying device.

Stopping emulation or disconnecting releases held keys and buttons. Locking also releases held input and rejects new requests. Existing transports remain resumed while their events are discarded: pausing them would make Xwayland queue XTest events and replay them after unlock. Modifier notifications are withheld during lock. Lock-surface cleanup after unlock cannot replace the desktop keyboard focus that the unlock handler restored.

## Verification

`tests/unit/eis_socket.cpp` exercises real Unix sockets and the Wayland event loop: peer credentials and rejection before backend creation, bidirectional backpressure and stream order, keymap descriptor forwarding, truncated-control rejection without descriptor leaks, and half-close draining and cleanup.

The real XTest sender in `tests/harness/clients/xwayland_input_client.cpp` maps no X11 window. Its input must reach native Wayland observer windows to pass these isolated compositor checks:

- `input/xwayland_desktop`: consent, mixed output scales, negative origins, native-resolution X11 coordinates, relative motion, clicks, keyboard input, compositor shortcuts, and consent reuse.
- `input/xwayland_permissions`: denial, prevention of emulated approval, held-input cleanup, physical lock-screen input, and discarded lock-time input without replay after unlock.
- `input/xwayland_eis_peer`: an unrelated EI process claiming an approved name is rejected while the genuine Xwayland connection remains usable.

A physical Steam Controller check completes the end-to-end Steam validation; the compositor transport and permission lifecycle can be exercised without controller hardware.
