#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Controlled /bin/sh startup; Qt prepared-frame callbacks are not pixels."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

from app_test_support import executable, isolated_profile, process_path, ensure_kwi_not_running
from smoke import records, completion, trace_clock_ns
from presentation import presented_trace_time_ns


def measure(app, directory, home, child_helper, presentation_library=None):
    directory.mkdir()
    isolated_profile(home, 741, 510, 1, 100000)
    lock = Path(f"/tmp/com.elashri.zirq-{os.geteuid()}.socket.lock")
    ensure_kwi_not_running(lock)
    env = {k: os.environ[k] for k in ("USER", "LOGNAME") if k in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8",
               ZIRQ_TRACE="2", ZIRQ_TRACE_MARKER="KWI_STARTUP_SETTLED")
    if presentation_library:
        env.update(DYLD_INSERT_LIBRARIES=str(presentation_library), KWI_PRESENTATION_TRACE=str(directory / "drawables.jsonl"))
    marker = directory / "ready.json"
    binary = executable(app)
    started = trace_clock_ns()
    with (directory / "app.log").open("w") as log:
        process = subprocess.Popen([str(binary), "--hold", "-e", "/bin/sh", "-c",
                                    'printf "KWI_STARTUP_READY\\n"; exec "$1" "$2" 120 40',
                                    "kwi-startup", str(child_helper), str(directory)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            ready = None
            while time.monotonic() < deadline:
                if ready is None and marker.exists():
                    ready = json.loads(marker.read_text())["time_ns"]
                events = records((directory / "app.log").read_text(errors="replace"))
                completed = completion(events)
                if ready and completed and (directory / "settled.json").exists():
                    break
                if process.poll() is not None:
                    raise RuntimeError(f"Owned app exited {process.returncode}")
                time.sleep(.005)
            else:
                raise TimeoutError("Shell marker was not prepared and followed by a frame callback")
            if not process_path(process.pid).samefile(binary) or int(lock.read_text().splitlines()[0]) != process.pid:
                raise RuntimeError("The measured app does not own its process/activation endpoint")
            settled = json.loads((directory / "settled.json").read_text())
            geometry = [settled["rows"], settled["columns"]]
            if geometry != [40, 120]:
                raise RuntimeError(f"Startup viewport differs: {geometry}")
            first_render = next(e for e in events if e["kind"] == "render")
            first_frame = next(e for e in events if e["kind"] == "frame-swapped" and e["time_ns"] >= first_render["time_ns"])
            result = {"shell_ready_observed_ms": (ready - started) / 1e6,
                      "first_terminal_prepared_callback_ms": (first_frame["time_ns"] - started) / 1e6,
                      "shell_marker_prepared_callback_ms": (completed[2]["time_ns"] - started) / 1e6,
                      "prepared_shell_marker": completed, "pid": process.pid,
                      "rows_columns": geometry,
                      "initial_pty_geometry": json.loads(marker.read_text()),
                      "physical_presentation_measured": False, "ready_observer_poll_ms": 5}
            if presentation_library:
                # The marker stays unchanged in this controlled child. If an
                # initial drawable is skipped, a later frame still contains it.
                for _ in range(500):
                    events = records((directory / "app.log").read_text(errors="replace"))
                    try:
                        frames = {f["sequence"]: f for f in map(json.loads, (directory / "drawables.jsonl").read_text().splitlines())}
                    except (FileNotFoundError, json.JSONDecodeError):
                        frames = {}
                    matched = []
                    for cutoff in (first_render["time_ns"], completed[1]["time_ns"]):
                        candidates = [frames[e["drawable_sequence"]] for e in events if e["kind"] == "frame-swapped" and e["time_ns"] >= cutoff
                                      and e.get("drawable_sequence") in frames and frames[e["drawable_sequence"]]["presented_seconds"] > 0]
                        matched.append(min(candidates, key=lambda f: f["presented_seconds"]) if candidates else None)
                    if all(matched):
                        result["terminal_software_presented_ms"] = (presented_trace_time_ns(matched[0]) - started) / 1e6
                        result["shell_marker_software_presented_ms"] = (presented_trace_time_ns(matched[1]) - started) / 1e6
                        result["presented_drawable_sequences"] = [f["sequence"] for f in matched]
                        break
                    time.sleep(.01)
                else:
                    raise TimeoutError("No matched software-presented startup frame")
            (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            return result
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fresh-profile-per-run", action="store_true")
    parser.add_argument("--presentation-library", type=Path)
    parser.add_argument("--child-helper", type=Path, required=True)
    args = parser.parse_args()
    app, output = args.app.resolve(), args.output.resolve()
    args.child_helper = args.child_helper.resolve()
    if not args.child_helper.is_file():
        parser.error("Build test-macos-startup-child before measuring startup")
    if args.presentation_library:
        args.presentation_library = args.presentation_library.resolve()
        if not args.presentation_library.is_file():
            parser.error("Presentation library does not exist")
    output.mkdir(parents=True, exist_ok=False)
    runs = []
    warmup = None
    for n in range(6):
        home = output / (f"profile-{n}" if args.fresh_profile_per_run else "profile-home")
        result = measure(app, output / ("warmup" if n == 0 else f"run-{n}"), home, args.child_helper, args.presentation_library)
        if n == 0:
            warmup = result
        else:
            runs.append(result)
            print(f"{n}: shell marker prepared callback {result['shell_marker_prepared_callback_ms']:.1f} ms", flush=True)
    result = {"app": str(app), "executable_sha256": hashlib.sha256(executable(app).read_bytes()).hexdigest(),
              "scope": "Controlled /bin/sh without user startup files; coarse tracing; warm OS/font/Metal caches. Fresh app profile/cache per run only when requested; no OS-cache-cold claim.",
              "fresh_profile_per_run": args.fresh_profile_per_run, "warmup": warmup, "runs": runs,
              "median_shell_marker_prepared_callback_ms": statistics.median(r['shell_marker_prepared_callback_ms'] for r in runs)}
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
