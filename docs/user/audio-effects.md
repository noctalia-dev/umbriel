# Audio inputs for effects

An effect can bind one named audio source. Sources are shared across consumers;
declaring a source or compiling a preset does not start recording. Acquisition
starts when an eligible effect actually uses its audio uniforms and stops when
its last consumer disappears. Missing providers leave audio unavailable while
ordinary rendering continues.

## Quick start

The optional `umbriel-audio` helper uses PipeWire 1.0.5 or later. Build it with
Meson's `-Daudio_helper=enabled`, or the Nix package's `enableAudio = true`
override, and keep it on the compositor's `PATH`.

Include a bundled preset and select it:

```toml
[include]
files = ["/usr/share/umbriel/effects/border/spectrum/effect.toml"]

[effects]
border = "spectrum"
```

The path is the installed effect directory, `<prefix>/share/umbriel/effects`.
Play something and the border responds. The bundled presets `spectrum`,
`border.music-lines`, `cursor.music-radiance` and `window.music-smoke` all
follow system playback. To try one without editing the configuration, select it at
runtime with `umbriel msg effect-border-set:spectrum` and clear it with
`umbriel msg effect-border-reset`.

## Sources

Playback and microphone are distinct source modes; they never substitute for one
another.

```toml
[effects.audio.sources.desktop]
provider = "pipewire"
mode = "playback"
follow_default = true

[effects.audio.sources.voice]
provider = "pipewire"
mode = "microphone"
target = "the-explicit-node-name"

[effects.preset.music_border]
kind = "border"
shader = "music-border.glsl"
audio = "desktop"
animated = false

[effects]
border = "music_border"
```

Each source must select exactly one fixed `target` or `follow_default = true`.
Following a default is an explicit choice within the selected source type: the
source follows `default.audio.sink` in playback mode and `default.audio.source`
in microphone mode, and moves when either changes. A fixed target is the node's
`node.name`. It is unavailable while that device is missing and resumes when a
device with that name returns. List the names with:

```sh
pw-dump | jq -r '.[] | select(.type == "PipeWire:Interface:Node") | .info.props["node.name"] // empty'
```

An external provider uses `provider = "external"`, an `executable` path and an
optional `args` array. Relative executable paths resolve beside their declaring
configuration file. Arguments are passed directly, including empty strings;
there is no shell expansion. The provider receives a private Unix seqpacket
channel on standard input and must implement the versioned audio protocol in
`src/audio/protocol.h`. It must negotiate the requested source type and stop
acquisition when the channel closes.

## Shader inputs

The helpers work in every existing effect kind and require no `umbriel_time`
reference. A border's `animated = false` or `speed = 0` freezes its time input;
it does not disable its separately selected audio source.

| GLSL helper | Value |
| --- | --- |
| `umbriel_audio_available()` | 1 for a current measurement, otherwise 0 |
| `umbriel_audio_rms()` | RMS amplitude across channels |
| `umbriel_audio_peak()` | Largest absolute sample |
| `umbriel_audio_level()` or `umbriel_audio_envelope()` | RMS with 10 ms attack and 150 ms release |
| `umbriel_audio_band_at(int index)` | Discrete band, index clamped to 0–15 |
| `umbriel_audio_band(float position)` | Interpolation across bands, position clamped to 0–1 |

All amplitudes are linear values in [0,1], quantized in increments of 1/65535.
There is no automatic gain, beat detector, BPM estimate or waveform input.
Ordinary music sits low in that range and carries less energy in the higher
bands, so shaders scale what they read; the bundled ones multiply RMS by 16 and
each band by 24 to 32 before taking a square root.

Analysis uses 48 kHz audio, a 2048-sample periodic Hann window and an 800-sample
hop. Frequency edges are 20, 40, 80, 120, 180, 270, 400, 600, 900, 1350, 2000,
3000, 4500, 6750, 10000, 15000 and 20000 Hz. Low bands share the FFT's coarse
frequency resolution. Channel power averaging preserves anti-phase stereo.
The analysis window spans about 42.7 ms.

For example, this border responds without a running shader clock:

```glsl
vec4 border(vec2 uv) {
    float strength = umbriel_audio_level();
    return vec4(0.1 + 0.9 * strength, 0.2, 1.0 - strength * 0.5, 1.0);
}
```

Audio-only frames follow `effects.max_fps`. Unchanged silence does not request
additional effect frames. Provider loss marks input unavailable and fades held
amplitudes to exact zero over 150 ms. Heartbeats do not refresh measurements;
measurements become stale after 250 ms without a new valid snapshot. Lock and
inactive session clear inputs and stop providers.

## Inspecting a source

`umbriel effects --json` reports every configured source under `audio` without
starting acquisition: `name`, `state`, `mode`, `demanded`, `ready`, `available`,
`epoch`, `generation`, `sequence`, `observation_ns` and `age_ns`, plus
`audio_demanded_sources`. While the session is locked, sources report `unused`.
At most four sources are acquired at once; a consumer of a fifth reports
`source_limit`.

| `state` | Meaning |
| --- | --- |
| `unused` | No visible effect uses the source; nothing is running |
| `starting` | The helper is running and has not reported yet |
| `available` | Fresh measurements are arriving |
| `unavailable` | The helper is running but has no device or no current measurement |
| `suspended` | The session is inactive while the source is still demanded |
| `stopping` | The helper is shutting down |
| `retrying`, `retry_exhausted`, `launch_failed` | The helper exited or failed to start |
| `helper_missing` | `umbriel-audio` is not on `PATH` |
| `source_limit` | Four other sources are already acquired |
| `retiring` | A configuration reload is waiting for the previous helper to exit |

## Cost and responsiveness

Measured with a release build on an AMD Ryzen 9 6900HX with PipeWire 1.6.9,
stereo 48 kHz playback:

| | |
| --- | --- |
| Helper CPU while acquiring | about 3% of one core; a mono microphone about 1.6%; about 0.2% with no device |
| Helper memory | about 9 MB |
| Compositor CPU | no measurable change with one border effect |
| Analysis cadence | one window every 16.7 ms |
| Player's first sample to an RMS of 0.05 | rising 53 ms median, 64 ms at the 95th percentile; falling 85 ms median, 96 ms |
| Source `available` after an effect is selected | about 0.1 s |
| After the session unlocks | about 0.1 s |
| After a fixed device reappears | about 60 ms |
| After the default device changes | about 90 ms |
| After PipeWire restarts | about 1.1 s |

The latency figures exclude display latency. The reappearance and default-device
times are single observations. The helper stops within a second of
the session locking and when the last effect using the source is cleared.
`tests/audio_device_probe.py` records the measurements for a given setup.
