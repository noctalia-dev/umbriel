# Color management

Implementation notes for the color-management protocol and the `color_profile`/`sdr_gamut` output settings. The user-facing contract is in [Outputs](../user/outputs.md#color-profile-and-sdr-gamut); this note explains the pieces that are easy to break.

## Characterization

`Output::color_profile` resolves to a `PanelColorimetry`: primaries, white, and a `PanelTransfer` that is either a named curve (`Srgb`, `Gamma22`), or a sampled encode curve baked into a 3x1D LUT when the panel's TRC matches no named curve exactly.

- A configured ICC file is parsed by `parseIccProfile` (`src/output/colorimetry_icc_lcms2.cpp`). An EDID is parsed by `panelColorimetryFromEdid`; `iccStoreForPanel` then synthesizes the matrix-shaper profile that `get_information` advertises, so EDID characterization still answers `icc_file`.
- The parser undoes the profile's D50 adaptation (`chad`, or a measured `wtpt`) before reporting colorants, so the reported primaries are the measured ones rather than PCS-relative ones.
- The parser picks the panel transfer: `PanelTransfer::Kind::Srgb` or `Gamma22` when the TRC matches a named curve, `Kind::Curve` otherwise. `Kind::Curve` is sampled into a 3x1D LUT by `buildSdrGamutTransform` and rendered exactly, so output characterization does not approximate curves. When that curve is a pure power, `PanelTransfer::exponent` records the exponent (0 otherwise) so the description can restate it as the protocol's `tf_power` without contradicting the profile.
- `fitTransfer` (`src/output/colorimetry.cpp`) is the client-description classification only, not an output-path step: it probes a parsed curve against linear, sRGB and gamma 2.2 (including the shadow toe, with geometric probes) and reports `Unsupported` for anything else, which `clientIccDescription` turns into `failed('unsupported')`.

## What a client description can carry

`ColorDescription::data` is wlroots' protocol struct, and `wlr_scene_buffer_set_transfer_function` takes only a named `wlr_color_transfer_function`. There is no per-buffer exponent or LUT, so a transfer curve is representable only when it is exactly one of the named curves the renderer implements.

- `clientIccDescription` (`src/server/color_description.cpp`) accepts the `Srgb`, `Linear` and `Gamma22` fits and refuses `TransferFit::Kind::Unsupported` with `unsupported`. That covers ICC v2/v4 profiles: 3-channel, Display or ColorSpace class, matrix-shaper, at most 32 MB (`kClientIccSizeLimit`), and identical RGB TRCs. A curve with no exact named equivalent cannot be painted, and substituting the nearest named curve would advertise colorimetry the profile does not contain.
- The parametric `set_tf_power` request admits exponents in `[1.0, 10.0]` per protocol; `namedTransferForPower` canonicalizes only the values within `1e-3` of 1.0 (ext_linear) and 2.2 (gamma 2.2), and `handleParametricCreate` fails the description otherwise. The `eexp` wire step is `1e-4`, so the window absorbs the four-decimal quantization (at most 0.015% relative error) rather than admitting a different curve. BT.1886 is *not* a plain 2.4: it carries the BT.2035 luminance offset, so a 2.4 power has no exact named equivalent.
- A client-created description is a statement about the content, not a request for conversion, so `ColorDescription::tfPower` stays 0 for it: its transfer curve is always `data.tf_named`.

## What an output advertises

`descriptionForOutput` must name the encoding the frame is really written in, because a client converts its buffer into whatever the description says.

- Without a staged SDR gamut transform the renderer composes into sRGB (`fx_pass.c`: `pass->sdr_tf = WLR_COLOR_TRANSFER_FUNCTION_SRGB`, primaries converted to sRGB) regardless of `sdr_transfer`. The description is therefore the uncharacterized output description, whatever `color_profile` says; advertising the panel there would make a tagged client convert into an encoding we never produce. Pinned live by `color_profile.sh` (tagged pixels must equal untagged).
- A staged transform makes the panel the destination: `characterizedOutputDescription` replaces the primaries with the panel's and names the curve when it has one (`Srgb`, `Gamma22`), otherwise reports `tf_power`. A sampled power curve is reported both in the ICC profile and as `tf_power`, both derived from the same parsed value, so the two can never disagree.
- `bufferTransferFunction` cannot express an exponent (`wlr_scene_buffer_set_transfer_function` takes named curves), so tagged content described by `tf_power` is decoded through the nearest named EOTF: ext_linear up to 1.6, gamma 2.2 up to 2.3, BT.1886 above. That is an input-side approximation only — the output stage still renders the measured curve — and it is why the sampled-curve path stays exact for untagged content.

The ICC parser applies one curve to all three channels, so a profile whose RGB TRCs differ is rejected (`per-channel TRCs differ`) instead of being rendered with the red curve: a gray-axis cast is not something the clients asked for, and the same bytes are advertised back through `icc_file`. Curves are compared after the monotone repair (`evaluateCurve`), so a shared non-monotone table is repaired once per channel, compares equal, and stays accepted.

## ICC payload lifetime and the per-client budget

A client's staged profile and every description it creates from it share one `IccProfileStore` (`std::shared_ptr<const std::vector<uint8_t>>`). The compositor copies descriptions — surface state, cached commits, wlroots' synchronized state — so the resource that created the payload is the wrong proxy for the memory's lifetime.

- `iccStoreForFileProfile` takes an optional release hook. The store's deleter runs it when the last copy dies, which is exactly when the compositor stops retaining those bytes.
- The budget is one `bytes` bucket covering staged *and* retained bytes per `wl_client` (`kMaxIccBytesPerClient`), plus `kMaxIccCreatorsPerClient` concurrent creators. `IccCreator::charge` owns the staged charge and `handleIccCreate` moves it into the payload; the creator never touches the ledger afterwards. Charging at create *and* keeping the staged charge would double count, and releasing the staged charge at create would zero the retained charge.
- `IccCharge` holds the `IccBudget` object, not the `wl_client*`: wl_client addresses are reused, and a late release must not debit a later client. A `wl_client` destroy listener removes the budget from the map so a reused address starts empty while live payloads keep their budget alive.
- `SurfaceState` embeds `ColorDescription`, so its `wlr_surface_synced_impl` must provide real `init_state`, `finish_state` and `move_state` hooks. wlroots `memcpy`s a synchronized state when `move_state` is null (including the cached-commit path), which would share a `shared_ptr` control block nobody counted; `finish_state` releases the payload without ending the object because wlroots frees cached states itself. `move_state` copies rather than moves, because wlroots reuses `surface->pending` across commits. `wlr_surface_synced_init` memsets the pending/current members before `init_state`'s placement new (its documented contract); they are empty at that point today, but a future member with a non-trivial default would be zeroed there.
- `unset_image_description` clears the pending description as well as `set`, so the payload it held is released instead of staying charged until something overwrites it.

Request-contract violations are protocol errors, not graceful failures: a `set_icc_file` fd that is not a seekable regular file posts `bad_fd`, a file over 32 MB or one that would exceed the per-client byte budget posts `bad_size`, too many concurrent creators post `unsupported_feature` (the manager enum has no resource-limit entry), and `get_information` on a client-created description posts `no_information`. Each disconnects the offending client and leaves the compositor running. Only what cannot be judged before reading the bytes — ICC version, class, structure, per-channel TRCs, a non-representable curve — fails the description gracefully instead (`wp_image_description_v1.failed`, cause `unsupported`), which a client can observe without losing its connection.

## Advertising bytes

`wp_image_description_info_v1.icc_file` must return the exact bytes the characterization stores — the configured file as written, never a reserialization — because clients compare identity and detect changes through it. Client-created descriptions never answer `get_information` (the create contract forbids it), so their store is used only for identity and change detection.

## Tests

- `tests/unit/colorimetry.cpp`: parser contracts (per-channel TRC rejection, repair warnings, `chad`/`wtpt` handling, store release hook).
- `tests/unit/color_description.cpp`: exact-fit mapping, power canonicalization, refused curves.
- `tests/harness/checks/protocol/color_profile.sh`: characterization end to end, the advertised destination (`tf`/`primaries`/`icc_size` under `"native"`, `tf_power` and the profile bytes under `"srgb"`), the tagged-vs-untagged pixel pin, and the create_icc contract including the refused gamma 1.8 profile.
- `tests/harness/checks/protocol/color_profile_limits.sh`: staged, retained, and surface-retained budget accounting, plus the `get_information` fd pin.
- `tests/harness/checks/protocol/color_feedback.sh`: parametric validation including `set_tf_power`.
