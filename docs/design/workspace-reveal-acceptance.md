# Workspace reveal validation

The implementation is ready for draft review, with release-performance acceptance
still open. Normal and Tracy builds passed. Five capture-demand unit cases, four
renderer capture cases and four reveal integration checks passed. Sparse/shared
capture pixel checks passed once and in four concurrent stress repetitions.

Manual testing confirms everything is working well: multiple slides in all
directions, across many different applications, including active content such as
YouTube. This is user-reported coverage in the native reveal session.

## Comparable capture measurements

Three isolated baseline/candidate pairs per transport used Intel ARL, Mesa 26.2.4,
2560×1440@60 headless output, scale 1 SDR, two live workspaces, three-pass radius-12
blur, a held half transition and effects excluded from capture. The baseline is the
same reveal implementation without capture-demand optimization. Eight-second
samples ran with Tracy disconnected; variant order reversed in round two.

| Transport | Baseline captures | Candidate captures |
| --- | --- | --- |
| SHM | 201, 199, 198 | 199, 199, 200 |
| DMA-BUF | 58, 120, 87 | 234, 236, 234 |

Median DMA-BUF completed captures increased 2.69×. Median per-run mean fence wait
fell from 83.34 to 18.80 ms. SHM capture throughput was unchanged; small differences
in client updates and settled idle CPU remain unresolved. These counts are not
physical presentation FPS. The DMA-BUF helper reuses one buffer, waits for implicit
fences and does not encode video or verify captured pixel content.

A matched Tracy 0.13.1 diagnostic pair placed GPU submit drawing timestamps before
flush/synchronization. That scope was identical between versions (p99 0.000261 ms)
and blur p99 was 1.785/1.788 ms. The scope is nearly empty for SDR without an output
color transform; it is not whole-frame GPU time. The earlier broad submit zone
straddled a flush boundary, so its tail alone does not establish drawing cost.
Synchronization costs are not proved harmless by excluding them from this scope.
CPU render p99 was 1.814/2.320 ms, with maxima 75.348/2.556 ms; candidate texture
zone tails remained higher. The diagnostic instrumentation is not retained.

## Remaining limits

An agreed compositor workload budget, physical presentation, NVIDIA coverage and
a real recorder with a buffer ring are not established. The refresh interval is
not a compositor-work budget. Existing Tracy format warnings remain; project-wide
lint/test acceptance is not claimed. The bounded comparison supports retaining the
optimization for draft review, not unconditional release acceptance.
