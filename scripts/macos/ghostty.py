#!/usr/bin/env python3
# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
"""Build pinned libghostty-vt sources for the Craft recipe; never install globally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]


def download(url, destination):
    command = ["curl", "--ipv4", "--http1.1", "--fail", "--location", "--silent", "--show-error", "--retry", "3",
               "--connect-timeout", "20", "--max-time", "180", "--output", str(destination)]
    if os.environ.get("SSL_CERT_FILE"):
        command += ["--cacert", os.environ["SSL_CERT_FILE"]]
    subprocess.run([*command, url], check=True, timeout=240)


def fetch(source, cache):
    filename = source["sha256"] + "-" + Path(urllib.parse.urlparse(source["url"]).path).name
    path = cache / filename
    candidate = path
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".partial")
        download(source["url"], partial)
        candidate = partial
    with candidate.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != source["sha256"]:
        raise RuntimeError(f"SHA256 mismatch for {source['url']}; inspect the invalid cached file {candidate}")
    if candidate != path:
        candidate.replace(path)
    return path


def apply_patches(source):
    patches = sorted((ROOT / "craft/kwi/libghostty-vt").glob("*.patch"))
    records = []
    for patch in patches:
        command = ["git", "apply", "--whitespace=error", str(patch)]
        check = subprocess.run([*command[:2], "--check", *command[2:]], cwd=source, capture_output=True)
        if check.returncode == 0:
            subprocess.run(command, cwd=source, check=True)
        else:
            subprocess.run([*command[:2], "--reverse", "--check", *command[2:]], cwd=source, check=True)
        records.append({"path": str(patch.relative_to(ROOT)), "sha256": hashlib.sha256(patch.read_bytes()).hexdigest()})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "zig", "prefix", "cache"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", default="4")
    args = parser.parse_args()
    # Zig mixes canonical cache paths with source-relative tool paths. Resolve
    # macOS aliases such as /tmp -> /private/tmp before it computes those paths.
    for name in ("source", "zig", "prefix", "cache"):
        setattr(args, name, getattr(args, name).resolve())
    args.cache.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / "craft/ghostty-sources.json").read_text())
    for source in manifest["sources"]:
        if source.get("dest", ".") in (".", "zig"):
            continue  # Main source and compiler are verified/installed by Craft.
        path = fetch(source, args.cache)
        destination = args.source / source.get("dest", ".")
        destination.mkdir(parents=True, exist_ok=True)
        marker = destination / (".kwi-sha256-" + source["sha256"])
        if marker.exists():
            continue
        if source["type"] == "archive":
            subprocess.run(["tar", "-xf", str(path), "--strip-components=1", "-C", str(destination)], check=True, timeout=120)
        else:
            shutil.copyfile(path, destination / Path(urllib.parse.urlparse(source["url"]).path).name)
        marker.touch()
    patches = apply_patches(args.source)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(args.cache / "zig-global-cache"))
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], env=env, text=True).strip()
    # Craft supplies GNU libtool for autotools; Ghostty's Darwin archive step
    # explicitly requires Apple's distinct libtool -static interface.
    native_libtool = subprocess.check_output(["xcrun", "--find", "libtool"], env=env, text=True).strip()
    env["PATH"] = str(Path(native_libtool).parent) + os.pathsep + env["PATH"]
    env["SDKROOT"] = sdk  # Ghostty's apple-sdk helper obtains absolute paths through xcrun.
    command = [str(args.zig), "build", "--system", str(args.source / "zig-packages"),
               "-Demit-xcframework=false", "-Doptimize=ReleaseFast", "-Dcpu=baseline",
               "-Dtarget=aarch64-macos.14.4",
               "--prefix", str(args.prefix), "-j" + args.jobs]
    print("Building pinned Ghostty with native arm64 Zig", flush=True)
    subprocess.run(command, cwd=args.source, env=env, check=True, timeout=3600)
    licenses = args.prefix / "share/licenses/libghostty-vt"
    licenses.mkdir(parents=True, exist_ok=True)
    for directory, name in [(args.source, "ghostty"), (args.zig.parent, "zig"),
                            (args.source / "pkg/simdutf/vendor", "simdutf"),
                            (args.source / "uucode-licenses", "uucode")]:
        target = licenses / name
        target.mkdir(exist_ok=True)
        for path in directory.glob("LICENSE*"):
            if path.is_file():
                shutil.copyfile(path, target / path.name)
    shutil.copyfile(args.zig.parent / "lib/libc/musl/COPYRIGHT", licenses / "zig/musl-COPYRIGHT")
    shutil.copyfile(args.source / "deps/libghostty-vt-third-party.json", licenses / "libghostty-vt-third-party.json")
    simdutf = (args.source / "pkg/simdutf/vendor/simdutf.h").read_text()
    notice_start = simdutf.index("/* From\n")
    notice_end = simdutf.index("*/", notice_start) + 2
    (licenses / "simdutf/ISA-DETECTION-NOTICE").write_text(simdutf[notice_start:notice_end] + "\n")
    for package in (args.source / "zig-packages").iterdir():
        for folder in (package, package / "licenses"):
            for path in folder.glob("LICENSE*"):
                if path.is_file():
                    target = licenses / package.name / folder.relative_to(package)
                    target.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target / path.name)
    (args.prefix / "share/kwi").mkdir(parents=True, exist_ok=True)
    (args.prefix / "share/kwi/ghostty-build-inputs.json").write_text(json.dumps({"sources": manifest, "patches": patches, "command": command, "sdk": sdk}, indent=2) + "\n")


if __name__ == "__main__":
    main()
