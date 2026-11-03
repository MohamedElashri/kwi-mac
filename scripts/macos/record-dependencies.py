#!/usr/bin/env python3
# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
"""Record installed Craft receipts, downloaded inputs and target-library Mach-O floors."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import subprocess


def command(args):
    return subprocess.check_output([str(a) for a in args], text=True, timeout=30).strip()


def inspect(path):
    architectures = command(["lipo", "-archs", path]).split()
    load = command(["otool", "-l", path])
    minimum = re.findall(r"\bminos\s+(\d+(?:\.\d+)*)", load)
    if not minimum:
        minimum = re.findall(r"LC_VERSION_MIN_MACOSX\s+cmdsize\s+\d+\s+version\s+([\d.]+)", load)
    return {"architectures": architectures, "minimum_macos": minimum,
            "sdk": re.findall(r"\bsdk\s+(\d+(?:\.\d+)*)", load)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    database = prefix / "etc/blueprints/install.db"
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        receipts = [dict(row) for row in db.execute("SELECT packagePath,version,revision,cacheVersion FROM packageList ORDER BY packagePath")]
    archives = []
    for path in sorted((prefix / "download").rglob("*")):
        if path.is_file() and any(path.name.endswith(ext) for ext in (".7z", ".tar.xz", ".tar.gz", ".pem")):
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            archives.append({"path": path.relative_to(prefix).as_posix(), "bytes": path.stat().st_size, "sha256": digest})
    paths = set()
    for directory in ("lib", "plugins", "qml"):
        base = prefix / directory
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and not path.is_symlink():
                with path.open("rb") as stream:
                    magic = stream.read(4)
                if magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                             b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
                    paths.add(path)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        libraries = {p.relative_to(prefix).as_posix(): data for p, data in zip(sorted(paths), pool.map(inspect, sorted(paths)))}
    floor = (14, 4, 0)
    problems = []
    for name, data in libraries.items():
        if "arm64" not in data["architectures"]:
            problems.append(name + ": missing arm64")
        if not data["minimum_macos"]:
            problems.append(name + ": deployment target not found")
        for version in data["minimum_macos"]:
            parts = tuple(int(n) for n in version.split("."))
            if (parts + (0,) * (3 - len(parts))) > floor:
                problems.append(name + ": requires macOS " + version)
    root = Path(__file__).resolve().parents[2]
    local_inputs = [*root.glob("scripts/macos/*.py"), *root.glob("craft/**/*.py"), *root.glob("craft/*.json"),
                    root / "Makefile", *root.glob(".github/workflows/*.yml")]
    result = {
        "status": "installed dependency snapshot; not a runtime or minimum-OS test",
        "source_commit": command(["git", "rev-parse", "HEAD"]),
        "craft_commit": command(["git", "-C", prefix / "craft", "rev-parse", "HEAD"]),
        "blueprints_commit": command(["git", "-C", prefix / "etc/blueprints/locations/craft-blueprints-kde", "rev-parse", "HEAD"]),
        "deployment_target": "14.4", "architecture": "arm64",
        "sdk_version": command(["xcrun", "--show-sdk-version"]),
        "compiler": command(["xcrun", "clang++", "--version"]),
        "local_input_sha256": {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in local_inputs},
        "craft_patch": json.loads((prefix / "etc/kwi-craft-patch.json").read_text()),
        "receipts": receipts, "archives": archives, "mach_o": libraries, "problems": problems,
    }
    # Local paths in toolchain output are evidence, not portable build requirements.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Recorded {len(receipts)} receipts, {len(archives)} input archives and {len(libraries)} Mach-O files")
    if problems:
        raise SystemExit("Dependency target audit failed: " + "; ".join(problems))


if __name__ == "__main__":
    main()
