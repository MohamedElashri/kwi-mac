#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Measure normal, untraced idle app CPU for one or ten controlled shells."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def snapshot(pid):
    output = subprocess.check_output(["/bin/ps", "-p", str(pid), "-o", "time=,rss="], text=True, timeout=5).split()
    fields = output[0].split(":")
    seconds = sum(float(value) * 60 ** n for n, value in enumerate(reversed(fields)))
    return {"cpu_seconds": seconds, "rss_bytes": int(output[1]) * 1024}


def children(pid):
    query = subprocess.run(["/usr/bin/pgrep", "-P", str(pid)], text=True, capture_output=True, timeout=5)
    if query.returncode not in (0, 1):
        raise RuntimeError("Could not inspect controlled child processes")
    return [int(value) for value in query.stdout.split()]


def exists(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--tabs", type=int, choices=(1, 10), required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error("Use 1–120 measured seconds")
    app = args.app.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    env = {k: os.environ[k] for k in ("HOME", "USER", "LOGNAME") if k in os.environ}
    env.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8", PS1="KWI_IDLE> ", TMOUT="180")
    executable = str(app / "Contents/MacOS/kwi")
    process = None
    result = None
    controlled = []
    try:
        with args.output.with_suffix(".log").open("w") as log:
            process = subprocess.Popen([executable, "-e", "/bin/sh", "-i"], env=env, stdout=log, stderr=subprocess.STDOUT)
            time.sleep(5)
            if process.poll() is not None:
                raise RuntimeError("Close other Kwi instances before an isolated idle measurement")
            for _ in range(args.tabs - 1):
                subprocess.run([executable, "--new-tab", "-e", "/bin/sh", "-i"], env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=10)
            time.sleep(5)
            controlled = children(process.pid)
            before = snapshot(process.pid)
            start = time.monotonic()
            remaining = args.seconds
            while remaining:
                wait = min(5, remaining)
                time.sleep(wait)
                remaining -= wait
                if process.poll() is not None:
                    raise RuntimeError("App exited during the idle interval")
            elapsed = time.monotonic() - start
            after = snapshot(process.pid)
            result = {"tabs": args.tabs, "measured_seconds": elapsed, "tracing": False,
                      "scope": "Visible window, controlled sh -i shells, default appearance; no blink/animation workload added. RSS is not physical footprint.",
                      "cpu_percent_of_one_core": (after["cpu_seconds"] - before["cpu_seconds"]) / elapsed * 100,
                      "before": before, "after": after}
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            deadline = time.monotonic() + 3
            while any(exists(pid) for pid in controlled) and time.monotonic() < deadline:
                time.sleep(0.1)
            if result is not None:
                result["controlled_child_pids"] = controlled
                result["children_still_present_after_exit"] = [pid for pid in controlled if exists(pid)]
    if result is not None:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
