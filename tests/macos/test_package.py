# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from test_build_tools import load


class PackageTest(unittest.TestCase):
    def modules(self):
        bundle = load("bundle")
        with mock.patch.dict("sys.modules", {"bundle": bundle}):
            package = load("package")
        return bundle, package

    def test_deployed_dependency_cannot_fall_back_to_prefix(self):
        bundle, _ = self.modules()
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "Zirq.app"
            binary = app / "Contents/MacOS/zirq"
            binary.parent.mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, "Unresolved deployed"):
                bundle.resolve_dependency(app, binary, "@rpath/libMissing.dylib", ["@executable_path/../Frameworks"])
            outside = Path(directory) / "outside.dylib"
            outside.write_bytes(b"outside")
            with self.assertRaisesRegex(RuntimeError, "escapes"):
                bundle.resolve_dependency(app, binary, "@loader_path/../../../outside.dylib", [])

    def test_internal_version_alias_resolves_inside_app(self):
        bundle, _ = self.modules()
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "Zirq.app"
            frameworks = app / "Contents/Frameworks"
            frameworks.mkdir(parents=True)
            (app / "Contents/MacOS").mkdir()
            real = frameworks / "libExample.1.2.dylib"
            real.write_bytes(b"library")
            (frameworks / "libExample.1.dylib").symlink_to(real.name)
            resolved = bundle.resolve_dependency(app, app / "Contents/MacOS/zirq", "@rpath/libExample.1.dylib", ["@executable_path/../Frameworks"])
            self.assertEqual(resolved, real.resolve())

    def test_modified_or_unowned_package_is_preserved(self):
        _, package = self.modules()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "package"
            output.mkdir()
            data = output / "user-data"
            data.write_text("original")
            with self.assertRaisesRegex(RuntimeError, "not owned"):
                package.check_output(output)
            (output / "package.json").write_text(json.dumps({"owner": package.OWNER, "published_files": package.snapshot(output)}))
            package.check_output(output)
            data.write_text("user edit")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                package.check_output(output)
            self.assertEqual(data.read_text(), "user edit")

    def test_failed_publish_restores_previous_package(self):
        _, package = self.modules()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "package"
            output.mkdir()
            (output / "old").write_text("preserve")
            (output / "package.json").write_text(json.dumps({"owner": package.OWNER, "published_files": package.snapshot(output)}))
            staged = Path(directory) / "staged"
            staged.mkdir()
            original_replace = Path.replace
            def fail_promotion(path, target):
                if path == staged:
                    raise OSError("simulated publish failure")
                return original_replace(path, target)
            with mock.patch.object(Path, "replace", fail_promotion):
                with self.assertRaisesRegex(OSError, "publish failure"):
                    package.publish(staged, output)
            self.assertEqual((output / "old").read_text(), "preserve")
            package.check_output(output)

    def test_snapshot_detects_executable_mode_and_link_changes(self):
        _, package = self.modules()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "program"
            file.write_bytes(b"same bytes")
            (root / "alias").symlink_to("program")
            before = package.snapshot(root)
            file.chmod(0o755)
            self.assertNotEqual(before, package.snapshot(root))
            before = package.snapshot(root)
            (root / "alias").unlink()
            (root / "alias").symlink_to("elsewhere")
            self.assertNotEqual(before, package.snapshot(root))

    def test_live_app_refusal_happens_before_prefix_hiding(self):
        smoke = load("smoke")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "craft-root"
            (prefix / "etc").mkdir(parents=True)
            (prefix / "etc/kwi-bootstrap.json").write_text('{}')
            with mock.patch("sys.argv", ["smoke.py", "--app", str(root / "Zirq.app"), "--output", str(root / "run/result.json"),
                                         "--hide-prefix", str(prefix)]), mock.patch.object(smoke, "ensure_kwi_not_running", side_effect=RuntimeError("live app")), \
                    mock.patch.object(smoke.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(RuntimeError, "live app"):
                    smoke.main()
            launch.assert_not_called()
            self.assertTrue(prefix.exists())
            self.assertFalse(prefix.with_name("craft-root.smoke-hidden").exists())
