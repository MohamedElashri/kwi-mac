#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Measure fixed-content tab cycles in the real Mac app's outer event loop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

from app_test_support import executable, isolated_profile, process_path, ensure_kwi_not_running


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def wait_for(predicate, process, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise RuntimeError(f"Owned app exited {process.returncode}")
        time.sleep(.01)
    raise TimeoutError("Controlled app operation timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--metrics-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--batches", type=int, default=6, help="100 cycles per batch, including the first warm-up batch (6–30)")
    args = parser.parse_args()
    if not 6 <= args.batches <= 30:
        parser.error("Use 6–30 equal batches")
    app, output = args.app.resolve(), args.output.resolve()
    binary = executable(app)
    endpoint = Path(f"/tmp/org.kde.kwi-{os.geteuid()}.socket")
    lock = Path(str(endpoint) + ".lock")
    ensure_kwi_not_running(lock)
    output.mkdir(parents=True, exist_ok=False)
    home = output / "home"
    isolated_profile(home, 741, 510, 1, 100000)
    env = {k: os.environ[k] for k in ("USER", "LOGNAME") if k in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8")
    anchor = output / "anchor.pid"
    command = 'printf KWI_MEMORY_ANCHOR; printf "%s" "$$" > "$1"; exec /bin/sleep 300'
    samples = []
    with (output / "app.log").open("w") as log:
        process = subprocess.Popen([str(binary), "--hold", "-e", "/bin/sh", "-c", command, "kwi-memory", str(anchor)],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: anchor.exists(), process, 30)
            if int(lock.read_text().splitlines()[0]) != process.pid or not process_path(process.pid).samefile(binary):
                raise RuntimeError("The isolated app does not own the activation endpoint")
            for batch in range(args.batches):
                for n in range(100):
                    marker = output / f"child-{batch * 100 + n}.pid"
                    arguments = [str(binary), "--new-tab", "-e", "/bin/sh", "-c",
                                 'printf KWI_MEMORY_FIXED; printf "%s" "$$" > "$1"', "kwi-memory", str(marker)]
                    # This is the same bounded local protocol used by the CLI;
                    # verify the owner for every request before sending it.
                    if int(lock.read_text().splitlines()[0]) != process.pid:
                        raise RuntimeError("Activation endpoint ownership changed")
                    with socket.socket(socket.AF_UNIX) as client:
                        client.settimeout(5)
                        client.connect(str(endpoint))
                        client.sendall(json.dumps({"arguments": arguments, "directory": str(output)}).encode() + b"\n")
                        response = client.makefile("rb").readline(16)
                        if response != b"0\n":
                            raise RuntimeError(f"Activation rejected the controlled launch: {response!r}")
                    wait_for(lambda: marker.exists() and marker.stat().st_size > 0, process)
                    child = int(marker.read_text())
                    wait_for(lambda: not alive(child), process)
                    time.sleep(.025)  # Let normal close/delete events complete.
                time.sleep(3)
                measured = json.loads(subprocess.check_output([str(args.metrics_tool.resolve()), "--process-metrics", str(process.pid)],
                                                             env=env, text=True, timeout=10))
                measured["cycles"] = (batch + 1) * 100
                samples.append(measured)
                (output / "checkpoint.json").write_text(json.dumps(samples, indent=2) + "\n")
                print(f"{measured['cycles']} cycles: {measured['physical_footprint']} footprint, {measured['rss']} RSS", flush=True)
            result = {"app": str(app), "executable_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "samples": samples,
                      "scope": f"Real QApplication::exec; untraced; 100 warm-up + {args.batches - 1} equal 100-cycle batches; 3 seconds settling; no forced GC or allocator purge."}
            (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
            if anchor.exists() and anchor.stat().st_size:
                deadline = time.monotonic() + 3
                while alive(int(anchor.read_text())) and time.monotonic() < deadline:
                    time.sleep(.05)


if __name__ == "__main__":
    main()
