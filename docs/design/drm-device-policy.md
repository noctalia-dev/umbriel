# DRM device policy

Umbriel must decide which GPUs it can use before wlroots opens a KMS node.
Filtering after `wlr_backend_autocreate()` is too late: wlroots has already
opened every discovered card and installed its own hotplug monitor.
`WLR_DRM_DEVICES` cannot provide the same contract because it encodes multiple
paths with `:`, which is also part of a PCI by-path name.

`BackendManager` keeps this behavior behind one boundary. `Server` receives a
backend, a session, and a renderer factory. It does not know whether the
manager used wlroots discovery or Umbriel's filtered native path.

## Compatibility path

When `[drm]` is absent or empty, `BackendManager` calls
`wlr_backend_autocreate()` and `fx_renderer_create()` without changing the
environment. This path preserves the previous behavior.

A nonempty `[drm]` section stays inactive for Wayland, X11, and headless
backends. In a native session, `render_device` overrides
`WLR_RENDER_DRM_DEVICE`, and exclusions override `WLR_DRM_DEVICES`. A
render-only configuration preserves `WLR_DRM_DEVICES`.

`BackendManager` hides only the overridden variables while wlroots creates the
backend, then restores them. Renderer creation normally receives an explicit
DRM file descriptor. The fallback for a backend without a DRM file descriptor
hides only `WLR_RENDER_DRM_DEVICE`.

## Filtered native path

Exclusions require a custom native path. Startup follows this sequence:

1. Create an active wlroots session and add libinput in the requested
   `WLR_BACKENDS` order.
2. Resolve the ignored paths and enumerate the seat's DRM card nodes through
   udev.
3. Exclude matching physical GPUs and recheck each allowed card before and
   after the privileged open.
4. Create an independent backend for each allowed KMS node. The first
   successful backend becomes the primary.
5. Create the renderer from an explicit DRM file descriptor through GBM.
6. Inspect open character-device file descriptors after renderer and allocator
   creation. Fail startup if one belongs to an excluded GPU.

Enumeration matches wlroots 0.20.2, including the ten-second waits and
boot-display preference. Device filtering is the only change before open.
Allowed DRM backends remain independent afterward so no hidden renderer can
enumerate excluded EGL devices.

`DrmDeviceIdentity` joins card and render nodes through their parent sysfs
device. A selector can match a canonical node path, a udev link, a device
number, the parent sysfs path, or a PCI address. A render-node path therefore
excludes the same GPU's card node.

If an ignored path disappears, the policy clears its device number but keeps
its last parent and PCI identity. A later card add or session resume resolves
the path again. An invalid existing path blocks new device opens. A path that
has never resolved and is not advertised as a udev link cannot identify an
unbound GPU, so VFIO configurations also need `ignored_pci_addresses`.

## Runtime transitions

The manager listens for session card-add, active, and destroy signals. It also
listens for the destruction of libinput and every DRM backend.

- A card add or session resume reruns path resolution and enumeration.
- A newly available allowed card becomes a secondary backend and starts
  immediately if the main backend has started.
- If an existing path selector starts matching a secondary GPU, the manager
  removes that backend.
- If an existing path selector starts matching the primary GPU or renderer
  GPU, the manager ends the session.
- Loss of the primary GPU, libinput, or the native session ends the session.
- A failed hotplug reconciliation gets one idle retry. It does not enter a
  retry loop.

The primary GPU never changes in a running session. Existing outputs,
allocators, and renderer state depend on that choice, so live promotion would
leave mixed ownership across wlroots objects.

## Renderer selection

`render_device` accepts a render node. Umbriel resolves the path and checks all
exclusions before opening it. Umbriel then checks the opened file descriptor
and verifies that umbrielfx selected the same physical GPU. These checks prevent
a path replacement from changing the selected GPU.

An unavailable, invalid, excluded, or unusable renderer produces a warning.
Umbriel then uses the primary allowed GPU. With exclusions, both paths create
the renderer through GBM and never use global EGL device enumeration. Some
GLVND vendors retain file descriptors for every GPU they probe, even when they
select another GPU as the renderer.

Allowed secondary GPUs import the compositor's DMA-BUFs directly. Umbriel does
not give them a wlroots parent because that creates an internal multi-GPU blit
renderer through EGL device enumeration. Drivers with incompatible DMA-BUF
formats or modifiers therefore cannot use an output on the secondary GPU under
strict exclusion.

After renderer and allocator creation, Umbriel scans its open character-device
file descriptors and resolves each one through udev. Startup fails if a device
number, physical device, or PCI address matches an exclusion. This catches
driver behavior outside the DRM backend before the compositor starts.
`WLR_RENDERER_FORCE_SOFTWARE=1` is incompatible with exclusions because
umbrielfx's software path enumerates EGL devices.

Proprietary NVIDIA per-GPU nodes do not expose a udev parent on every driver
version. Umbriel maps `/dev/nvidiaN` through
`/proc/driver/nvidia/gpus/*/information` before applying the same PCI exclusion
check. Incomplete or unreadable metadata fails the audit instead of weakening
it. Shared control nodes such as `/dev/nvidiactl` are not attributed to one
physical GPU.

Renderer recovery uses the immutable startup policy. A configuration reload
can record new `[drm]` values, but those values cannot affect a recovered
renderer until Umbriel restarts.

## Verification

Run the automated checks from the repository root:

```sh
just test debug
just verify debug
just lint debug
```

The unit tests cover configuration, physical GPU matching, path identity,
device filtering, and backend selection. The headless checks use a nonempty
`[drm]` section to verify that native-only policy stays inactive. The
`umbrielfx` tests cover GBM renderer identity and file descriptor ownership.

Automated checks cannot reproduce native GPU lifecycle events. Before a
release, test these cases on a dual-GPU host:

- Path and PCI exclusion
- Explicit renderer selection and fallback
- Secondary GPU add and remove
- Suspend and resume
- Startup with every GPU excluded
- Primary GPU loss

Run the hardware tests from a separate TTY. Primary GPU loss intentionally
ends the compositor.
