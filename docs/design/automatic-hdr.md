# Automatic HDR color feedback

The color-management protocol describes a logical encoding independently of the destination color volume. Umbriel offers a PQ encoding in automatic HDR mode on outputs that can activate HDR, even while their current scanout is SDR. This prevents a startup dependency with clients such as Wine: the client needs HDR headroom before creating HDR content, while physical activation needs that content to arrive first.

`ColorManager::descriptionForOutput` selects the logical description, and both per-output information and preferred surface information use it. An idle automatic HDR output offers BT.2020 PQ with a primary maximum of 10000 cd/m² and the configured SDR reference white. Its target remains sRGB with the nominal SDR luminance range. Active HDR replaces that target with the output's display metadata. Unsupported outputs and disabled HDR policies retain SDR descriptions.

The description objects are immutable. Returning to idle changes the target back to SDR while retaining logical HDR headroom, so a new client can still start HDR. Changing `sdr_white` updates the logical reference white even while scanout remains SDR. Reusing an output or preferred PQ description as surface metadata retains its reference luminance and requests automatic HDR just like a client-created PQ description.

## Capability and activation

Output configuration tests an HDR state through the backend using the display's PQ and BT.2020 support, renderer output transforms and available 10-bit formats. The test uses a separate pending state and can try without VRR. It does not commit that state or change the scene's SDR white level.

Physical activation remains tied to a mapped fullscreen view with committed HDR metadata and content headroom. A PQ encoding can also carry SDR content: Chromium's [preferred color-space handling](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/ozone/platform/wayland/host/wayland_wp_image_description.cc) uses that encoding for ordinary desktop and video compositions. Its [description builder](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/ozone/platform/wayland/host/wayland_wp_color_manager.cc) defaults MaxCLL and MaxFALL to SDR reference white when content-light metadata is absent. Those surfaces do not request automatic HDR merely because their encoding is PQ. `colorDescriptionRequestsHdrOutput` checks the content peak against the surface description's own reference luminance before requesting activation.

A positive MaxCLL at or below reference white suppresses activation unless the client also supplies mastering luminance above reference white. That exception preserves HDR video whose content-light values default to SDR white but whose mastering metadata still identifies HDR content. MaxCLL zero means unknown, so existing encoding and luminance headroom remain the fallback for Wine, Proton, and other clients without content-light metadata. MaxFALL alone never rules out HDR highlights. Color decoding and HDR capability feedback retain the original surface encoding regardless of the physical activation decision.

The output frame builds the scene buffer and commits it together with the pending HDR encoding and render format. Its pixels therefore use the new encoding from the first presented HDR frame.

A real activation rejection withdraws logical HDR availability even if SDR fallback succeeds. Ordinary idle commits preserve that rejection; clearing a diagnostic fallback reason does not re-advertise HDR. A deliberate output reconfiguration or mode change probes support again. A successful physical HDR commit also restores availability. Retryable scene-build failures do not invalidate previously tested capability.

## SDR fallback

The scene marks HDR sources for SDR mapping when their destination is SDR. This includes PQ, absolute-luminance scRGB and descriptions with luminance headroom. The texture shader decodes and converts straight RGB before applying a continuous Reinhard shoulder above linear 0.8. Scaling all channels together retains their ratios. It then restores premultiplied alpha before blending. Ordinary SDR sources and intermediate textures leave the mapping disabled.

The pending output state selects this policy, so a static source buffer moves between SDR mapping and HDR presentation without requiring a client repaint. Window capture scenes use the same SDR mapping. The additional texture option belongs to UmbrielFX; shared wlroots structure prefixes remain unchanged.

## Verification

`color-description` covers logical headroom across idle, active, returned-idle and unavailable states, truthful destination metadata and reference-white changes. `color-scene-hdr-sdr-fallback` renders SDR/HDR/SDR transitions and checks distinct highlights, PQ/scRGB equivalence, custom luminances and alpha. Headless protocol checks verify that unsupported outputs continue to offer SDR. `protocol/color_content_hdr` commits SDR-limited, HDR, unknown-peak, and mastering-only HDR descriptions through top-level and child surfaces. It checks automatic requests in both directions, fullscreen eligibility, and preserved PQ metadata without requiring hardware HDR activation.

Real DRM verification must also cover a fresh Proton startup in automatic mode, fullscreen activation, return to idle, windowed fallback and mixed SDR/HDR outputs. Headless outputs cannot establish successful hardware activation.
