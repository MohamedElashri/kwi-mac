#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Build and verify a complete local arm64 development package before publishing it."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

import bundle

OWNER = "kwi-macos-package-v1"
ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def snapshot(root, exclude=()):
    result = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if relative in exclude:
            continue
        if path.is_symlink():
            result[relative] = {"symlink": os.readlink(path)}
        elif path.is_file():
            result[relative] = {"sha256": digest(path), "mode": stat.S_IMODE(path.stat().st_mode)}
        elif path.is_dir():
            result[relative] = {"directory": True, "mode": stat.S_IMODE(path.stat().st_mode)}
    return result


def check_output(path):
    if path.is_symlink():
        raise RuntimeError("Package output must not be a symlink")
    if path.exists():
        marker = path / "package.json"
        if not marker.is_file() or marker.is_symlink():
            raise RuntimeError("Existing package output is not owned by this tool")
        record = json.loads(marker.read_text())
        if record.get("owner") != OWNER or record.get("published_files") != snapshot(path, ("package.json",)):
            raise RuntimeError("Existing output is unowned or changed; preserve it and choose a fresh output")


def publish(staged, destination):
    check_output(destination)
    backup = destination.with_name("." + destination.name + ".previous-" + uuid.uuid4().hex)
    had_previous = destination.exists()
    if had_previous:
        destination.replace(backup)
    try:
        staged.replace(destination)
    except BaseException:
        if had_previous:
            backup.replace(destination)
        raise
    if had_previous:
        shutil.rmtree(backup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=ROOT / "build-macos/craft-root")
    parser.add_argument("--output", type=Path, default=ROOT / "build-macos/phase5/package")
    args = parser.parse_args()
    destination = args.output.absolute()
    check_output(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".kwi-package-", dir=destination.parent) as temporary:
        work = Path(temporary)
        staged = work / "package"
        staged.mkdir()
        app = staged / "Kwi.app"
        # Final packages always rediscover the complete Qt/QML/plugin closure.
        subprocess.run([sys.executable, str(ROOT / "scripts/macos/bundle.py"), "--prefix", str(args.prefix.resolve()), "--output", str(app)],
                       check=True, env=bundle.tool_environment(), timeout=1200)
        bundle.run(["codesign", "--verify", "--deep", "--strict", app])
        archive = staged / "Kwi-arm64.zip"
        bundle.run(["/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, archive])
        extracted = work / "Extracted Kwi-é"
        bundle.run(["/usr/bin/ditto", "-x", "-k", archive, extracted])
        restored = extracted / "Kwi.app"
        bundle.run(["codesign", "--verify", "--deep", "--strict", restored])
        if snapshot(app) != snapshot(restored):
            raise RuntimeError("Archive extraction changed app bytes, permissions or symlinks")
        native = bundle.audit(restored)
        resources = bundle.inventory_resources(restored)
        (staged / "README.txt").write_text(
            "Kwi: local Apple Silicon development build\n\n"
            "Open Kwi.app, or copy it to a location on this Mac. Keep the app bundle intact.\n"
            "The ZIP contains the same verified app. No Craft shell or loader/QML environment overrides are needed.\n"
            "Signing is ad-hoc for local development; Developer ID/notarization and public redistribution are not verified.\n"
            "Installed license notices are in Kwi.app/Contents/Resources/licenses.\n"
            "Public distribution requires a separate dependency notice/source-obligation review.\n")
        record = {"owner": OWNER, "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "scope": "Local Apple Silicon development; public release verification is separate",
                  "full_deployment": True, "strict_signatures_original_and_extracted": True,
                  "archive_round_trip_bytes_modes_symlinks": True, "mach_o_files": len(native),
                  "archive": {"name": archive.name, "sha256": digest(archive), "bytes": archive.stat().st_size},
                  "executable_sha256": digest(app / "Contents/MacOS/kwi"), "resources": resources,
                  "bundle_manifest_sha256": digest(staged / "Kwi.bundle.json"),
                  "source": json.loads((app / "Contents/Resources/kwi-bundle.json").read_text()),
                  "public_notice_and_source_closure_verified": False,
                  "published_files": snapshot(staged)}
        (staged / "package.json").write_text(json.dumps(record, indent=2) + "\n")
        publish(staged, destination)
    print(f"Verified local package: {destination}", flush=True)


if __name__ == "__main__":
    main()
