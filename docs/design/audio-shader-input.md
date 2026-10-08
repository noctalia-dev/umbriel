# Audio shader input

An external producer supplies one normalized level over the
[IPC audio connection](../user/ipc.md#audio-input). The
[shader interface](../user/effects.md#audio-input) is shared by existing effect
kinds. Acquisition and analysis belong to the producer.

`Ipc` owns the producer and reuses its connection deadline for freshness.
`EffectRegistry` retains the latest level and availability. Disconnect, a
stale measurement, a locked session, and an inactive session clear the feed.

## Rendering

A changed pair arms the existing effect timer only on outputs with visible audio consumers.
Each output latches the pair at its effect cadence and passes it into composition.
This keeps a shared scene node's input independent of another output's cadence.

The draw path binds one `vec2` uniform through existing uniform reflection and
binding. Display, capture, feedback, and light passes share the composition's
input. Each node slot remembers its last drawn pair so a closing copy can retain
that value. Live slots continue to use the composing output's input.

A cleared feed also clears output values waiting for a capped frame. Renderer
replacement rebinds the current pair; if recovery exceeds the producer deadline,
the producer reconnects with a fresh measurement. Audio freshness uses real time
and remains independent of the animation clock.
