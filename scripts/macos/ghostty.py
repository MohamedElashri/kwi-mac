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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "zig", "prefix", "cache"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", default="4")
    args = parser.parse_args()
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
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(args.cache / "zig-global-cache"))
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], env=env, text=True).strip()
    # Craft supplies GNU libtool for autotools; Ghostty's Darwin archive step
    # explicitly requires Apple's distinct libtool -static interface.
    native_libtool = subprocess.check_output(["xcrun", "--find", "libtool"], env=env, text=True).strip()
    env["PATH"] = str(Path(native_libtool).parent) + os.pathsep + env["PATH"]
    env["SDKROOT"] = sdk  # Ghostty's apple-sdk helper obtains absolute paths through xcrun.
    command = [str(args.zig), "build", "--system", str(args.source / "zig-packages"),
               "-fno-sys=simdutf", "-fno-sys=highway", "-Demit-lib-vt", "-Demit-xcframework=false",
               "-Demit-macos-app=false", "-Demit-themes=false", "-Demit-terminfo=false", "-Dapp-runtime=none",
               "-Di18n=false", "-Doptimize=fast", "-Dcpu=baseline", "-Dtarget=aarch64-macos.14.4",
               "-Dversion-string=1.4.0", "-Dlib-version-string=0.1.0-dev+83fb95624b0439ee660c2679b43b38c8b63769cd",
               "--prefix", str(args.prefix), "-j" + args.jobs]
    print("Building pinned Ghostty with native arm64 Zig", flush=True)
    subprocess.run(command, cwd=args.source, env=env, check=True, timeout=3600)
    licenses = args.prefix / "share/licenses/libghostty-vt"
    licenses.mkdir(parents=True, exist_ok=True)
    for directory, name in [(args.source, "ghostty"), (args.zig.parent, "zig"),
                            (args.source / "simdutf-licenses", "simdutf")]:
        target = licenses / name
        target.mkdir(exist_ok=True)
        for path in directory.glob("LICENSE*"):
            if path.is_file():
                shutil.copyfile(path, target / path.name)
    for package in (args.source / "zig-packages").iterdir():
        for folder in (package, package / "licenses"):
            for path in folder.glob("LICENSE*"):
                if path.is_file():
                    target = licenses / package.name / folder.relative_to(package)
                    target.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target / path.name)
    (args.prefix / "share/kwi").mkdir(parents=True, exist_ok=True)
    (args.prefix / "share/kwi/ghostty-build-inputs.json").write_text(json.dumps({"sources": manifest, "command": command, "sdk": sdk}, indent=2) + "\n")


if __name__ == "__main__":
    main()
