#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Build and verify an arm64 application package with matching sources and notices."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

import bundle

OWNER = "zirq-macos-package-v1"
LEGACY_OWNER = "kwi-macos-package-v1"
ROOT = Path(__file__).resolve().parents[2]
ASSET_FILES = ("Zirq-arm64.zip", "Zirq-sources.tar.gz", "SOURCES.json", "package.json", "Zirq.bundle.json", "README.txt", "dependencies.json")


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
        if record.get("owner") not in (OWNER, LEGACY_OWNER) or record.get("published_files") != snapshot(path, ("package.json",)):
            raise RuntimeError("Existing output is unowned or changed; preserve it and choose a fresh output")


def publish(staged, destination, validate=check_output):
    validate(destination)
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


def verify_assets(directory, expected_commit=None):
    if not directory.is_dir():
        raise RuntimeError("Release assets directory does not exist; run make ci first")
    if directory.is_symlink() or {p.name for p in directory.iterdir()} != {*ASSET_FILES, "SHA256SUMS"}:
        raise RuntimeError("Release assets must contain exactly the expected files")
    if any(p.is_symlink() or not p.is_file() for p in directory.iterdir()):
        raise RuntimeError("Release assets must be regular files")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] not in ASSET_FILES or match[2] in checksums:
            raise RuntimeError("Invalid release checksum manifest")
        checksums[match[2]] = match[1]
    if set(checksums) != set(ASSET_FILES):
        raise RuntimeError("Incomplete release checksum manifest")
    for name, expected in checksums.items():
        if digest(directory / name) != expected:
            raise RuntimeError(f"Release asset changed: {name}")
    record = json.loads((directory / "package.json").read_text())
    sources = json.loads((directory / "SOURCES.json").read_text())
    dependencies = json.loads((directory / "dependencies.json").read_text())
    manifest = json.loads((directory / "Zirq.bundle.json").read_text())
    commit = record["source"]["source_commit"]
    if (not re.fullmatch(r"[0-9a-f]{40}", commit) or sources["project_commit"] != commit
            or dependencies["source_commit"] != commit or (expected_commit and expected_commit != commit)):
        raise RuntimeError("Release source identity does not match")
    if (record["owner"] != OWNER or not record["full_deployment"] or manifest["reused_deployment"]
            or not record["strict_signatures_original_and_extracted"] or not record["archive_round_trip_bytes_modes_symlinks"]):
        raise RuntimeError("Release package verification is incomplete")
    if (record["archive"]["name"] != "Zirq-arm64.zip" or record["archive"]["sha256"] != checksums["Zirq-arm64.zip"]
            or record["archive"]["bytes"] != (directory / "Zirq-arm64.zip").stat().st_size
            or record["source_distribution"]["archive"] != "Zirq-sources.tar.gz"
            or record["source_distribution"]["sha256"] != checksums["Zirq-sources.tar.gz"]
            or record["bundle_manifest_sha256"] != checksums["Zirq.bundle.json"]):
        raise RuntimeError("Release metadata does not match its archives")
    return commit


def check_assets_output(directory):
    if directory.is_symlink():
        raise RuntimeError("Release asset output must not be a symlink")
    if directory.exists():
        verify_assets(directory)


def assemble_assets(package_dir, dependencies, destination):
    canonical = destination.resolve()
    if canonical == package_dir or canonical.is_relative_to(package_dir) or package_dir.is_relative_to(canonical):
        raise RuntimeError("Release assets and package directories must be separate")
    check_assets_output(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".zirq-assets-", dir=destination.parent) as temporary:
        staged = Path(temporary) / "assets"
        staged.mkdir()
        for name in ASSET_FILES:
            shutil.copy2(dependencies if name == "dependencies.json" else package_dir / name, staged / name)
        (staged / "SHA256SUMS").write_text("".join(f"{digest(staged / name)}  {name}\n" for name in ASSET_FILES))
        verify_assets(staged)
        publish(staged, destination, validate=check_assets_output)


def validate_tag(tag):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-mac\.[0-9]+)?", tag):
        raise RuntimeError("Set TAG to an existing version tag")
    version = re.search(r"project\(zirq VERSION ([\d.]+)", (ROOT / "CMakeLists.txt").read_text())[1]
    if tag.removeprefix("v").split("-mac.")[0] != version:
        raise RuntimeError("Tag must match the CMake project version")
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", f"refs/tags/{tag}^{{commit}}"], text=True).strip()
    head = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    if commit != head:
        raise RuntimeError("Check out the release tag before preparing a draft")
    return commit


def draft_release(tag, directory):
    commit = validate_tag(tag)
    verify_assets(directory, commit)
    state = subprocess.run(["gh", "release", "view", tag, "--json", "isDraft"], capture_output=True, text=True, cwd=ROOT)
    if state.returncode == 0:
        if not json.loads(state.stdout)["isDraft"]:
            raise RuntimeError("Refusing to replace a published release")
    elif "release not found" not in state.stderr.lower():
        raise RuntimeError(state.stderr.strip() or "Cannot inspect GitHub release")
    notes = (f"Zirq — Apple Silicon\n\nSource: {commit}\n\n"
             "An independently maintained macOS fork of KDE Kwi.\n\n"
             "Extract Zirq.app from Zirq-arm64.zip and copy it to Applications.\n"
             "Ad-hoc signed; Developer ID signing and notarization are not configured.\n"
             "Headless CI tests and package audits pass. GUI/Metal checks remain local.\n"
             "Dependency notices are bundled in the app. Zirq-sources.tar.gz contains matching application, dependency and build recipe sources.\n"
             "SOURCES.json lists versions and hashes. Verify downloads with SHA256SUMS.\n"
             "Review this draft before publication.\n")
    with tempfile.TemporaryDirectory(prefix="zirq-release-notes-") as temporary:
        note_file = Path(temporary) / "notes.txt"
        note_file.write_text(notes)
        flags = ["--draft", "--prerelease=" + str("-" in tag).lower(), "--notes-file", str(note_file)]
        assets = [str(directory / name) for name in (*ASSET_FILES, "SHA256SUMS")]
        if state.returncode == 0:
            subprocess.run(["gh", "release", "upload", tag, *assets, "--clobber"], check=True, cwd=ROOT)
            subprocess.run(["gh", "release", "edit", tag, *flags], check=True, cwd=ROOT)
        else:
            subprocess.run(["gh", "release", "create", tag, *assets, "--verify-tag", "--title", f"Zirq {tag}", *flags], check=True, cwd=ROOT)


def build(args):
    destination = args.output.absolute()
    check_output(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".zirq-package-", dir=destination.parent) as temporary:
        work = Path(temporary)
        staged = work / "package"
        staged.mkdir()
        app = staged / "Zirq.app"
        # Final packages always rediscover the complete Qt/QML/plugin closure.
        subprocess.run([sys.executable, str(ROOT / "scripts/macos/bundle.py"), "--prefix", str(args.prefix.resolve()), "--output", str(app)],
                       check=True, env=bundle.tool_environment(), timeout=1200)
        import source_bundle
        source_distribution = source_bundle.prepare(args.prefix.resolve(), app, staged)
        bundle.run(["codesign", "--force", "--sign", "-", app])
        bundle.run(["codesign", "--verify", "--deep", "--strict", app])
        manifest_path = staged / "Zirq.bundle.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["resources"] = bundle.inventory_resources(app)
        manifest["files"] = {p.relative_to(app).as_posix(): digest(p) for p in app.rglob("*") if p.is_file() and not p.is_symlink()}
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        archive = staged / "Zirq-arm64.zip"
        bundle.run(["/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, archive])
        extracted = work / "Extracted Zirq-é"
        bundle.run(["/usr/bin/ditto", "-x", "-k", archive, extracted])
        restored = extracted / "Zirq.app"
        bundle.run(["codesign", "--verify", "--deep", "--strict", restored])
        if snapshot(app) != snapshot(restored):
            raise RuntimeError("Archive extraction changed app bytes, permissions or symlinks")
        native = bundle.audit(restored)
        resources = bundle.inventory_resources(restored)
        (staged / "README.txt").write_text(
            "Zirq — Apple Silicon\n\n"
            "Open Zirq.app, or copy it to a location on this Mac. Keep the app bundle intact.\n"
            "The ZIP contains the same verified app. No Craft shell or loader/QML environment overrides are needed.\n"
            "Ad-hoc signed; Developer ID signing and notarization are not configured.\n"
            "Installed license notices are in Zirq.app/Contents/Resources/licenses.\n"
            "Zirq-sources.tar.gz includes application, dependency and build recipe sources; SOURCES.json maps versions and archives.\n")
        record = {"owner": OWNER, "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "scope": "Apple Silicon macOS application package",
                  "full_deployment": True, "strict_signatures_original_and_extracted": True,
                  "archive_round_trip_bytes_modes_symlinks": True, "mach_o_files": len(native),
                  "archive": {"name": archive.name, "sha256": digest(archive), "bytes": archive.stat().st_size},
                  "executable_sha256": digest(app / "Contents/MacOS/zirq"), "resources": resources,
                  "bundle_manifest_sha256": digest(staged / "Zirq.bundle.json"),
                  "source": json.loads((app / "Contents/Resources/zirq-bundle.json").read_text()),
                  "source_distribution": source_distribution, "notarized": False,
                  "published_files": snapshot(staged)}
        (staged / "package.json").write_text(json.dumps(record, indent=2) + "\n")
        publish(staged, destination)
    print(f"Verified package: {destination}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", default="build", choices=("build", "assets", "verify", "validate-tag", "draft"))
    parser.add_argument("--prefix", type=Path, default=ROOT / "build-macos/craft-root")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--package-dir", type=Path, default=ROOT / "build-macos/package")
    parser.add_argument("--dependencies", type=Path, default=ROOT / "build-macos/dependencies.json")
    parser.add_argument("--tag", default=os.environ.get("RELEASE_TAG") or os.environ.get("TAG", ""))
    parser.add_argument("--commit", default=os.environ.get("EXPECTED_COMMIT") or None)
    args = parser.parse_args()
    args.output = (args.output or ROOT / "build-macos" / ("package" if args.action == "build" else "release-assets")).absolute()
    if args.action == "build":
        build(args)
    elif args.action == "assets":
        assemble_assets(args.package_dir.resolve(), args.dependencies.resolve(), args.output)
    elif args.action == "verify":
        print(f"Verified release assets for {verify_assets(args.output, args.commit)}")
    elif args.action == "validate-tag":
        print(f"Validated {args.tag}: {validate_tag(args.tag)}")
    elif args.action == "draft":
        draft_release(args.tag, args.output)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
