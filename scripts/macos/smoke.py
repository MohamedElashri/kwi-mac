#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Bounded relocated-bundle launch check; does not replace GUI interaction tests."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import sys
import unicodedata


def ensure_kwi_not_running(lock=None):
    lock = lock if lock is not None else Path(f"/tmp/org.kde.kwi-{os.geteuid()}.socket.lock")
    if not lock.exists():
        return
    owner = int(lock.read_text().splitlines()[0])
    try:
        os.kill(owner, 0)
    except ProcessLookupError:
        return  # QLockFile handles stale endpoint cleanup.
    raise RuntimeError("Close the existing Kwi instance before measuring; it was not modified")


def trace_clock_ns():
    # libc++ steady_clock uses CLOCK_MONOTONIC_RAW on Darwin, while Python's
    # monotonic clock uses CLOCK_UPTIME_RAW. Their epochs differ after sleep.
    if sys.platform == "darwin":
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    return time.monotonic_ns()


def records(log):
    result = []
    for line in log.splitlines():
        if "KWI_TRACE {" in line:
            result.append(json.loads(line.split("KWI_TRACE ", 1)[1]))
    return result


def completion(events):
    marker = prepared = None
    for event in events:
        if event["kind"] == "marker-snapshot":
            marker, prepared = event, None
        elif event["kind"] == "render" and marker and event["terminal_id"] == marker["terminal_id"] and event["generation"] >= marker["generation"]:
            prepared = event
        elif event["kind"] == "frame-swapped" and prepared and event["time_ns"] >= prepared["time_ns"]:
            return marker, prepared, event
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hide-prefix", type=Path)
    parser.add_argument("--isolated-profile", action="store_true", help="Keep this check's settings/cache out of the normal user profile")
    args = parser.parse_args()
    app = args.app.resolve()
    work = args.output.resolve().parent
    work.mkdir(parents=True, exist_ok=True)
    executable = app / "Contents/MacOS/kwi"
    log_path = work / "launch.log"
    marker = work / "child-marker.txt"
    forwarded_marker = work / "forwarded-marker.txt"
    caller = work / "caller"
    requested_directory = caller / "cwd-é"
    requested_directory.mkdir(parents=True, exist_ok=True)
    if forwarded_marker.exists():
        forwarded_marker.unlink()
    if marker.exists():
        marker.unlink()
    env = {key: os.environ[key] for key in ("HOME", "USER", "LOGNAME") if key in os.environ}
    env.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8", KWI_TRACE="1", KWI_TRACE_MARKER="KWI_BUNDLE_SMOKE_READY", QSG_INFO="1")
    profile_home = work / "profile-home"
    if args.isolated_profile:
        profile_home.mkdir(exist_ok=True)
        env.update(HOME=str(profile_home), CFFIXED_USER_HOME=str(profile_home))
    hidden = None
    process = None
    started = trace_clock_ns()
    try:
        if args.hide_prefix:
            prefix = args.hide_prefix.resolve()
            if app.is_relative_to(prefix) or not (prefix / "etc/kwi-bootstrap.json").exists():
                parser.error("Only a marked Craft prefix outside this app can be hidden")
            candidate = prefix.with_name(prefix.name + ".smoke-hidden")
            if candidate.exists():
                parser.error("Hidden-prefix destination already exists; inspect it first")
        ensure_kwi_not_running()
        if args.hide_prefix:
            prefix.rename(candidate)
            hidden = candidate
        with log_path.open("w") as log:
            # $1 is a separate argv entry, including spaces/Unicode, never shell interpolation.
            command = "printf 'KWI_BUNDLE_SMOKE_READY\\n'; printf '%s|%s|%s|%s|%s\\n' \"$TERM\" \"$COLORTERM\" \"$TERM_PROGRAM\" \"${KWI_TRACE-unset}\" \"$PWD\" > \"$1\""
            process = subprocess.Popen([str(executable), "--hold", "-e", "/bin/sh", "-c", command, "kwi-smoke", str(marker)],
                                       cwd="/", env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = log_path.read_text(errors="replace")
                events = records(text)
                if marker.exists() and completion(events):
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.05)
            # Give the final output a frame opportunity; this still isn't text inspection.
            time.sleep(0.5)
            events = records(log_path.read_text(errors="replace"))
            frames = [e for e in events if e["kind"] == "frame-swapped"]
            graphics = [e for e in events if e["kind"] == "graphics"]
            snapshots = [e for e in events if e["kind"] == "marker-snapshot"]
            submitted = completion(events)
            literal = "literal space é ; $HOME"
            forwarded = None
            if submitted and process.poll() is None:
                request = subprocess.run([str(executable), "--new-tab", "--hold", "--workdir", requested_directory.name,
                                          "-e", "/bin/sh", "-c", 'printf "%s\\n%s\\n" "$PWD" "$1" > "$2"',
                                          "kwi-forwarded", literal, str(forwarded_marker)],
                                         cwd=caller, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=15)
                deadline = time.monotonic() + 5
                while not forwarded_marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                actual = forwarded_marker.read_text().splitlines() if forwarded_marker.exists() else []
                forwarded = {"exit_status": request.returncode, "directory_and_literal_argument": actual,
                             "unicode_comparison": "NFC equivalence, not byte identity",
                             "passed": request.returncode == 0 and len(actual) == 2
                             and Path(actual[0]).samefile(requested_directory)
                             and unicodedata.normalize("NFC", actual[1]) == literal}
            result = {"app": str(app), "environment": sorted(env), "prefix_hidden": bool(hidden),
                      "isolated_profile": args.isolated_profile,
                      "trace_clock": "Darwin CLOCK_MONOTONIC_RAW / libc++ steady_clock",
                      "child_marker": marker.read_text().strip() if marker.exists() else None,
                      "frame_callbacks": len(frames), "graphics": graphics,
                      "logical_marker_snapshots": snapshots,
                      "prepared_marker_and_frame": submitted,
                      "cli_forwarding": forwarded,
                      "window": [e for e in events if e["kind"] == "window"][-1:],
                      "first_frame_callback_from_spawn_ms": (frames[0]["time_ns"] - started) / 1e6 if frames else None,
                      "frames": frames[:3], "pid": process.pid,
                      "limits": "Logical terminal marker plus frame-swapped callback, not measured pixel presentation or interactive GUI coverage."}
            # QSGRendererInterface::Metal == 6; a fallback backend is not a pass.
            result["passed"] = bool(result["child_marker"] == "xterm-256color|truecolor|kwi|unset|/" and frames and graphics
                                    and all(e["api"] == 6 for e in graphics) and submitted and forwarded and forwarded["passed"]
                                    and process.poll() is None)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
            if not result["passed"]:
                raise SystemExit("Relocated-bundle launch check failed; inspect launch.log")
    finally:
        if process and process.poll() is None:
            # The controlled child has already exited; no user session is terminated.
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if hidden:
            hidden.rename(prefix)


if __name__ == "__main__":
    main()
