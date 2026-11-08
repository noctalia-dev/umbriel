# Color management

Implementation notes for the color-management protocol and the `color_profile`/`sdr_gamut` output settings. The user-facing contract is in [Outputs](../user/outputs.md#color-profile-and-sdr-gamut); this note explains the pieces that are easy to break.

## Characterization

`Output::color_profile` resolves to a `PanelColorimetry`: primaries, white, and a `PanelTransfer` that is either a named curve (`Srgb`, `Gamma22`), or a sampled encode curve baked into a 3x1D LUT when the panel's TRC matches no named curve exactly.

- A configured ICC file is parsed by `parseIccProfile` (`src/output/colorimetry_icc_lcms2.cpp`). An EDID is parsed by `panelColorimetryFromEdid`; `iccStoreForPanel` then synthesizes the matrix-shaper profile that `get_information` advertises, so EDID characterization still answers `icc_file`.
- The parser undoes the profile's D50 adaptation (`chad`, or a measured `wtpt`) before reporting colorants, so the reported primaries are the measured ones rather than PCS-relative ones.
- The parser picks the panel transfer: `PanelTransfer::Kind::Srgb` or `Gamma22` when the TRC matches a named curve, `Kind::Curve` otherwise. `Kind::Curve` is sampled into a 3x1D LUT by `buildSdrGamutTransform` and rendered exactly, so output characterization does not approximate curves.
- `fitTransfer` (`src/output/colorimetry.cpp`) is the client-description classification only, not an output-path step: it probes a parsed curve against linear, sRGB and gamma 2.2 (including the shadow toe, with geometric probes) and reports `Unsupported` for anything else, which `clientIccDescription` turns into `failed('unsupported')`.

## What a client description can carry

`ColorDescription::data` is wlroots' protocol struct, and `wlr_scene_buffer_set_transfer_function` takes only a named `wlr_color_transfer_function`. There is no per-buffer exponent or LUT, so a transfer curve is representable only when it is exactly one of the named curves the renderer implements.

- `clientIccDescription` (`src/server/color_description.cpp`) accepts the `Srgb` and `Gamma22` fits and refuses `TransferFit::Kind::Power` with `unsupported`. The power fit of an arbitrary TRC is an approximation, and substituting the nearest named curve would advertise colorimetry the profile does not contain.
- The parametric `set_tf_power` request admits exponents in `[1.0, 10.0]` per protocol, but `handleParametricCreate` canonicalizes only 1.0 (ext_linear) and 2.2 (gamma 2.2) through `namedTransferForPower` and fails the description otherwise. BT.1886 is *not* a plain 2.4: it carries the BT.2035 luminance offset, so a 2.4 power has no exact named equivalent.
- `ColorDescription` therefore has no power field of its own; a description's transfer curve is always `data.tf_named`.

The ICC parser applies one curve to all three channels, so a profile whose RGB TRCs differ is rejected (`per-channel TRCs differ`) instead of being rendered with the red curve: a gray-axis cast is not something the clients asked for, and the same bytes are advertised back through `icc_file`. Curves are compared after the monotone repair (`evaluateCurve`), so a shared non-monotone table is repaired once per channel, compares equal, and stays accepted.

## ICC payload lifetime and the per-client budget

A client's staged profile and every description it creates from it share one `IccProfileStore` (`std::shared_ptr<const std::vector<uint8_t>>`). The compositor copies descriptions — surface state, cached commits, wlroots' synchronized state — so the resource that created the payload is the wrong proxy for the memory's lifetime.

- `iccStoreForFileProfile` takes an optional release hook. The store's deleter runs it when the last copy dies, which is exactly when the compositor stops retaining those bytes.
- The budget is one `bytes` bucket covering staged *and* retained bytes per `wl_client` (`kMaxIccBytesPerClient`), plus `kMaxIccCreatorsPerClient` concurrent creators. `IccCreator::charge` owns the staged charge and `handleIccCreate` moves it into the payload; the creator never touches the ledger afterwards. Charging at create *and* keeping the staged charge would double count, and releasing the staged charge at create would zero the retained charge.
- `IccCharge` holds the `IccBudget` object, not the `wl_client*`: wl_client addresses are reused, and a late release must not debit a later client. A `wl_client` destroy listener removes the budget from the map so a reused address starts empty while live payloads keep their budget alive.
- `SurfaceState` embeds `ColorDescription`, so its `wlr_surface_synced_impl` must provide real `init_state`, `finish_state`, and `move_state` hooks. wlroots `memcpy`s a synchronized state when `move_state` is null (including the cached-commit path), which shares a `shared_ptr` control block nobody counted; `finish_state` releases the payload without ending the object because wlroots frees cached states itself.

## Advertising bytes

`wp_image_description_info_v1.icc_file` must return the exact bytes the characterization stores — the configured file as written, never a reserialization — because clients compare identity and detect changes through it. Client-created descriptions never answer `get_information` (the create contract forbids it), so their store is used only for identity and change detection.

## Tests

- `tests/unit/colorimetry.cpp`: parser contracts (per-channel TRC rejection, repair warnings, `chad`/`wtpt` handling, store release hook).
- `tests/unit/color_description.cpp`: exact-fit mapping, power canonicalization, refused curves.
- `tests/harness/checks/protocol/color_profile.sh`: characterization end to end, `icc_file` bytes, the create_icc contract including the refused gamma 1.8 profile.
- `tests/harness/checks/protocol/color_profile_limits.sh`: staged, retained, and surface-retained budget accounting.
- `tests/harness/checks/protocol/color_feedback.sh`: parametric validation including `set_tf_power`.
