# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0
"""Exercise installer replacement and recovery using isolated release/OS fixtures."""
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

from test_build_tools import ROOT


SHIM = r'''
import os, plistlib, shutil, subprocess, sys, zipfile
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
fixture = Path(os.environ["KWI_FIXTURE"])
if name == "uname":
    print(os.environ.get("KWI_FIXTURE_OS", "Darwin") if args == ["-s"] else os.environ.get("KWI_FIXTURE_ARCH", "arm64"))
elif name == "sysctl":
    print("0")
elif name == "sw_vers":
    print(os.environ.get("KWI_FIXTURE_OS_VERSION", "27.2.0"))
elif name == "curl":
    with (fixture / "requests").open("a") as out:
        out.write(args[-1] + "\n")
    if "--write-out" in args:
        print("https://github.com/MohamedElashri/zirq/releases/tag/v0.2.0", end="")
    else:
        source = fixture / ("SHA256SUMS" if args[-1].endswith("SHA256SUMS") else "release.zip")
        shutil.copyfile(source, args[args.index("--output") + 1])
elif name == "plutil":
    print(plistlib.loads(Path(args[-1]).read_bytes())[args[1]])
elif name == "codesign":
    sys.exit(1 if (Path(args[-1]) / "bad-signature").exists() else int(os.environ.get("KWI_FIXTURE_BAD_SIGNATURE", "0")))
elif name == "pgrep":
    sys.exit(3 if os.environ.get("KWI_FIXTURE_PROCESS_ERROR") else 0 if os.environ.get("KWI_FIXTURE_RUNNING") else 1)
elif name == "ditto":
    with zipfile.ZipFile(args[2]) as archive:
        archive.extractall(args[3])
        for member in archive.infolist():
            (Path(args[3]) / member.filename).chmod((member.external_attr >> 16) & 0o777)
elif name == "mv":
    if os.environ.get("KWI_FIXTURE_FAIL_REPLACE") and "/extracted/Zirq.app" in args[0]:
        sys.exit(1)
    sys.exit(subprocess.call([os.environ["KWI_FIXTURE_REAL_MV"], *args]))
'''


class InstallerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="kwi installer ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.apps = self.root / "Applications with spaces"
        self.tools = self.root / "tools"
        self.tools.mkdir()
        command = self.tools / "fixture"
        command.write_text(f"#!{sys.executable}\n" + SHIM)
        command.chmod(0o755)
        for name in ("uname", "sysctl", "sw_vers", "curl", "plutil", "codesign", "pgrep", "ditto", "mv"):
            (self.tools / name).symlink_to(command)
        self.env = {**os.environ, "PATH": str(self.tools) + os.pathsep + os.environ["PATH"],
                    "KWI_FIXTURE": str(self.root), "KWI_FIXTURE_REAL_MV": shutil.which("mv")}
        self.release()

    @staticmethod
    def bundle(app, version="0.1.0", identifier="com.elashri.zirq", minimum="14.4", executable="zirq"):
        binary = app / "Contents/MacOS" / executable
        binary.parent.mkdir(parents=True)
        binary.write_text("previous app" if version == "0.1.0" else "new app")
        binary.chmod(0o755)
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": identifier, "CFBundleExecutable": executable,
            "CFBundleShortVersionString": version, "LSMinimumSystemVersion": minimum}))
        return app

    def release(self, version="0.2.0", product="Zirq", **kwargs):
        source = self.root / "source"
        if source.exists():
            shutil.rmtree(source)
        app = self.bundle(source / f"{product}.app", version=version, **kwargs)
        with zipfile.ZipFile(self.root / "release.zip", "w") as archive:
            for path in app.rglob("*"):
                archive.write(path, path.relative_to(source))
        digest = hashlib.sha256((self.root / "release.zip").read_bytes()).hexdigest()
        (self.root / "SHA256SUMS").write_text(f"{digest}  {product}-arm64.zip\n")

    def run_installer(self, **env):
        return subprocess.run(["/bin/bash", str(ROOT / "install.sh"), "--app-dir", str(self.apps)],
                              env={**self.env, **env}, text=True, capture_output=True, timeout=20)

    def installed_version(self):
        return plistlib.loads((self.apps / "Zirq.app/Contents/Info.plist").read_bytes())["CFBundleShortVersionString"]

    def assert_preserved(self, **env):
        self.bundle(self.apps / "Zirq.app")
        result = self.run_installer(**env)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.installed_version(), "0.1.0")
        self.assertEqual((self.apps / "Zirq.app/Contents/MacOS/zirq").read_text(), "previous app")
        self.assertEqual(list(self.apps.glob(".zirq*")), [])
        return result

    def test_fresh_install_then_current_version_is_a_noop(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.installed_version(), "0.2.0")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("already installed", result.stdout)
        self.assertEqual((self.root / "requests").read_text().count("/Zirq-arm64.zip"), 1)

    def test_upgrade_replaces_only_zirq(self):
        self.bundle(self.apps / "Zirq.app")
        other = self.apps / "other-file"
        other.write_text("keep")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.installed_version(), "0.2.0")
        self.assertEqual(other.read_text(), "keep")
        self.assertEqual(list(self.apps.glob(".zirq*")), [])

    def test_legacy_release_installs_under_the_new_app_name(self):
        self.release(product="Kwi", identifier="org.kde.kwi", executable="kwi")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.installed_version(), "0.2.0")
        self.assertTrue((self.apps / "Zirq.app/Contents/MacOS/kwi").is_file())
        self.assertIn("/Kwi-arm64.zip", (self.root / "requests").read_text())

    def test_legacy_app_is_upgraded_and_renamed(self):
        self.bundle(self.apps / "Kwi.app", identifier="org.kde.kwi", executable="kwi")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.apps / "Kwi.app").exists())
        self.assertEqual(self.installed_version(), "0.2.0")

    def test_current_legacy_app_is_renamed_without_downloading(self):
        self.bundle(self.apps / "Kwi.app", version="0.2.0", identifier="org.kde.kwi", executable="kwi")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.apps / "Kwi.app").exists())
        self.assertTrue((self.apps / "Zirq.app/Contents/MacOS/kwi").is_file())
        self.assertNotIn("/SHA256SUMS", (self.root / "requests").read_text())

    def test_failed_migration_restores_the_legacy_name_and_contents(self):
        self.bundle(self.apps / "Kwi.app", identifier="org.kde.kwi", executable="kwi")
        result = self.run_installer(KWI_FIXTURE_FAIL_REPLACE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Previous installation restored", result.stderr)
        self.assertEqual((self.apps / "Kwi.app/Contents/MacOS/kwi").read_text(), "previous app")
        self.assertFalse((self.apps / "Zirq.app").exists())
        self.assertEqual(list(self.apps.glob(".zirq*")), [])
        self.assertFalse((self.apps / ".kwi-install.lock").exists())

    def test_legacy_installer_lock_is_respected_and_preserved(self):
        self.apps.mkdir()
        lock = self.apps / ".kwi-install.lock"
        lock.mkdir()
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(lock.is_dir())
        self.assertFalse((self.apps / ".zirq-install.lock").exists())
        self.assertFalse((self.root / "requests").exists())

    def test_current_but_damaged_app_is_repaired(self):
        app = self.bundle(self.apps / "Zirq.app", version="0.2.0")
        (app / "bad-signature").write_text("damaged")
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((app / "bad-signature").exists())

    def test_newer_installation_is_not_downgraded(self):
        self.bundle(self.apps / "Zirq.app", version="0.3.0")
        self.assertEqual(self.run_installer().returncode, 0)
        self.assertEqual(self.installed_version(), "0.3.0")
        self.assertNotIn("/Zirq-arm64.zip", (self.root / "requests").read_text())

    def test_bad_checksum_preserves_previous_app(self):
        (self.root / "SHA256SUMS").write_text("0" * 64 + "  Zirq-arm64.zip\n")
        self.assertIn("checksum verification failed", self.assert_preserved().stderr)

    def test_bad_signature_preserves_previous_app(self):
        self.assert_preserved(KWI_FIXTURE_BAD_SIGNATURE="1")

    def test_failed_replacement_restores_previous_app(self):
        result = self.assert_preserved(KWI_FIXTURE_FAIL_REPLACE="1")
        self.assertIn("Previous installation restored", result.stderr)

    def test_running_app_is_left_alone(self):
        self.assertIn("Quit Zirq", self.assert_preserved(KWI_FIXTURE_RUNNING="1").stderr)

    def test_process_query_failure_preserves_the_installed_app(self):
        self.assertIn("Cannot check", self.assert_preserved(KWI_FIXTURE_PROCESS_ERROR="1").stderr)

    def test_truncated_pipe_does_not_start_an_install(self):
        truncated = (ROOT / "install.sh").read_text().rsplit("\n}", 1)[0]
        result = subprocess.run(["/bin/bash", "-s", "--", "--app-dir", str(self.apps)], input=truncated,
                                env=self.env, text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "requests").exists())
        self.assertFalse(self.apps.exists())

    def test_incompatible_macos_preserves_previous_app(self):
        self.assertIn("macOS 14.4", self.assert_preserved(KWI_FIXTURE_OS_VERSION="14.3.1").stderr)

    def test_unexpected_downloaded_bundle_is_rejected(self):
        self.release(identifier="example.unrelated")
        self.assert_preserved()

    def test_archive_paths_cannot_escape_the_install_work_directory(self):
        with zipfile.ZipFile(self.root / "release.zip", "a") as archive:
            archive.writestr("../escape", "unwanted file")
        digest = hashlib.sha256((self.root / "release.zip").read_bytes()).hexdigest()
        (self.root / "SHA256SUMS").write_text(f"{digest}  Zirq-arm64.zip\n")
        self.assertIn("unexpected paths", self.assert_preserved().stderr)
        self.assertFalse((self.apps / "escape").exists())

    def test_app_version_must_match_the_release_tag(self):
        self.release(version="9.9.9")
        self.assertIn("version does not match", self.assert_preserved().stderr)

    def test_unrelated_existing_app_is_not_replaced(self):
        self.bundle(self.apps / "Zirq.app", identifier="example.unrelated")
        self.assertNotEqual(self.run_installer().returncode, 0)
        self.assertEqual(self.installed_version(), "0.1.0")

    def test_symlink_destination_is_not_followed(self):
        outside = self.bundle(self.root / "outside.app")
        self.apps.mkdir()
        (self.apps / "Zirq.app").symlink_to(outside)
        self.assertNotEqual(self.run_installer().returncode, 0)
        self.assertTrue((self.apps / "Zirq.app").is_symlink())
        self.assertTrue(outside.is_dir())

    def test_unsupported_platforms_fail_before_downloading(self):
        for env in ({"KWI_FIXTURE_OS": "Linux"}, {"KWI_FIXTURE_ARCH": "x86_64"}):
            with self.subTest(env=env):
                self.assertNotEqual(self.run_installer(**env).returncode, 0)
                self.assertFalse((self.root / "requests").exists())
                self.assertFalse(self.apps.exists())


if __name__ == "__main__":
    unittest.main()
