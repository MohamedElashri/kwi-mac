# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Docs-only CI skips native builds without hiding source changes or bad diffs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_build_tools import ROOT


class CiChangesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kwi-ci-changes-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.git("init", "-q")
        self.write("README.md", "Initial documentation\n")
        self.write("src/main.cpp", "// source\n")
        self.base = self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-c", "user.name=Kwi test", "-c", "user.email=test@example.invalid",
                                        "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", *args],
                                       cwd=self.root, text=True, stderr=subprocess.PIPE).strip()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self):
        self.git("add", "-f", ".")
        self.git("commit", "-qm", "Fixture")
        return self.git("rev-parse", "HEAD")

    def classify(self, **extra):
        output = self.root / ".git/step-output"
        return subprocess.run([shutil.which("make"), "--no-print-directory", "-f", str(ROOT / "Makefile"), "ci-changes"], cwd=self.root,
                              env={**os.environ, "CI_BASE": self.base, "CI_HEAD": "HEAD", "CI_EVENT": "push", "CI_IS_TAG": "false",
                                   "GITHUB_OUTPUT": str(output), **extra}, capture_output=True, text=True, timeout=15)

    def test_documentation_and_screenshot_only_skip_native_build(self):
        self.write("README.md", "Updated documentation\n")
        self.write("assets/readme/screenshot.png", "Screenshot fixture")
        self.write("assets/readme/screenshot.png.license", "CC0-1.0")
        self.commit()
        result = self.classify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "build=false")
        self.assertEqual((self.root / ".git/step-output").read_text(), "build=false\n")

    def test_source_workflow_and_application_assets_require_native_build(self):
        for filename in ("src/main.cpp", ".github/workflows/checks.yml", "Makefile", "logo.png"):
            with self.subTest(filename=filename):
                self.write(filename, "Changed\n")
                self.write("README.md", "Documentation also changed\n")
                self.commit()
                result = self.classify()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "build=true")
                self.base = self.git("rev-parse", "HEAD")

    def test_removing_source_by_renaming_into_docs_still_builds(self):
        (self.root / "docs").mkdir()
        (self.root / "src/main.cpp").rename(self.root / "docs/example.md")
        self.commit()
        self.assertEqual(self.classify().stdout.strip(), "build=true")

    def test_manual_tags_and_new_branch_build_even_when_only_docs_changed(self):
        self.write("README.md", "Documentation\n")
        self.commit()
        for extra in ({"CI_EVENT": "workflow_dispatch"}, {"CI_IS_TAG": "true"}, {"CI_BASE": "0" * 40}):
            with self.subTest(extra=extra):
                result = self.classify(**extra)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "build=true")

    def test_invalid_diff_fails_instead_of_skipping_the_build(self):
        result = self.classify(CI_BASE="not-a-commit")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("build=false", result.stdout)


if __name__ == "__main__":
    unittest.main()
