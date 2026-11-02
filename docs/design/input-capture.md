# Portal input capture

Umbriel owns input interception, approval, barrier geometry, and the EIS server. `xdg-desktop-portal-umbriel` implements `org.freedesktop.impl.portal.InputCapture` version 2, which the standard desktop portal frontend exposes to applications. The portal passes the approved EIS connection FD to the application; it does not process input events or require libeis at runtime.

The private `umbriel_input_capture_v1` Wayland protocol provides version 2 of the compositor bridge. This version is independent of the public D-Bus portal version. Its XML lives in both repositories and must stay identical. It transports session control, logical zones, barrier results, activation IDs, permission grants, and the EIS FD. Input events use standard EI rather than this Wayland protocol. The portal requires bridge version 2; version 1 bridge clients retain session-only approval.

## Supported scope

The backend advertises keyboard and pointer capabilities, mask `3`, and accepts either capability independently. It supports the version 2 session lifecycle, screen zones, horizontal and vertical exterior barrier segments, activation and deactivation, explicit release, and connection reuse across enable/disable cycles. The official legacy `CreateSession` method uses the same approval path for older frontends.

Permission persistence supports modes 0, 1, and 2 through the standard frontend restore-token flow. Clipboard integration, touch, tablets, gesture capture, and the RemoteDesktop portal are outside this implementation. Existing Xwayland input emulation remains a separate sender-client path.

## Permission and lifecycle

Creating a control session grants no input access. `Start` requests a compositor-owned confirmation unless it restores a valid grant. Only backend keyboard input can approve it: Enter allows once, and physical R remembers only when the client requested persistence. Wayland virtual-keyboard and EI-emulated events cannot approve capture. Labels in the compositor prompt are explicitly requester-supplied, so a direct host client cannot bypass approval by claiming a portal or application name. Security-context clients cannot see the bridge by default.

The compositor owns opaque 256-bit grant credentials, separate from the frontend's public restore tokens. Grants check the exact application identifier, including empty identifiers, and the maximum approved capability mask. Restore consumes and replaces the credential; failed identity, capability, or persistence upgrades leave it unchanged. A valid mode-zero restore consumes the stored grant and allows only the current session. Frontend tokens are translated into backend restore data tagged `umbriel-input-capture`, implementation version 1.

Mode-one grants stay in memory and belong to an application instance within the portal's Wayland connection. The portal derives that instance from the standard session path and expires it when its original D-Bus peer disappears, including after its old capture session closed. Bridge disconnect also expires its transient grants. Mode-two grants use an owned 0700 state directory and an atomic 0600 JSON store. Durable transactions refresh the store under a nonblocking lock, preventing replay across compositor instances. Malformed, insecure, or unavailable stores cannot silently create remembered permission.

The permission IPC lists grant IDs and scopes without credentials. Revoking a grant stops its live sessions, including a restored-once session whose stored credential was already consumed. Remembered approval only authorizes session creation; barriers, emergency release, locking, and other capture restrictions remain in force.

The portal backend accepts control calls only from the current unique owner of `org.freedesktop.portal.Desktop`. Sessions are bound to that owner and the application ID supplied by the frontend. Losing the frontend owner closes its sessions. Request cancellation validates that owner before changing any pending operation.

After approval, `ConnectToEIS` returns a private FD-backed connection. The compositor accepts EI receiver clients on this endpoint. `Enable` arms barriers even if the asynchronous EI handshake has not completed; a crossing can activate capture only after the required devices are available. The same EIS connection survives release and enable/disable cycles.

Only one session captures at a time. The first exterior barrier crossed by the unclipped motion segment wins. The compositor freezes its cursor at that intersection while `Activated` reports the unclipped target, preserving overshoot for the application. The activation ID is also the sequence passed to EI `start_emulating`. Stale release IDs cannot terminate a later activation.

Explicit `Disable` and `Release` do not emit the corresponding unsolicited lifecycle signals. Compositor-initiated suspension emits `Deactivated` before `Disabled`. Escape, locking, VT/session loss, device or transport loss, and output changes release captured state. A client must enable again after suspension. Output changes invalidate barriers and publish the previous zone-set ID in `ZonesChanged`.

## Event routing

Relative motion is intercepted before relative-pointer broadcasts, pointer constraints, and output clamping. Buttons and scrolling are intercepted before mouse bindings and seat delivery. Absolute pointers retain a separate previous sample per device, because subtracting the frozen local cursor would repeat movement. Relative motion resets those absolute baselines.

Each backend keyboard has an EI device with its own XKB keymap. Captured keys and modifiers bypass ordinary bindings, local keyboard delivery, and input-method grabs, while physical XKB and shared lock state continue to update. Pointer-only clients do not receive captured keyboard modifiers. The reserved Ctrl+Alt+Escape chord and VT switching remain compositor-controlled. Keyboard-only capture does not turn captured modifiers into local mouse bindings.

Activation clears local focus and transfers eligible held keys. Deactivation releases remote keys and buttons and consumes keys still physically held before restoring local focus. Keymap changes disable capture before replacing an affected EI keyboard. Touchscreen and tablet input restore local control; their events are not exported. Touchpad gesture capture is not advertised and the existing gesture pipeline is retained.

All zone rectangles use logical coordinates from the desktop output layout, including negative positions. Xwayland's transformed output coordinates are not used. Barrier validation rejects shared interior edges, diagonal lines, invalid IDs, and segments extending beyond their containing zone.

The relative EI pointer publishes one logical region per enabled output, with that output's physical scale. EI region offsets are unsigned, so their origin is translated to the layout's minimum x and y while portal zones and activation/release positions retain their signed layout coordinates. This preserves monitor sizes, gaps, and mixed scales without adding absolute pointer capabilities. Output layout or scale changes replace the pointer device because its regions are immutable after publication.

## Dependencies and verification

Umbriel retains its existing `libeis-1.0 >= 1.3` requirement. The capture implementation compiles against the 1.3 header. The portal daemon adds no runtime library dependency; its integration client uses test-only `libei-1.0 >= 1.3` alongside its existing sdbus-c++ dependency.

The integration runner also requires `dbus-daemon` and `dbus-send` from the `dbus` package, using their existing D-Bus 1 tool interface with no additional version minimum or pkg-config linkage. Both development shells include these test tools. Supply the `xdg-desktop-portal` frontend executable explicitly when it is outside `PATH`.

`tests/unit/input_capture_geometry.cpp` covers logical coordinates, shared and exposed edges, inclusive pixel endpoints, single-pixel corners, unclipped crossings, and one-event motion across outputs. Mutation checks remove or reverse those rules and must fail the corresponding assertions.

`tests/input_capture.sh` starts a private D-Bus, the actual portal backend, frontend and permission store, an EI receiver, a local Wayland observer, and a real Umbriel Server with backend-boundary keyboard and pointer devices. It verifies physical approval and virtual-keyboard rejection, asynchronous activation, event routing, held-state cleanup, stale releases, connection reuse, emergency escape, absolute deltas, lock-screen exclusion, monitor changes, and session teardown. It also checks EI region geometry, pointer replacement after hotplug, negative output positions, mixed scales, local monitor crossings, and capture on all four exterior sides. Its persistence checks cover both grant lifetimes, allow-once behavior, unrelated programs and empty identifiers, replay and escalation rejection, durable restart, and active revocation. It does not use the host's input devices, compositor, session services, or permission store.

Run it after building both repositories, passing the portal frontend executable when it is outside `PATH`:

```sh
bash tests/input_capture.sh ./build-debug ../xdg-desktop-portal-umbriel/build-debug /path/to/xdg-desktop-portal
```
