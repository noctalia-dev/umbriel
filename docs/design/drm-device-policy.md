# DRM GPU exclusion

`BackendManager` filters GPUs before wlroots opens their KMS nodes and gives
`Server` the selected backend, session, and renderer factory.

Without exclusions, Umbriel uses `wlr_backend_autocreate()` and
`fx_renderer_create()`. The policy does not apply to nested Wayland, X11, or
headless backends.

## Native path

For a native session with exclusions, `BackendManager`:

1. Resolves the ignored GPU identities and enumerates DRM cards through udev.
2. Opens one backend for each allowed card.
3. Creates the renderer from the primary card through GBM.
4. Checks open device descriptors after allocator creation and renderer recovery.

Separate DRM backends and the explicit GBM constructor avoid automatic
renderer discovery that could open excluded GPUs. Descriptor checks reject
startup or renderer recovery if drivers open excluded DRM or proprietary
NVIDIA devices. Unreadable descriptors and DRM descriptors without a resolvable
GPU identity also fail the check. Descriptors closed during the scan are skipped.
NVIDIA driver metadata is read only for open per-GPU devices that lack a PCI
identity from udev; device numbers identify these even through aliases.

Device paths resolve into `DrmDeviceIdentity` before any GPU opens. Startup
rejects unresolved paths. Hotplug filtering uses the captured identities even
after paths disappear or get reused. PCI selectors need no DRM node to resolve.

Configuration errors that could hide exclusions stop startup. The
[configuration reference](../user/configuration.md) covers selectors, includes,
reloads, and compatibility limits.

## Verification

The [contributor guide](../../CONTRIBUTING.md#umbrielfx) describes simulated
backend tests, EGL ABI checks, and hardware renderer checks. Physical VFIO
transitions require testing in a dedicated native session.
