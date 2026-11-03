#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""First-bundle startup/output diagnostics, with logical completion and frame callbacks."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import statistics
import subprocess
import time

from smoke import records, trace_clock_ns


def percentile(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, max(0, math.ceil(len(values) * p) - 1))] if values else None


def measure(app, workload, directory):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "trace.log"
    env = {k: os.environ[k] for k in ("HOME", "USER", "LOGNAME") if k in os.environ}
    env.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8", KWI_TRACE="1", KWI_TRACE_MARKER="KWI_WORKLOAD_DONE")
    command = '/bin/cat "$1"; printf "\\030\\033[0m\\r\\nKWI_WORKLOAD_DONE\\r\\n"'
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = trace_clock_ns()
    process = None
    try:
        with path.open("w") as stream:
            process = subprocess.Popen([str(app / "Contents/MacOS/kwi"), "--hold", "-e", "/bin/sh", "-c", command, "kwi-benchmark", str(workload)],
                                       env=env, stdout=stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 120
            events = []
            pending = ""
            marker_event = None
            prepared_event = None
            frame_time = None
            with path.open(encoding="utf-8", errors="replace") as reader:
                while time.monotonic() < deadline:
                    pending += reader.read()
                    split = pending.rfind("\n")
                    chunk, pending = pending[:split + 1], pending[split + 1:]
                    new = records(chunk)
                    events.extend(new)
                    for event in new:
                        if event["kind"] == "marker-snapshot":
                            marker_event, prepared_event = event, None
                        if event["kind"] == "render" and marker_event and event["terminal_id"] == marker_event["terminal_id"] and event["generation"] >= marker_event["generation"]:
                            prepared_event = event
                        if event["kind"] == "frame-swapped" and prepared_event is not None and event["time_ns"] >= prepared_event["time_ns"]:
                            frame_time = event["time_ns"]
                            break
                    if frame_time is not None:
                        break
                    if process.poll() is not None:
                        raise RuntimeError(f"App exited before logical completion: {path}")
                    time.sleep(0.1)
                else:
                    raise RuntimeError(f"No logical marker/frame before timeout: {path}")
            markers = [e for e in events if e["kind"] == "marker-snapshot"]
            frames = [e for e in events if e["kind"] == "frame-swapped"]
            feeds = [e for e in events if e["kind"] == "feed"]
            snapshots = [e for e in events if e["kind"] == "snapshot"]
            render = [e for e in events if e["kind"] == "render"]
            configuration = [e for e in events if e["kind"] == "configuration"]
            windows = [e for e in events if e["kind"] == "window"]
            startup = next(e for e in events if e["kind"] == "startup")
            completed = frame_time
            first_feed = feeds[0]["start_ns"]
            bytes_read = sum(e["bytes"] for e in feeds)
            active_frames = [e["time_ns"] for e in frames if first_feed <= e["time_ns"] <= completed]
            frame_intervals = [(right - left) / 1e6 for left, right in zip(active_frames, active_frames[1:])]
            result = {"spawn_to_first_frame_callback_ms": (frames[0]["time_ns"] - started) / 1e6,
                      "spawn_to_main_ms": (startup["main_start_ns"] - started) / 1e6,
                      "main_to_first_frame_callback_ms": (frames[0]["time_ns"] - startup["main_start_ns"]) / 1e6,
                      "first_feed_to_marker_frame_ms": (completed - first_feed) / 1e6,
                      "pty_bytes": bytes_read, "feed_calls": len(feeds),
                      "pty_mib_per_second": bytes_read / 1024**2 / ((completed - first_feed) / 1e9),
                      "feed_ms_p95": percentile([e["elapsed_ns"] / 1e6 for e in feeds], .95),
                      "feed_ms_max": max(e["elapsed_ns"] for e in feeds) / 1e6,
                      "snapshot_ms_p95": percentile([e["elapsed_ns"] / 1e6 for e in snapshots], .95),
                      "prepare_ms_p95": percentile([e["prepare_ns"] / 1e6 for e in render], .95),
                      "output_frame_callback_interval_ms_p95": percentile(frame_intervals, .95),
                      "output_frame_callback_interval_ms_max": max(frame_intervals) if frame_intervals else None,
                      "viewport": {k: snapshots[-1][k] for k in ("columns", "rows")},
                      "window": {k: windows[-1][k] for k in ("width", "height", "dpr", "refresh_hz", "accessibility_active")},
                      "prepared_marker_generation": prepared_event["generation"],
                      "render_counters_at_completion": {k: prepared_event[k] for k in
                                                        ("shaped_runs", "rasterized_glyphs", "upload_bytes", "rebuilt_rows", "atlas_bytes")},
                      "logical_marker_and_following_frame": True}
            result["configuration"] = configuration
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    result["cpu_seconds_app_and_descendants"] = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    result["trace_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=5)
    args = parser.parse_args()
    app, workload = args.app.resolve(), args.workload.resolve()
    if not 1 <= args.iterations <= 20:
        parser.error("Use 1–20 measured iterations")
    work = args.output.resolve().parent
    warmup = measure(app, workload, work / "warmup")
    runs = []
    for n in range(args.iterations):
        runs.append(measure(app, workload, work / f"run-{n + 1}"))
        print(f"Run {n + 1}: {runs[-1]['pty_mib_per_second']:.2f} MiB/s", flush=True)
    result = {"scope": "Diagnostic pilot; traces on, actual viewport, default history. Not the frozen Phase 2A performance gate.",
              "percentile_method": "nearest rank",
              "executable_sha256": hashlib.sha256((app / "Contents/MacOS/kwi").read_bytes()).hexdigest(),
              "bundle_inputs_sha256": hashlib.sha256((app / "Contents/Resources/kwi-bundle.json").read_bytes()).hexdigest(),
              "limits": "Logical snapshot and frame-swapped callback, not GPU/pixel presentation or input latency; logging overhead is uncalibrated.",
              "workload": str(workload), "bytes": workload.stat().st_size, "sha256": hashlib.sha256(workload.read_bytes()).hexdigest(),
              "warmup": warmup, "runs": runs,
              "median_pty_mib_per_second": statistics.median(r["pty_mib_per_second"] for r in runs)}
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
