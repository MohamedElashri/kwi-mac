# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
"""Safety checks for build-prefix ownership and checksum-verified source downloads."""
import hashlib
import importlib.util
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts/macos" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.object(sys, "path", [str(ROOT / "scripts/macos"), *sys.path]):
        spec.loader.exec_module(module)
    return module


class BuildToolsTest(unittest.TestCase):
    def test_invalid_job_count_does_not_modify_a_prefix(self):
        craft = load("craft")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "prefix"
            with self.assertRaisesRegex(RuntimeError, "positive"):
                craft.prepare(root, jobs=0)
            self.assertFalse(root.exists())

    def test_measurement_does_not_forward_to_a_live_app(self):
        app_test_support = load("app_test_support")
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "owned.socket.lock"
            lock.write_text("12345\nkwi\n")
            with mock.patch.object(app_test_support.os, "kill") as check:
                with self.assertRaisesRegex(RuntimeError, "not modified"):
                    app_test_support.ensure_kwi_not_running(lock)
                check.assert_called_once_with(12345, 0)
            self.assertEqual(lock.read_text(), "12345\nkwi\n")

    def test_measurement_leaves_stale_endpoint_cleanup_to_qt(self):
        app_test_support = load("app_test_support")
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "owned.socket.lock"
            lock.write_text("12345\nkwi\n")
            with mock.patch.object(app_test_support.os, "kill", side_effect=ProcessLookupError):
                app_test_support.ensure_kwi_not_running(lock)
            self.assertTrue(lock.exists())

    def test_output_completion_requires_matching_prepared_terminal(self):
        smoke = load("smoke")
        events = [
            {"kind": "marker-snapshot", "terminal_id": 9, "generation": 5, "time_ns": 10},
            {"kind": "frame-swapped", "time_ns": 11},
            {"kind": "render", "terminal_id": 9, "generation": 4, "time_ns": 12},
            {"kind": "frame-swapped", "time_ns": 13},
            {"kind": "render", "terminal_id": 8, "generation": 7, "time_ns": 14},
            {"kind": "frame-swapped", "time_ns": 15},
        ]
        self.assertIsNone(smoke.completion(events))
        events.extend([
            {"kind": "render", "terminal_id": 9, "generation": 6, "time_ns": 16},
            {"kind": "frame-swapped", "time_ns": 17},
        ])
        marker, prepared, frame = smoke.completion(events)
        self.assertEqual((marker["generation"], prepared["generation"], frame["time_ns"]), (5, 6, 17))

    def test_versioned_library_aliases_share_one_image(self):
        bundle = load("bundle")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "prefix"
            (prefix / "lib").mkdir(parents=True)
            (prefix / "lib/libExample.6.1.dylib").write_bytes(b"library")
            (prefix / "lib/libExample.6.dylib").symlink_to("libExample.6.1.dylib")
            app = root / "Kwi.app"
            frameworks = app / "Contents/Frameworks"
            frameworks.mkdir(parents=True)
            (frameworks / "libExample.6.1.dylib").write_bytes(b"current image")
            (frameworks / "libExample.6.dylib").write_bytes(b"copied alias")
            bundle.normalize_library_aliases(app, prefix)
            self.assertTrue((frameworks / "libExample.6.dylib").is_symlink())
            self.assertEqual((frameworks / "libExample.6.dylib").read_bytes(), b"current image")

    def test_bundle_failure_preserves_previous_output(self):
        bundle = load("bundle")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "Kwi.app"
            marker = app / "Contents/Resources/kwi-bundle.json"
            marker.parent.mkdir(parents=True)
            marker.write_text('{"owner":"kwi-macos-bundle"}')
            sentinel = app / "previous-app"
            sentinel.write_text("preserve me")
            with mock.patch("sys.argv", ["bundle.py", "--prefix", str(root / "missing"), "--output", str(app)]), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    bundle.main()
            self.assertEqual(sentinel.read_text(), "preserve me")

    def test_bundle_audit_rejects_external_symlink(self):
        bundle = load("bundle")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "Kwi.app"
            app.mkdir()
            target = root / "external-data"
            target.write_text("external")
            (app / "link").symlink_to(target)
            with self.assertRaisesRegex(RuntimeError, "escapes"):
                bundle.audit(app)

    def test_smoke_conflict_preserves_prefix_and_hidden_directory(self):
        smoke = load("smoke")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "craft-root"
            (prefix / "etc").mkdir(parents=True)
            (prefix / "etc/kwi-bootstrap.json").write_text('{}')
            hidden = root / "craft-root.smoke-hidden"
            hidden.mkdir()
            sentinel = hidden / "preserve"
            sentinel.write_text("other data")
            with mock.patch("sys.argv", ["smoke.py", "--app", str(root / "Kwi.app"), "--output", str(root / "result.json"), "--hide-prefix", str(prefix)]), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    smoke.main()
            self.assertEqual(sentinel.read_text(), "other data")
            self.assertTrue((prefix / "etc/kwi-bootstrap.json").exists())

    def test_existing_unowned_prefix_is_preserved(self):
        craft = load("craft")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sentinel = root / "user-data"
            sentinel.write_text("preserve me")
            with self.assertRaisesRegex(RuntimeError, "not owned"):
                craft.prepare(root)
            self.assertEqual(sentinel.read_text(), "preserve me")
            self.assertEqual(list(root.iterdir()), [sentinel])

    def test_mismatched_prefix_pins_are_rejected_without_fetch(self):
        craft = load("craft")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "etc").mkdir()
            (root / "etc/kwi-bootstrap.json").write_text('{"craft":"different"}')
            with mock.patch.object(craft, "checkout") as checkout:
                with self.assertRaisesRegex(RuntimeError, "pins differ"):
                    craft.prepare(root)
                checkout.assert_not_called()

    def test_wrong_download_is_never_promoted_to_cache(self):
        ghostty = load("ghostty")
        source = {"url": "https://example.invalid/package.tar.gz", "sha256": hashlib.sha256(b"reviewed source").hexdigest()}
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            with mock.patch.object(ghostty, "download", side_effect=lambda url, path: path.write_bytes(b"unexpected bytes")):
                with self.assertRaisesRegex(RuntimeError, "SHA256 mismatch"):
                    ghostty.fetch(source, cache)
            self.assertFalse((cache / (source["sha256"] + "-package.tar.gz")).exists())

    def test_verified_cache_can_be_reused_offline(self):
        ghostty = load("ghostty")
        body = b"reviewed source"
        source = {"url": "https://example.invalid/package.tar.gz", "sha256": hashlib.sha256(body).hexdigest()}
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            with mock.patch.object(ghostty, "download", side_effect=lambda url, path: path.write_bytes(body)):
                path = ghostty.fetch(source, cache)
            with mock.patch.object(ghostty, "download", side_effect=AssertionError("network access")):
                self.assertEqual(ghostty.fetch(source, cache), path)
            self.assertEqual(path.read_bytes(), body)


if __name__ == "__main__":
    unittest.main()
