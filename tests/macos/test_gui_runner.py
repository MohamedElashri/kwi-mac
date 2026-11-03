# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

scripts = Path(__file__).resolve().parents[2] / "scripts/macos"
sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location("gui_runner", scripts / "run-gui-test.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
sys.path.pop(0)


class GuiRunnerTest(unittest.TestCase):
    def status(self, xml):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "junit.xml"
            report.write_text(xml)
            return runner.junit_status(report)

    def test_test_failures_override_successful_launcher(self):
        self.assertTrue(self.status('<testsuite tests="2" failures="0" errors="0"/>'))
        for key in ("failures", "errors", "skipped"):
            self.assertFalse(self.status(f'<testsuites><testsuite tests="2" {key}="1"/></testsuites>'))

    def test_missing_test_cases_cannot_pass(self):
        for report in ('<testsuites/>', '<testsuite tests="0"/>'):
            with self.assertRaises(ValueError):
                self.status(report)
