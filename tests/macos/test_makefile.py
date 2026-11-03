# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Exercise build ordering and failure propagation without downloading or launching apps."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_build_tools import ROOT


class MakefileTest(unittest.TestCase):
    def campaign(self, failure=""):
        with tempfile.TemporaryDirectory(prefix="kwi make ") as directory:
            root = Path(directory)
            tools = root / "tools"
            tools.mkdir()
            runner = tools / "python helper"
            runner.write_text(f"#!{sys.executable}\n" + '''import json, os, sys
from pathlib import Path
args = sys.argv[1:]
if sys.argv[0].endswith('ctest'):
    action = 'ctest'
elif args[:1] == ['-c']:
    action = 'doctor'
elif args[:1] == ['-m']:
    action = 'tools'
elif args[0].endswith('craft.py'):
    action = 'prepare' if args[-1] == 'prepare' else ('build' if '--make' in args else 'configure' if '--configure' in args else 'dependencies')
elif args[0].endswith('package.py'):
    action = 'assets' if args[1] == 'assets' else 'package'
else:
    action = Path(args[0]).stem
with open(os.environ['CALL_LOG'], 'a') as stream:
    stream.write(json.dumps({'action': action, 'args': args}) + '\\n')
sys.exit(9 if action == os.environ.get('FAIL_ACTION') else 0)
''')
            runner.chmod(0o755)
            for name, body in {"uname": 'if [ "$1" = -s ]; then echo Darwin; else echo arm64; fi',
                               "git": "exit 0", "xcodebuild": "exit 0", "sw_vers": "exit 0"}.items():
                path = tools / name
                path.write_text("#!/bin/sh\n" + body + "\n")
                path.chmod(0o755)
            developer = root / "Xcode/Contents/Developer"
            (developer / "usr/bin").mkdir(parents=True)
            (developer / "usr/bin/xcodebuild").symlink_to(tools / "xcodebuild")
            prefix = root / "craft prefix"
            (prefix / "dev-utils/bin").mkdir(parents=True)
            shutil.copy2(runner, prefix / "dev-utils/bin/ctest")
            log = root / "calls.jsonl"
            result = subprocess.run([shutil.which("make"), "-f", str(ROOT / "Makefile"), "-j8", "ci",
                                     f"PYTHON={runner}", f"PREFIX={prefix}", f"BUILD_DIR={root / 'build output'}",
                                     f"DEVELOPER_DIR={developer}", "JOBS=2"], cwd=root,
                                    env={**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
                                         "CALL_LOG": str(log), "FAIL_ACTION": failure},
                                    capture_output=True, text=True, timeout=30)
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            return result, calls

    def test_parallel_make_builds_once_and_orders_release_checks(self):
        result, calls = self.campaign()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call["action"] for call in calls],
                         ["tools", "doctor", "source_bundle", "prepare", "dependencies", "configure", "build", "ctest", "record-dependencies", "package", "assets"])
        prepare = next(call["args"] for call in calls if call["action"] == "prepare")
        self.assertEqual(prepare[-3:], ["--jobs", "2", "prepare"])

    def test_failures_stop_all_later_steps_including_after_tee(self):
        for failure in ("tools", "build", "package"):
            with self.subTest(failure=failure):
                result, calls = self.campaign(failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls[-1]["action"], failure)
                self.assertNotIn("assets", [call["action"] for call in calls])


if __name__ == "__main__":
    unittest.main()
