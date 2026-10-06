#!/usr/bin/env python3
"""Explicit opt-in probe for a disposable audio test session; stores features, never PCM."""

import argparse
import json
import math
import os
from pathlib import Path
import resource
import socket
import struct
import subprocess
import time


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True)
    parser.add_argument("--helper-arg", action="append", default=[])
    parser.add_argument("--mode", choices=("playback", "microphone"), required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--target")
    selection.add_argument("--follow-default", action="store_true")
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--permit-device-acquisition", action="store_true")
    parser.add_argument("--require-snapshots", action="store_true")
    args = parser.parse_args()
    if not args.permit_device_acquisition:
        parser.error("device acquisition requires --permit-device-acquisition in the disposable session")
    if not math.isfinite(args.duration) or not 0 < args.duration <= 3600:
        parser.error("duration must be between zero and 3600 seconds")
    target = (args.target or "").encode("utf-8")
    if len(target) > 1024 or b"\0" in target or (args.target is not None and not target):
        parser.error("target must contain 1..1024 UTF-8 bytes without NUL")
    source = 1 if args.mode == "playback" else 2
    epoch = time.monotonic_ns() or 1
    configuration = bytearray(32 + len(target))
    configuration[:4] = b"UAF1"
    struct.pack_into("<HHHH", configuration, 4, 1, 1, len(configuration), 1)
    struct.pack_into("<QHHH", configuration, 16, epoch, source, 2 if args.follow_default else 1, len(target))
    configuration[32:] = target
    host, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    ages, gaps = [], []
    counts = {"ready": 0, "snapshot": 0, "heartbeat": 0, "unavailable": 0}
    previous = None
    status_generation = 0
    unavailable_generation = 0
    peak_rss_kib = 0
    rss_sampled_ns = 0
    started = time.monotonic_ns()
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as output, args.output.with_suffix(".helper.log").open("w") as log:
        helper = subprocess.Popen([args.helper, *args.helper_arg], stdin=child,
                                  stdout=subprocess.DEVNULL, stderr=log)
        child.close()
        host.settimeout(.25)
        output.write(json.dumps({"event": "start", "monotonic_ns": started, "pid": helper.pid,
                                 "mode": args.mode, "target": args.target, "follow_default": args.follow_default,
                                 "pipewire_remote": os.environ.get("PIPEWIRE_REMOTE", "default")}) + "\n")
        output.flush()
        try:
            host.sendall(configuration)
            while time.monotonic_ns() - started < args.duration * 1e9:
                if time.monotonic_ns() - rss_sampled_ns >= 100_000_000:
                    try:
                        status = Path(f"/proc/{helper.pid}/status").read_text()
                        peak_rss_kib = max(peak_rss_kib, next(int(line.split()[1]) for line in status.splitlines()
                                                            if line.startswith("VmRSS:")))
                    except (FileNotFoundError, StopIteration):
                        pass
                    rss_sampled_ns = time.monotonic_ns()
                try:
                    packet, _, flags, _ = host.recvmsg(256)
                except TimeoutError:
                    if helper.poll() is not None:
                        raise RuntimeError(f"helper exited {helper.returncode}")
                    continue
                received = time.monotonic_ns()
                if not packet:
                    raise RuntimeError("helper closed the channel")
                assert not flags & socket.MSG_TRUNC, "truncated provider packet"
                magic, version, kind, length, profile, reserved = struct.unpack_from("<4sHHHHI", packet)
                assert (magic, version, length, profile, reserved) == (b"UAF1", 1, len(packet), 1, 0)
                assert struct.unpack_from("<Q", packet, 16)[0] == epoch, "wrong epoch"
                record = {"received_ns": received}
                if kind == 2:
                    assert len(packet) == 32 and not counts["ready"], "invalid or duplicate READY"
                    assert struct.unpack_from("<H", packet, 24)[0] == source, "wrong negotiated source type"
                    assert packet[26:] == bytes(6)
                    record["event"] = "ready"
                elif kind == 3:
                    assert counts["ready"] == 1 and len(packet) == 124
                    generation, sequence, observation = struct.unpack_from("<QQQ", packet, 24)
                    features = struct.unpack_from("<19f", packet, 48)
                    assert generation > 0 and sequence > 0 and observation > 0
                    assert generation >= status_generation, "snapshot predates published status"
                    assert generation > unavailable_generation, "source loss reused an old generation"
                    assert all(math.isfinite(value) and 0 <= value <= 1 for value in features)
                    age = (received - observation) / 1e6
                    assert age >= -10, "observation is in the future"
                    if previous is not None:
                        assert generation >= previous[0], "generation regressed"
                        if generation == previous[0]:
                            assert sequence > previous[1] and observation > previous[2], "stale measurement"
                            gaps.append((observation - previous[2]) / 1e6)
                    previous = (generation, sequence, observation)
                    ages.append(age)
                    record.update(event="snapshot", generation=generation, sequence=sequence,
                                  observation_ns=observation, transport_age_ms=age,
                                  rms=features[0], peak=features[1], envelope=features[2], bands=features[3:])
                else:
                    assert counts["ready"] == 1 and kind in (4, 5) and len(packet) == 32
                    generation = struct.unpack_from("<Q", packet, 24)[0]
                    assert generation >= status_generation, "status generation regressed"
                    assert previous is None or generation >= previous[0], "status predates a snapshot"
                    status_generation = generation
                    if kind == 5:
                        unavailable_generation = generation
                    record.update(event="heartbeat" if kind == 4 else "unavailable",
                                  generation=generation)
                counts[record["event"]] += 1
                output.write(json.dumps(record) + "\n")
                output.flush()
        finally:
            host.close()
            try:
                result = helper.wait(timeout=2)
            except subprocess.TimeoutExpired:
                helper.kill()
                helper.wait()
                raise RuntimeError("helper ignored EOF for two seconds") from None
        assert result == 0, f"helper EOF exit {result}"
        assert counts["ready"] == 1, "provider did not negotiate READY"
        if args.require_snapshots:
            assert counts["snapshot"] > 0, "provider never acquired an available measurement"
    elapsed = (time.monotonic_ns() - started) / 1e9
    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = cpu_after.ru_utime + cpu_after.ru_stime - cpu_before.ru_utime - cpu_before.ru_stime
    summary = {"elapsed_seconds": elapsed, "helper_cpu_seconds": cpu, "helper_cpu_percent_one_core": 100 * cpu / elapsed,
               "helper_peak_sampled_rss_kib": peak_rss_kib,
               "packets": counts, "snapshot_hz": counts["snapshot"] / elapsed,
               "transport_age_ms_p50": percentile(ages, .5), "transport_age_ms_p95": percentile(ages, .95),
               "transport_age_ms_max": max(ages, default=None), "observation_gap_ms_p95": percentile(gaps, .95),
               "note": "Transport age excludes acquisition, analysis-window, compositor, display, and acoustic latency."}
    args.output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
