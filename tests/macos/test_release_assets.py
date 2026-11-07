# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Release integrity and ownership checks shared by local Make targets and CI."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from test_build_tools import load


class ReleaseAssetsTest(unittest.TestCase):
    def setUp(self):
        self.package = load("package")
        self.commit = "a" * 40

    def fixture(self, root):
        root.mkdir()
        (root / "Zirq-arm64.zip").write_bytes(b"verified app archive")
        (root / "Zirq-sources.tar.gz").write_bytes(b"verified sources")
        (root / "SOURCES.json").write_text(json.dumps({"project_commit": self.commit}))
        (root / "dependencies.json").write_text(json.dumps({"source_commit": self.commit}))
        (root / "Zirq.bundle.json").write_text('{"reused_deployment": false}')
        (root / "README.txt").write_text("Install instructions")
        record = {"owner": self.package.OWNER, "source": {"source_commit": self.commit},
                  "full_deployment": True, "strict_signatures_original_and_extracted": True,
                  "archive_round_trip_bytes_modes_symlinks": True,
                  "archive": {"name": "Zirq-arm64.zip", "sha256": self.package.digest(root / "Zirq-arm64.zip"),
                              "bytes": (root / "Zirq-arm64.zip").stat().st_size},
                  "source_distribution": {"archive": "Zirq-sources.tar.gz", "sha256": self.package.digest(root / "Zirq-sources.tar.gz")},
                  "bundle_manifest_sha256": self.package.digest(root / "Zirq.bundle.json")}
        (root / "package.json").write_text(json.dumps(record))
        self.checksums(root)

    def checksums(self, root):
        (root / "SHA256SUMS").write_text("".join(f"{self.package.digest(root / name)}  {name}\n" for name in self.package.ASSET_FILES))

    def test_verified_assets_require_matching_source_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "assets"
            self.fixture(root)
            self.assertEqual(self.package.verify_assets(root, self.commit), self.commit)
            with self.assertRaisesRegex(RuntimeError, "source identity"):
                self.package.verify_assets(root, "b" * 40)
            (root / "dependencies.json").write_text(json.dumps({"source_commit": "b" * 40}))
            self.checksums(root)
            with self.assertRaisesRegex(RuntimeError, "source identity"):
                self.package.verify_assets(root)

    def test_checksums_and_package_metadata_both_protect_archives(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "assets"
            self.fixture(root)
            (root / "Zirq-arm64.zip").write_bytes(b"modified archive")
            with self.assertRaisesRegex(RuntimeError, "asset changed"):
                self.package.verify_assets(root)
            self.checksums(root)
            with self.assertRaisesRegex(RuntimeError, "metadata does not match"):
                self.package.verify_assets(root)

    def test_invalid_new_package_preserves_previous_release_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "package", root / "assets"
            self.fixture(source)
            self.package.assemble_assets(source, source / "dependencies.json", output)
            before = self.package.snapshot(output)
            (source / "Zirq-arm64.zip").write_bytes(b"invalid replacement")
            with self.assertRaisesRegex(RuntimeError, "metadata does not match"):
                self.package.assemble_assets(source, source / "dependencies.json", output)
            self.assertEqual(self.package.snapshot(output), before)
            (output / "user-notes.txt").write_text("preserve me")
            with self.assertRaisesRegex(RuntimeError, "expected files"):
                self.package.assemble_assets(source, source / "dependencies.json", output)
            self.assertEqual((output / "user-notes.txt").read_text(), "preserve me")

    def test_symlink_output_is_rejected_by_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "package"
            self.fixture(source)
            target = root / "user-data"
            target.mkdir()
            link = root / "output"
            link.symlink_to(target, target_is_directory=True)
            with mock.patch("sys.argv", ["package.py", "assets", "--package-dir", str(source), "--output", str(link)]):
                with self.assertRaisesRegex(RuntimeError, "symlink"):
                    self.package.main()
            self.assertTrue(link.is_symlink())
            self.assertEqual(list(target.iterdir()), [])

    def test_published_release_cannot_be_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "assets"
            self.fixture(root)
            with mock.patch.object(self.package, "validate_tag", return_value=self.commit), \
                    mock.patch.object(self.package.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, '{"isDraft":false}', "")) as call:
                with self.assertRaisesRegex(RuntimeError, "published release"):
                    self.package.draft_release("v1.2.3", root)
                self.assertEqual(call.call_count, 1)

    def test_bad_version_fails_before_git_or_network_operations(self):
        with mock.patch.object(self.package.subprocess, "check_output") as command:
            for tag in ("main", "v999999.0.0", "v1.2.3\nmalformed"):
                with self.subTest(tag=tag), self.assertRaises(RuntimeError):
                    self.package.validate_tag(tag)
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
