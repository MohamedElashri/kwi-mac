#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Run an owned Qt GUI test through macOS Launch Services and verify JUnit."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

from app_test_support import process_path


def junit_status(path):
    report = ET.parse(path).getroot()
    suites = [report] if report.tag == "testsuite" else list(report.iter("testsuite"))
    if not suites or not sum(int(s.get("tests", 0)) for s in suites):
        raise ValueError("GUI test did not report any test cases")
    return not any(int(s.get(key, 0)) for s in suites for key in ("failures", "errors", "skipped"))


def main():
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--trace", type=int, choices=(0, 1), default=0)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--presented", action="store_true")
    parser.add_argument("--gpu-trace", action="store_true", help="Diagnostic command-buffer timing; excluded from standard measurements")
    parser.add_argument("--render-loop", choices=("basic", "threaded"), help="Diagnostic Qt render-loop override")
    parser.add_argument("--background", action="store_true", help="Launch without activation; supported by background rendering and hidden idle fixtures")
    parser.add_argument("--scale-factor", type=float, choices=(.5, 1., 2.))
    parser.add_argument("--key-diagnostics", action="store_true")
    parser.add_argument("--drawable-count", type=int, choices=(2, 3))
    parser.add_argument("methods", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.drawable_count is not None and not args.presented:
        parser.error("--drawable-count requires --presented test instrumentation")
    if args.gpu_trace and not args.presented:
        parser.error("--gpu-trace requires --presented test instrumentation")
    executable = args.executable.resolve()
    app = next(p for p in executable.parents if p.suffix == ".app")
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="kwi-gui-test-"))
    home = output / "home"
    home.mkdir()
    pid_file = output / "owned.pid"
    activation_file = output / "activation.json"
    env = {k: os.environ[k] for k in ("USER", "LOGNAME") if k in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8",
               KWI_GUI_TEST_PID_FILE=str(pid_file), KWI_TEST_PYTHON="/usr/bin/python3", KWI_PERF_OUTPUT=str(output),
               KWI_PERF_TRACE=str(args.trace), KWI_PERF_SECONDS=str(args.seconds))
    if args.scale_factor is not None:
        env["QT_SCALE_FACTOR"] = str(args.scale_factor)
    if args.background:
        env["KWI_PERF_BACKGROUND"] = "1"
    else:
        env["KWI_GUI_TEST_ACTIVATION_FILE"] = str(activation_file)
    if args.key_diagnostics:
        env["QT_LOGGING_RULES"] = "qt.qpa.input.keys.debug=true;qt.gui.shortcutmap.debug=true"
    if args.drawable_count is not None:
        env["KWI_PRESENTATION_DRAWABLE_COUNT"] = str(args.drawable_count)
    if args.presented:
        env["KWI_PRESENTATION_TRACE"] = str(output / "drawables.jsonl")
    if args.gpu_trace:
        env["KWI_GPU_TRACE"] = "1"
    if args.render_loop:
        env["QSG_RENDER_LOOP"] = args.render_loop
    command = ["/usr/bin/open", "-n", "-W", "--stdout", str(output / "launch.log"), "--stderr", str(output / "launch.log"), "-a", str(app)]
    if args.background:
        command.insert(1, "-g")
    for key, value in env.items():
        command += ["--env", f"{key}={value}"]
    methods = args.methods[1:] if args.methods[:1] == ["--"] else args.methods
    command += ["--args", *methods, "-o", str(output / "test.log") + ",txt", "-o", str(output / "junit.xml") + ",junitxml"]
    process = subprocess.Popen(command)
    owned_pid = None
    activation_requests = []
    activation_sequence = 0
    try:
        deadline = time.monotonic() + args.timeout
        while process.poll() is None:
            if owned_pid is None and pid_file.exists() and pid_file.stat().st_size:
                candidate = int(pid_file.read_text())
                if not process_path(candidate).samefile(executable):
                    raise RuntimeError("GUI test PID marker identifies a different executable")
                owned_pid = candidate
            if owned_pid is not None and not args.background and activation_file.exists():
                request = json.loads(activation_file.read_text())
                if request["pid"] != owned_pid:
                    raise RuntimeError("Activation request does not identify the owned GUI test")
                if request["sequence"] > activation_sequence:
                    if not process_path(owned_pid).samefile(executable):
                        raise RuntimeError("GUI activation executable identity changed")
                    activated = subprocess.run(["/usr/bin/open", "-a", str(app)], capture_output=True, text=True, timeout=5)
                    activation_sequence = request["sequence"]
                    ack = {"pid": owned_pid, "sequence": activation_sequence, "success": activated.returncode == 0,
                           "exit": activated.returncode, "stderr": activated.stderr}
                    temporary = activation_file.with_suffix(".ack.tmp")
                    temporary.write_text(json.dumps(ack) + "\n")
                    temporary.replace(str(activation_file) + ".ack")
                    activation_requests.append(ack)
            if time.monotonic() >= deadline:
                raise TimeoutError("Owned GUI test exceeded its bounded runtime")
            time.sleep(.05)
        passed = process.returncode == 0 and junit_status(output / "junit.xml")
        result = {"passed": passed, "launchservices_exit": process.returncode, "verified_owned_pid": owned_pid,
                  "output": str(output), "executable": str(executable), "launch_method": "Launch Services; test outcome comes from JUnit, not open's status",
                  "qt_scale_factor": args.scale_factor, "key_diagnostics": args.key_diagnostics,
                  "launch_background": args.background,
                  "gpu_timing_diagnostic": args.gpu_trace,
                  "render_loop_override": args.render_loop,
                  "activation_requests": activation_requests,
                  "metal_drawable_count_request": args.drawable_count}
        (output / "runner.json").write_text(json.dumps(result, indent=2) + "\n")
        test_log = output / "test.log"
        if test_log.exists():
            print(test_log.read_text(), end="")
        print(json.dumps(result))
        if not passed:
            raise SystemExit(1)
    finally:
        if owned_pid is not None:
            try:
                if process_path(owned_pid).samefile(executable):
                    os.kill(owned_pid, signal.SIGTERM)
            except (ProcessLookupError, RuntimeError):
                pass
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
