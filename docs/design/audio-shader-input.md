# Audio shader input

An external producer supplies a normalized 64-band spectrum over the [IPC audio connection](../user/ipc.md#audio-input). The [shader interface](../user/effects.md#audio-input) is shared by existing effect kinds. Acquisition and analysis belong to the producer.

`Ipc` owns the producer and reuses its connection deadline for freshness. `EffectRegistry` retains the latest spectrum and availability. Disconnect, a stale measurement, a locked session, and an inactive session clear the feed.

## Rendering

A changed snapshot arms the existing effect timer only on outputs with visible audio consumers. Each output latches the snapshot at its effect cadence and passes it into composition. This keeps a shared scene node's input independent of another output's cadence.

Each output owns a 64×1, 8-bit luminance texture with nearest filtering, allocated on its first spectrum-reading draw. Changed bands update that texture; the shader samples adjacent texels and interpolates them. Availability remains a float uniform. Display, capture, feedback, light passes, and closing copies use the composing output's input. Texture lifetime follows the output's renderer resources.

A cleared feed also clears output values waiting for a capped frame. Renderer replacement rebinds the current snapshot; if recovery exceeds the producer deadline, the producer reconnects with a fresh measurement. Audio freshness uses real time and remains independent of the animation clock.
