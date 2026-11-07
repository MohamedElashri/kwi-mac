# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0
"""Keep cask version and checksum updates paired and reject ambiguous input."""
from pathlib import Path
import base64
import json
import subprocess
import tempfile
import unittest
from unittest import mock

from test_build_tools import ROOT, load


class CaskTest(unittest.TestCase):
    def test_version_and_checksum_update_together(self):
        updater = load("update-cask")
        with tempfile.TemporaryDirectory() as directory:
            cask = Path(directory) / "kwi-mac.rb"
            cask.write_text((ROOT / "Casks/zirq.rb").read_text())
            updater.update("v0.3.0", "a" * 64 + "  Zirq-arm64.zip\n", cask)
            self.assertIn('version "0.3.0"', cask.read_text())
            self.assertIn('/Zirq-arm64.zip"', cask.read_text())
            self.assertIn('  app "Zirq.app"\n', cask.read_text())
            self.assertIn('sha256 "' + "a" * 64 + '"', cask.read_text())

    def test_legacy_archive_and_homebrew_rename_remain_valid(self):
        text = load("update-cask").render("v0.2.0", "b" * 64 + "  Kwi-arm64.zip\n", (ROOT / "Casks/zirq.rb").read_text())
        self.assertIn('/Kwi-arm64.zip"', text)
        self.assertIn('app "Kwi.app", target: "Zirq.app"', text)
        self.assertEqual(json.loads((ROOT / "cask_renames.json").read_text()), {"kwi-mac": "zirq"})

    def test_invalid_or_ambiguous_release_input_preserves_cask(self):
        updater = load("update-cask")
        with tempfile.TemporaryDirectory() as directory:
            cask = Path(directory) / "kwi-mac.rb"
            original = (ROOT / "Casks/zirq.rb").read_text()
            for tag, hashes in [("../main", "a" * 64 + "  Zirq-arm64.zip\n"),
                                ("v0.3.0", "missing manifest entry"),
                                ("v0.3.0", "a" * 64 + "  Kwi-arm64.zip\n" + "b" * 64 + "  Zirq-arm64.zip\n"),
                                ("v0.3.0", ("a" * 64 + "  Zirq-arm64.zip\n") * 2)]:
                cask.write_text(original)
                with self.subTest(tag=tag, hashes=hashes), self.assertRaises(ValueError):
                    updater.update(tag, hashes, cask)
                self.assertEqual(cask.read_text(), original)


class CaskAutomationTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="kwi cask automation ")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.path = self.root / "Casks/zirq.rb"
        self.path.parent.mkdir()
        self.path.write_text((ROOT / "Casks/zirq.rb").read_text())
        self.updater = load("update-cask")
        self.calls = []
        self.release = {"tag_name": "v0.3.0", "draft": False, "prerelease": False}
        self.signed = True
        self.existing = False

    def api(self, endpoint, data=None):
        self.calls.append((endpoint, data))
        if endpoint.endswith("releases/latest"):
            return self.release
        if endpoint.endswith("git/ref/heads/main"):
            return {"object": {"sha": "b" * 40}}
        if "git/matching-refs/" in endpoint:
            return [{"ref": "refs/heads/automation/homebrew-v0.3.0", "object": {"sha": "c" * 40}}] if self.existing else []
        if endpoint == "graphql":
            return {"data": {"createCommitOnBranch": {"commit": {"oid": "c" * 40, "signature": {"isValid": self.signed}}}}}
        if "/contents/" in endpoint:
            return {"content": base64.b64encode(self.path.read_bytes()).decode()}
        if "/compare/" in endpoint:
            return {"files": [{"filename": "Casks/zirq.rb"}]}
        if "/commits/" in endpoint:
            return {"commit": {"verification": {"verified": self.signed}}}
        if "/pulls?" in endpoint:
            return [{"html_url": "https://example.invalid/pr/1"}] if self.existing else []
        if endpoint.endswith("/pulls"):
            return {"html_url": "https://example.invalid/pr/1"}
        return {}

    def run_sync(self, tag="", check_error=None):
        with mock.patch.object(self.updater, "ROOT", self.root), \
                mock.patch.object(self.updater, "api", side_effect=self.api), \
                mock.patch.object(self.updater, "git", side_effect=lambda *a: "" if a[0] == "status" else "b" * 40), \
                mock.patch.object(self.updater, "fetch_checksums", return_value="a" * 64 + "  Zirq-arm64.zip\n"), \
                mock.patch.object(self.updater.subprocess, "run", side_effect=check_error) as checks:
            self.updater.sync_pull_request(tag)
        return checks

    def test_signed_commit_pr_and_explicit_ci_dispatch(self):
        checks = self.run_sync()
        checks.assert_called_once_with(["make", "check"], cwd=self.root, check=True)
        payload = next(data for endpoint, data in self.calls if endpoint == "graphql")["variables"]["input"]
        self.assertEqual(payload["expectedHeadOid"], "b" * 40)
        self.assertEqual([item["path"] for item in payload["fileChanges"]["additions"]], ["Casks/zirq.rb"])
        self.assertIn('version "0.3.0"', base64.b64decode(payload["fileChanges"]["additions"][0]["contents"]).decode())
        pr = next(data for endpoint, data in self.calls if endpoint.endswith("/pulls"))
        self.assertTrue(pr["body"].startswith("This PR is "))
        self.assertNotIn("\n\n", pr["body"])
        self.assertEqual(self.calls[-1][1], {"ref": "automation/homebrew-v0.3.0", "inputs": {"cask_check": "true"}})

    def test_failed_checks_cannot_write_remote_branches(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_sync(check_error=subprocess.CalledProcessError(1, ["make", "check"]))
        self.assertFalse(any(data is not None for _, data in self.calls))

    def test_unsigned_commit_cannot_open_a_pr(self):
        self.signed = False
        with self.assertRaisesRegex(RuntimeError, "signature"):
            self.run_sync()
        self.assertFalse(any("/pulls" in endpoint for endpoint, _ in self.calls))

    def test_existing_signed_branch_and_pr_are_reused(self):
        self.existing = True
        self.run_sync()
        self.assertFalse(any(endpoint == "graphql" or (endpoint.endswith("/pulls") and data) for endpoint, data in self.calls))
        self.assertTrue(self.calls[-1][0].endswith("/dispatches"))

    def test_draft_prerelease_old_release_and_current_cask_are_noops(self):
        for release, tag in [({"tag_name": "v0.3.0", "draft": True, "prerelease": False}, ""),
                             ({"tag_name": "v0.3.0", "draft": False, "prerelease": True}, ""),
                             ({"tag_name": "v0.3.0", "draft": False, "prerelease": False}, "v0.1.0"),
                             ({"tag_name": "v0.1.0", "draft": False, "prerelease": False}, "")]:
            self.calls.clear()
            self.release = release
            self.run_sync(tag).assert_not_called()
            self.assertFalse(any(data is not None for _, data in self.calls))
        self.release = {"tag_name": "v0.3.0", "draft": False, "prerelease": False}
        self.updater.update("v0.3.0", "a" * 64 + "  Zirq-arm64.zip\n", self.path)
        self.calls.clear()
        self.run_sync().assert_not_called()
        self.assertFalse(any(data is not None for _, data in self.calls))


if __name__ == "__main__":
    unittest.main()
