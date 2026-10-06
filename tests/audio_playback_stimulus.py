#!/usr/bin/env python3
"""Timestamped opt-in playback stimulus and producer-write-to-feature analysis."""

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import select
import struct
import subprocess
import time


RATE, HOP = 48000, 800


def produce(args):
    if not args.permit_playback:
        raise ValueError("playback requires --permit-playback in the disposable session")
    command = ["pw-cat", "--playback", "--raw", "--format", "f32", "--rate", str(RATE),
               "--channels", "2", "--channel-map", "FL,FR", "--latency", str(HOP),
               "--target", args.target, "-"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as log, args.output.with_suffix(".pw-cat.log").open("w") as errors:
        player = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=errors)
        try:
            fd = player.stdin.fileno()
            capacity = fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, 4096)
            os.set_blocking(fd, False)
            log.write(json.dumps({"event": "start", "command": command, "rate": RATE,
                                  "hop": HOP, "pipe_capacity_bytes": capacity,
                                  "cycles": args.cycles if args.pattern == "pulses" else None,
                                  "pattern": args.pattern, "steady_seconds": args.steady_seconds,
                                  "amplitude": .2,
                                  "origin": "first accepted PCM write per edge; not graph/DAC presentation"}) + "\n")
            previous = False
            # Two quiet seconds establish settled zero before the first edge.
            body_hops = args.cycles * 30 if args.pattern == "pulses" else math.ceil(args.steady_seconds * RATE / HOP)
            for hop in range(120 + body_hops + 120):
                active = 120 <= hop < 120 + body_hops and (args.pattern == "tone" or
                         (args.pattern == "pulses" and (hop - 120) % 30 >= 18))
                payload = b"".join(struct.pack("<ff", value, value) for value in
                                   (.2 * math.sin(2 * math.pi * 1000 * (hop * HOP + i) / RATE)
                                    if active else 0.0 for i in range(HOP)))
                edge = active != previous
                offset = 0
                deadline = time.monotonic() + 2
                while offset < len(payload):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([], [fd], [], remaining)[1]:
                        raise RuntimeError("playback pipe made no progress for two seconds")
                    before = time.monotonic_ns()
                    try:
                        written = os.write(fd, payload[offset:])
                    except BlockingIOError:
                        continue
                    after = time.monotonic_ns()
                    if edge and offset == 0:
                        log.write(json.dumps({"event": "edge", "high": active, "frame": hop * HOP,
                                              "write_before_ns": before, "write_after_ns": after}) + "\n")
                        log.flush()
                    offset += written
                    deadline = time.monotonic() + 2
                previous = active
            player.stdin.close()
            if player.wait(timeout=5):
                raise RuntimeError("pw-cat failed; inspect its log")
        finally:
            if player.poll() is None:
                player.kill()
                player.wait()


def analyze(args):
    edges = [r for r in map(json.loads, args.edges.read_text().splitlines()) if r["event"] == "edge"]
    records = list(map(json.loads, args.features.read_text().splitlines()))
    snapshots = [r for r in records if r["event"] == "snapshot"]
    results = []
    for index, edge in enumerate(edges):
        end = edges[index + 1]["write_before_ns"] if index + 1 < len(edges) else edge["write_before_ns"] + 1_000_000_000
        candidates = [r for r in snapshots if edge["write_before_ns"] <= r["received_ns"] < end]
        # Require the opposite state immediately before the edge: already-high
        # unrelated audio must never count as an instantaneous stimulus response.
        prior = [r for r in snapshots if r["received_ns"] < edge["write_before_ns"]]
        matched = None
        reason = "no opposite-state measurement before edge"
        if prior and (prior[-1]["rms"] >= args.threshold) != edge["high"]:
            matched = next((r for r in candidates if (r["rms"] >= args.threshold) == edge["high"]), None)
            reason = "no matching threshold crossing"
            if matched:
                interval = [prior[-1], *[r for r in candidates if r["received_ns"] <= matched["received_ns"]]]
                loss = any(r["event"] == "unavailable" and
                           prior[-1]["received_ns"] <= r["received_ns"] <= matched["received_ns"] for r in records)
                changed = any(r["generation"] != prior[-1]["generation"] for r in interval)
                stale = any((b["received_ns"] - a["received_ns"]) / 1e6 > args.max_gap_ms
                            for a, b in zip(interval, interval[1:]))
                if loss or changed or stale:
                    reason = "source loss, generation replacement, or stale measurements across edge"
                    matched = None
                else:
                    reason = None
        results.append({**edge, "received_ns": matched["received_ns"] if matched else None,
                        "latency_ms": (matched["received_ns"] - edge["write_before_ns"]) / 1e6 if matched else None,
                        "rejection": reason})
    summary = {"scope": "producer first PCM write to received RMS threshold; includes buffering, graph, analysis and transport; excludes physical/acoustic/display claims",
               "threshold": args.threshold, "max_gap_ms": args.max_gap_ms,
               "edges": len(edges), "matched": sum(r["latency_ms"] is not None for r in results)}
    for high, label in ((True, "rise"), (False, "fall")):
        values = sorted(r["latency_ms"] for r in results if r["high"] == high and r["latency_ms"] is not None)
        summary[label] = {"count": len(values), **{name: values[min(len(values) - 1, math.ceil(p * len(values)) - 1)] if values else None
                         for name, p in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99), ("max_ms", 1))}}
    args.output.write_text(json.dumps({"summary": summary, "measurements": results}, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    if not edges or summary["matched"] != len(edges):
        raise RuntimeError("unmatched stimulus edges; measurement is incomplete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    playback = commands.add_parser("produce")
    playback.add_argument("--target", required=True)
    playback.add_argument("--cycles", type=int, default=100)
    playback.add_argument("--pattern", choices=("pulses", "tone", "silence"), default="pulses")
    playback.add_argument("--steady-seconds", type=float, default=30)
    playback.add_argument("--output", type=Path, required=True)
    playback.add_argument("--permit-playback", action="store_true")
    analysis = commands.add_parser("analyze")
    analysis.add_argument("--edges", type=Path, required=True)
    analysis.add_argument("--features", type=Path, required=True)
    analysis.add_argument("--threshold", type=float, default=.05)
    analysis.add_argument("--max-gap-ms", type=float, default=100)
    analysis.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "produce":
        if not 1 <= args.cycles <= 1000:
            parser.error("cycles must be 1..1000")
        if not math.isfinite(args.steady_seconds) or not 0 < args.steady_seconds <= 3600:
            parser.error("steady-seconds must be finite and between zero and 3600")
        produce(args)
    else:
        if not math.isfinite(args.threshold) or not 0 < args.threshold < 1:
            parser.error("threshold must be finite and between zero and one")
        if not math.isfinite(args.max_gap_ms) or args.max_gap_ms <= 0:
            parser.error("max-gap-ms must be finite and positive")
        analyze(args)


if __name__ == "__main__":
    main()
