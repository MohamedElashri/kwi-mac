#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Check the real Mac bundle's CLI/preset/environment contract without timing it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import unicodedata

from app_test_support import ensure_kwi_not_running, executable, isolated_profile, process_path, wait_json
from smoke import records


def normalized(value):
    return unicodedata.normalize("NFC", value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    ensure_kwi_not_running()
    app, output = args.app.resolve(), args.output.resolve()
    binary = executable(app)
    output.mkdir(parents=True, exist_ok=False)
    home = output / "home"
    isolated_profile(home, 741, 510, 1, 100000)
    folder = output / "preset cwd-é"
    relative = output / "caller" / "relative cwd-é"
    folder.mkdir()
    relative.mkdir(parents=True)
    presets = home / "Library/Application Support/kwi/presets"
    presets.mkdir(parents=True)
    (presets / "test.preset").write_text(
        "[General]\nFormat=KwiLaunchPreset\nVersion=1\nName=Compatibility test\n"
        "[Launch]\nDirectoryMode=Custom\nDirectory=" + str(folder) + "\n"
        "Environment=KWI_PRESET=literal = spaced ; $HOME\n")
    capture = output / "capture.py"
    capture.write_text('''import json, os, sys
from pathlib import Path
target = Path(sys.argv[1])
data = {"pid": os.getpid(), "parent_pid": os.getppid(), "cwd": os.getcwd(),
        "arguments": sys.argv[2:], "environment": {k: os.environ.get(k) for k in
        ("TERM", "COLORTERM", "TERM_PROGRAM", "LANG", "KWI_PRESET")}}
temporary = target.with_suffix(".partial")
temporary.write_text(json.dumps(data))
os.replace(temporary, target)
print("KWI_COMPATIBILITY_CAPTURE", flush=True)
''')
    env = {key: os.environ[key] for key in ("USER", "LOGNAME") if key in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(output / "absent-bus.socket"),
               KWI_TRACE="2", KWI_TRACE_MARKER="KWI_COMPATIBILITY_CAPTURE")
    literal = ["space argument", "", "literal ; $HOME $(false)", "Unicode-é"]
    cases = []
    pending = []
    captures = []
    log_path = output / "app.log"
    with log_path.open("w") as log:
        first_path = output / "first.json"
        primary = subprocess.Popen([str(binary), "--preset", "Compatibility test", "--hold", "--tab-title", "Compatibility title", "-e",
                                    "/usr/bin/python3", str(capture), str(first_path), *literal],
                                   cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            first = wait_json(first_path, primary, 30)
            if first["parent_pid"] != primary.pid or not process_path(primary.pid).samefile(binary):
                raise RuntimeError("The controlled app did not own the preset launch")

            def check_capture(data, expected_folder, expected_arguments):
                if normalized(data["cwd"]) != normalized(str(expected_folder.resolve())):
                    raise RuntimeError("Working directory differs")
                if [normalized(v) for v in data["arguments"]] != [normalized(v) for v in expected_arguments]:
                    raise RuntimeError("Literal/empty argument fidelity differs")
                if data["parent_pid"] != primary.pid:
                    raise RuntimeError("Forwarding reached a different app process")
                for key, value in {"TERM": "xterm-256color", "COLORTERM": "truecolor", "TERM_PROGRAM": "kwi"}.items():
                    if data["environment"][key] != value:
                        raise RuntimeError("Terminal environment differs: " + key)
                if not (data["environment"]["LANG"] or "").endswith(".UTF-8"):
                    raise RuntimeError("Missing valid default UTF-8 locale")
                captures.append(data)

            check_capture(first, folder, literal)
            if first["environment"]["KWI_PRESET"] != "literal = spaced ; $HOME":
                raise RuntimeError("Preset environment was expanded or changed")
            cases.append({"case": "preset-command-override-cwd-environment-hold-title", "passed": True,
                          "title_scope": "CLI flag accepted; displayed custom title is checked by the native component fixture"})
            for label, flags, expected in [
                ("help", ["--help"], 0), ("version", ["--version"], 0),
                ("missing-execute-program", ["-e"], 2), ("empty-preset-name", ["--preset", ""], 2),
                ("unknown-option", ["--kwi-invalid-option"], 1),
                ("unknown-preset", ["--new-tab", "--preset", "Kwi nonexistent preset"], 1),
                ("bounded-forwarded-request", ["--new-tab", "-e", "/usr/bin/false", "x" * 150000], 2),
            ]:
                result = subprocess.run([str(binary), *flags], cwd=output, env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
                (output / (label + ".log")).write_text(result.stdout)
                cases.append({"case": label, "expected_exit": expected, "actual_exit": result.returncode,
                              "passed": result.returncode == expected})
                if result.returncode != expected:
                    raise RuntimeError(label + " returned an unexpected status")

            for number in range(2):
                destination = output / f"concurrent-{number}.json"
                values = [*literal, str(number)]
                command = [str(binary), "--new-tab", "--noclose", "--preset", "Compatibility test", "--workdir", relative.name,
                           "-e", "/usr/bin/python3", str(capture), str(destination), *values]
                pending.append((subprocess.Popen(command, cwd=relative.parent, env=env,
                                                  stdout=log, stderr=subprocess.STDOUT), destination, values))
            for client, destination, values in pending:
                try:
                    if client.wait(timeout=15) != 0:
                        raise RuntimeError("Concurrent tab request rejected")
                finally:
                    if client.poll() is None:
                        client.kill()
                        client.wait(timeout=5)
                check_capture(wait_json(destination, primary, 15), relative, values)
            cases.append({"case": "concurrent-new-tabs-relative-workdir-empty-literal-unicode-argv-noclose", "passed": True})
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                trace = records(log_path.read_text(errors="replace"))
                if any(v["kind"] == "graphics" and v.get("api") == 6 for v in trace):
                    break
                time.sleep(.02)
            else:
                raise RuntimeError("No Metal scene-graph evidence")
            result = {"passed": True, "scope": "Functional packaged-app checks; no performance measurements. No reachable session bus.",
                      "app": str(app), "executable_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                      "cases": cases, "captures": captures, "metal_api": 6,
                      "unicode_comparison": "NFC equivalence for macOS filesystem/argv encoding; other literal bytes and empty arguments preserved",
                      "exit_status_contract": "CLI/activation status, not the child command's exit status"}
            (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2))
        finally:
            for client, _, _ in pending:
                if client.poll() is None:
                    client.kill()
                    client.wait(timeout=5)
            if primary.poll() is None:
                primary.terminate()
                primary.wait(timeout=10)


if __name__ == "__main__":
    main()
