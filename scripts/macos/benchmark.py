#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Isolated Mac-only Zirq/Ghostty parser calibration; DSR is not presentation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time

from smoke import records, completion, trace_clock_ns
from app_test_support import activate_owned_app, ensure_kwi_not_running, executable, isolated_profile, process_path, wait_json

ROOT = Path(__file__).resolve().parents[2]


def measure(args, directory, home):
    if args.terminal == "zirq":
        ensure_kwi_not_running()
    directory.mkdir(parents=True, exist_ok=False)
    isolated_profile(home, args.width, args.height, args.opacity, args.history)
    env = {k: os.environ[k] for k in ("USER", "LOGNAME") if k in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8")
    child = [sys.executable, str(ROOT / "scripts/macos/pty-workload.py"), "--directory", str(directory)]
    if args.workload:
        child += ["--workload", str(args.workload)]
    if args.keep_viewport:
        child += ["--keep-viewport"]
    if args.seconds:
        child += ["--seconds", str(args.seconds), "--rate", str(args.rate)]
    app_executable = executable(args.app)
    if args.terminal == "zirq":
        if args.presentation_library:
            env.update(DYLD_INSERT_LIBRARIES=str(args.presentation_library), KWI_PRESENTATION_TRACE=str(directory / "drawables.jsonl"))
        if args.trace:
            env.update(ZIRQ_TRACE=str(args.trace), ZIRQ_TRACE_MARKER="KWI_BENCHMARK_DONE")
        command = [str(app_executable), "--hold", "-e", *child]
    else:
        # Launch Services is the supported Ghostty.app entry point. Explicit env
        # forwarding and config-default-files=false isolate the user's settings.
        command = ["/usr/bin/open", "-n", "-W", "-a", str(args.app)]
        for key, value in env.items():
            command += ["--env", f"{key}={value}"]
        command += ["--args", "--config-default-files=false", "--window-save-state=never", "--font-family=Menlo", "--font-size=10",
                    f"--window-width={args.columns}", f"--window-height={args.rows}", "--window-inherit-font-size=false",
                    "--window-inherit-working-directory=false", f"--scrollback-limit-lines={args.history if args.history >= 0 else 'unlimited'}",
                    "--scrollback-limit-bytes=unlimited", f"--background-opacity={args.opacity}", "--background-blur=false",
                    "--cursor-style-blink=false", "--shell-integration=none", "--wait-after-command=true", "-e", *child]
    app_pid = None
    started = trace_clock_ns()
    with (directory / "app.log").open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            ready = wait_json(directory / "ready.json", process, 30)
            candidate = ready["parent_pid"]
            chain = []
            for _ in range(8):
                path = process_path(candidate)
                chain.append({"pid": candidate, "path": str(path)})
                if path.samefile(app_executable):
                    break
                if args.terminal == "zirq" or candidate <= 1:
                    raise RuntimeError(f"App identity differs: {chain}")
                candidate = int(subprocess.check_output(["/bin/ps", "-p", str(candidate), "-o", "ppid="], text=True, timeout=5))
            else:
                raise RuntimeError(f"App is not in the bounded launch ancestry: {chain}")
            if args.terminal == "zirq" and candidate != process.pid:
                raise RuntimeError("An existing instance received the request")
            app_pid = candidate
            foreground = activate_owned_app(args.app, app_pid) if args.terminal == "zirq" else None
            (directory / "start").touch()
            result = wait_json(directory / "result.json", process, max(100, args.seconds + 20))
            result["spawn_to_child_ready_ms"] = (ready["ready_ns"] - started) / 1e6
            if not args.probe and (result["initial_geometry"]["columns"], result["initial_geometry"]["rows"]) != (args.columns, args.rows):
                raise RuntimeError(f"Viewport is {result['initial_geometry']}, expected {args.columns}x{args.rows}")
            if not result["geometry_stable"]:
                raise RuntimeError("Viewport changed during the workload")
            if args.trace:
                deadline = time.monotonic() + 5
                submitted = None
                while time.monotonic() < deadline:
                    events = records((directory / "app.log").read_text(errors="replace"))
                    submitted = completion(events)
                    if submitted:
                        break
                    time.sleep(.02)
                if not submitted:
                    raise RuntimeError("Logical completion was not prepared and followed by a frame callback")
                result["prepared_marker_and_frame"] = submitted
                result["spawn_to_first_frame_callback_ms"] = (next(e for e in events if e["kind"] == "frame-swapped")["time_ns"] - started) / 1e6
                first_text = next((e for e in events if e["kind"] == "render" and e.get("rasterized_glyphs", 0) > 0), None)
                if first_text:
                    following = next(e for e in events if e["kind"] == "frame-swapped" and e["time_ns"] >= first_text["time_ns"])
                    result["spawn_to_first_nonempty_prepared_frame_callback_ms"] = (following["time_ns"] - started) / 1e6
            result["app_pid"] = app_pid
            result["foreground_pid_at_start"] = foreground
            result["launch_chain"] = chain
            return result
        finally:
            (directory / "stop").touch()
            time.sleep(.2)
            if app_pid is not None:
                # Identity is checked above; never terminate a forwarded/user instance.
                try:
                    os.kill(app_pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--terminal", choices=("zirq", "ghostty"), required=True)
    parser.add_argument("--workload", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--columns", type=int, default=120)
    parser.add_argument("--rows", type=int, default=40)
    parser.add_argument("--width", type=int, default=741)
    parser.add_argument("--height", type=int, default=510)
    parser.add_argument("--opacity", type=float, default=1)
    parser.add_argument("--history", type=int, default=100000)
    parser.add_argument("--trace", type=int, choices=(0, 1, 2), default=0)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=0)
    parser.add_argument("--rate", type=int, default=0)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--keep-viewport", action="store_true")
    parser.add_argument("--presentation-library", type=Path, help="Opt-in test-only Metal presentation instrumentation for Zirq")
    parser.add_argument("--alternating-presentation-library", type=Path, help="Alternate control/instrumented pairs for overhead calibration")
    args = parser.parse_args()
    if not 1 <= args.iterations <= 10 or not 0 <= args.seconds <= 60 or not 0 < args.opacity <= 1:
        parser.error("Use 1–10 repetitions, 0–60 seconds and opacity in (0,1]")
    args.app, args.output = args.app.resolve(), args.output.resolve()
    if args.presentation_library:
        args.presentation_library = args.presentation_library.resolve()
        if args.terminal != "zirq" or not args.presentation_library.is_file():
            parser.error("Presentation instrumentation requires Zirq and an existing test library")
    if args.alternating_presentation_library:
        args.alternating_presentation_library = args.alternating_presentation_library.resolve()
        if args.terminal != "zirq" or args.presentation_library or not args.alternating_presentation_library.is_file():
            parser.error("Alternating calibration requires Zirq, an existing library, and no --presentation-library")
    if args.workload:
        args.workload = args.workload.resolve()
    # A campaign owns a fresh output/profile directory. Never overwrite a user's
    # preferences or silently reuse part of an earlier measurement campaign.
    args.output.mkdir(parents=True, exist_ok=False)
    if args.alternating_presentation_library:
        variants = {"control": {"runs": []}, "instrumented": {"runs": []}}
        for name in variants:
            args.presentation_library = args.alternating_presentation_library if name == "instrumented" else None
            variants[name]["warmup"] = measure(args, args.output / name / "warmup", args.output / name / "profile-home")
        for n in range(args.iterations):
            for name in (list(variants) if n % 2 == 0 else list(reversed(variants))):
                args.presentation_library = args.alternating_presentation_library if name == "instrumented" else None
                run = measure(args, args.output / name / f"run-{n + 1}", args.output / name / "profile-home")
                variants[name]["runs"].append(run)
                print(f"{name} {n + 1}: {run['parser_mib_per_second']:.2f} MiB/s", flush=True)
        for value in variants.values():
            value["median_parser_mib_per_second"] = statistics.median(r["parser_mib_per_second"] for r in value["runs"])
        slowdown = 1 - variants["instrumented"]["median_parser_mib_per_second"] / variants["control"]["median_parser_mib_per_second"]
        result = {"scope": "Alternating paired overhead calibration; one warm-up + five measured runs per condition by default. Same trace level/grid/history/data.",
                  "variants": variants, "slowdown_fraction": slowdown, "within_five_percent_budget": slowdown <= .05,
                  "executable_sha256": hashlib.sha256(executable(args.app).read_bytes()).hexdigest(),
                  "parameters": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}}
        (args.output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        return
    home = args.output / "profile-home"
    home.mkdir(exist_ok=True)
    warmup = measure(args, args.output / "warmup", home)
    runs = []
    for n in range(args.iterations):
        run = measure(args, args.output / f"run-{n + 1}", home)
        runs.append(run)
        print(f"{args.terminal} {n + 1}: {run['parser_mib_per_second']:.2f} MiB/s; {run['initial_geometry']['columns']}x{run['initial_geometry']['rows']}", flush=True)
    result = {"terminal": args.terminal, "app": str(args.app), "executable_sha256": hashlib.sha256(executable(args.app).read_bytes()).hexdigest(),
              "parameters": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "scope": "Common PTY/DSR parser calibration, not pixel presentation. Untraced unless explicitly requested. Warm OS/disk cache after one warmup; fresh process per run.",
              "warmup": warmup, "runs": runs, "median_parser_mib_per_second": statistics.median(r['parser_mib_per_second'] for r in runs)}
    (args.output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
