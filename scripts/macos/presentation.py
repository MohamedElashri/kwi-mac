#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Correlate input/Qt-frame IDs with Metal's software-reported presentation."""
import argparse
import json
import math
from pathlib import Path


def percentile(values, fraction):
    return sorted(values)[math.ceil(len(values) * fraction) - 1] if values else None


def presented_trace_time_ns(frame):
    if frame["presented_seconds"] <= 0:
        return None
    return frame["trace_now_ns"] - (frame["ca_now_seconds"] - frame["presented_seconds"]) * 1e9


def correlate(events, drawables):
    by_sequence = {frame["sequence"]: frame for frame in drawables}
    pairs = []
    missing = skipped = 0
    for event in events:
        frame = by_sequence.get(event["drawable_sequence"])
        if frame is None:
            missing += 1
            continue
        if frame["presented_seconds"] <= 0:
            skipped += 1
            continue
        # Metal and CACurrentMediaTime use host seconds. Map that timestamp to
        # the app's MONOTONIC_RAW clock using adjacent reads in the handler.
        presented = presented_trace_time_ns(frame)
        latency = (presented - event["event_trace_time_ns"]) / 1e6
        if latency < 0:
            raise ValueError("Presented drawable predates its correlated input")
        pairs.append({**event, "presented_trace_time_ns": round(presented), "event_to_presented_ms": latency})
    return pairs, missing, skipped


def response_frames(events, drawables):
    """Keep pacing specific to the layer that actually responds to input."""
    sequences = {event["drawable_sequence"] for event in events}
    first, last = min(sequences), max(sequences)
    frames = [frame for frame in drawables if first <= frame["sequence"] <= last]
    if not any("layer_id" in frame for frame in frames):
        # Historical single-window traces predate layer identity metadata.
        return frames, None
    if any("layer_id" not in frame for frame in frames):
        raise ValueError("Incomplete presentation layer metadata")
    layers = {frame["layer_id"] for frame in frames if frame["sequence"] in sequences}
    if len(layers) > 1:
        raise ValueError("Response samples span multiple presentation layers")
    layer = next(iter(layers), None)
    return [frame for frame in frames if frame["layer_id"] == layer], layer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--contract", type=Path, default=Path(__file__).resolve().parents[2] / "tests/macos/performance-contract.json")
    args = parser.parse_args()
    directory = args.directory.resolve()
    contract = json.loads(args.contract.read_text())
    drawables = [json.loads(line) for line in (directory / "drawables.jsonl").read_text().splitlines()]
    if len({frame["sequence"] for frame in drawables}) != len(drawables):
        raise ValueError("Duplicate drawable acquisition sequence")
    results = []
    for path in sorted(directory.glob("interactive-*.json")):
        sample = json.loads(path.read_text())
        events = sample["sample_events"]
        if not events or len({e["drawable_sequence"] for e in events}) != len(events):
            raise ValueError("Response samples do not identify distinct drawable frames")
        pairs, missing, skipped = correlate(events, drawables)
        latencies = [p["event_to_presented_ms"] for p in pairs]
        frames, layer = response_frames(events, drawables)
        times = sorted(f["presented_seconds"] * 1e3 for f in frames if f["presented_seconds"] > 0)
        intervals = [after - before for before, after in zip(times, times[1:])]
        result = {"case": sample["case"], "sample_count": len(events), "matched_presented": len(pairs),
                  "presentation_layer_id": layer,
                  "missing_drawables": missing, "skipped_response_drawables": skipped,
                  "skipped_drawables_in_window": sum(f["presented_seconds"] <= 0 for f in frames),
                  "event_to_presented_p50_ms": percentile(latencies, .5),
                  "event_to_presented_p95_ms": percentile(latencies, .95),
                  "event_to_presented_p99_ms": percentile(latencies, .99),
                  "presented_frame_interval_p95_ms": percentile(intervals, .95),
                  "presented_frame_interval_p99_ms": percentile(intervals, .99),
                  "missed_display_intervals": sum(v > 1.5 * 1000 / 60 for v in intervals)}
        result["input_target_met"] = bool(not missing and not skipped and latencies
                                          and result["event_to_presented_p95_ms"] <= contract["input_to_response_p95_ms"]
                                          and result["event_to_presented_p99_ms"] <= contract["input_to_response_p99_ms"])
        result["frame_target_met"] = bool(intervals and result["presented_frame_interval_p95_ms"] <= contract["frame_interval_p95_ms"]
                                          and result["presented_frame_interval_p99_ms"] <= contract["frame_interval_p99_ms"])
        (directory / f"paired-{sample['case']}.json").write_text(json.dumps(pairs, indent=2) + "\n")
        results.append(result)
    if not results:
        raise ValueError("No interactive response records")
    summary = {"scope": "Qt input event to matched CAMetalDrawable.presentedTime; software-reported display presentation, not a photodiode/camera measurement.",
               "contract": str(args.contract), "cases": results}
    (directory / "presentation-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
