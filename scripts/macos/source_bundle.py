#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Assemble pinned dependency sources and notices for an application package."""
import concurrent.futures
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sqlite3
import subprocess
import tarfile
import zipfile

import bundle
from ghostty import fetch

ROOT = Path(__file__).resolve().parents[2]


def runtime_components(database, binaries):
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as db:
        rows = db.execute("SELECT p.packagePath,p.version,f.filename FROM fileList f JOIN packageList p ON p.packageId=f.packageId").fetchall()
    by_name = {}
    for package, version, filename in rows:
        if package.startswith(("libs/", "kde/", "kwi/")) and package != "kwi/kwi":
            by_name.setdefault(Path(filename).name, {})[package] = version
    required = {}
    for binary in binaries:
        name = Path(binary).name
        if name == "zirq" or name.startswith("libkwi-terminal"):
            continue
        if name not in by_name:
            raise RuntimeError(f"No source package owns bundled binary {name}")
        required.update(by_name[name])
    return required


def prune_translations(database, app, packages):
    """Omit translations installed for build tools and undeployed libraries."""
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as db:
        rows = db.execute("SELECT p.packagePath,f.filename FROM fileList f JOIN packageList p ON p.packageId=f.packageId WHERE f.filename LIKE 'share/locale/%'").fetchall()
    owners = {}
    for package, filename in rows:
        owners.setdefault(filename, set()).add(package)
    shipped = set(packages) | {"kwi/kwi"}
    locale = app / "Contents/Resources/locale"
    for path in locale.rglob("*"):
        if path.is_file():
            relative = "share/locale/" + path.relative_to(locale).as_posix()
            if relative not in owners:
                raise RuntimeError(f"No source package owns translation {relative}")
            if not owners[relative] & shipped:
                path.unlink()


def notice_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise RuntimeError(f"Unsafe archive path: {name}")
    return path


def write_notices(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    count = 0

    def consider(name, size, read):
        nonlocal count
        path = notice_path(name)
        legal = bool(re.match(r"(?i)^(copying|copyright|notice|licen[cs]e|authors)([._-].*)?$", path.name)) or "LICENSES" in path.parts
        readme = path.name.lower().startswith("readme")
        if not legal and not readme:
            return
        if size > 8 * 1024 * 1024:
            raise RuntimeError(f"Oversized notice: {name}")
        data = read()
        if readme and not legal and not re.search(rb"(?i)copyright|licen[cs]e", data):
            return
        target = destination / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        count += 1

    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                if not item.is_dir() and (item.external_attr >> 16) & 0o170000 != 0o120000:
                    consider(item.filename, item.file_size, lambda item=item: source.read(item))
    elif tarfile.is_tarfile(archive):
        with tarfile.open(archive) as source:
            for item in source:
                if item.isfile():
                    consider(item.name, item.size, lambda item=item: source.extractfile(item).read())
    elif "LICENSE" in archive.name.upper():
        consider(re.sub(r"^[a-f0-9]{64}-", "", archive.name), archive.stat().st_size, archive.read_bytes)
    return count


def fetch_sources(components):
    sources = {source["sha256"]: source for entry in components for source in entry["sources"]}
    cache = ROOT / "build-macos/release-source-cache"
    cache.mkdir(parents=True, exist_ok=True)
    def verified(source):
        path = fetch(source, cache)
        print(f"Verified source: {source['url']}", flush=True)
        return path
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        paths = dict(zip(sources, pool.map(verified, sources.values())))
    print(f"Verified {len(paths)} source archives", flush=True)
    return paths


def prepare(prefix, app, output):
    if subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=normal"], text=True).strip():
        raise RuntimeError("Commit source changes before building a distributable source archive")
    catalog = json.loads((ROOT / "craft/release-sources.json").read_text())
    installed = runtime_components(prefix / "etc/blueprints/install.db", bundle.macho_files(app))
    components = {entry["package"]: entry for entry in catalog["components"]}
    for package, version in installed.items():
        if package not in components or components[package]["version"] != version:
            raise RuntimeError(f"Source catalog must be updated for {package} {version}")
    bootstrap = json.loads((prefix / "etc/kwi-bootstrap.json").read_text())
    if bootstrap != {"craft": catalog["craft_commit"], "blueprints": catalog["blueprints_commit"]}:
        raise RuntimeError("Build recipe revisions differ from the source catalog")
    selected = [components[name] for name in sorted(installed)]
    prune_translations(prefix / "etc/blueprints/install.db", app, installed)
    paths = fetch_sources(selected)
    notices = app / "Contents/Resources/licenses/dependencies"
    if notices.exists():
        shutil.rmtree(notices)
    entries = []
    for component in selected:
        notice_count = 0
        slug = component["package"].replace("/", "_")
        for index, source in enumerate(component["sources"]):
            notice_count += write_notices(paths[source["sha256"]], notices / slug / str(index))
        entries.append({**component, "sources": [{**source, "archive": "dependencies/" + paths[source["sha256"]].name}
                                                for source in component["sources"]], "notice_files": notice_count})
    commit = bundle.run(["git", "-C", ROOT, "rev-parse", "HEAD"]).strip()
    project_source = output / ".application-source.tar.gz"
    subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar.gz", "--prefix=zirq/", "--output", str(project_source), commit], check=True)
    recipe_archives = []
    for name, checkout, revision in [
        ("craft", prefix / "craft", catalog["craft_commit"]),
        ("blueprints", prefix / "etc/blueprints/locations/craft-blueprints-kde", catalog["blueprints_commit"]),
    ]:
        path = output / f".{name}-source.tar.gz"
        subprocess.run(["git", "-C", str(checkout), "archive", "--format=tar.gz", f"--prefix={name}/", "--output", str(path), revision], check=True)
        recipe_archives.append((name, path))
    manifest = {"project_commit": commit, "project_source": "application/Zirq-source.tar.gz",
                "build_recipes": {"craft": catalog["craft_commit"], "blueprints": catalog["blueprints_commit"]},
                "components": entries, "source_archive": "Zirq-sources.tar.gz"}
    manifest_file = output / "SOURCES.json"
    manifest_file.write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copy2(manifest_file, notices / "SOURCES.json")
    source_archive = output / "Zirq-sources.tar.gz"
    with tarfile.open(source_archive, "w:gz", compresslevel=1) as archive:
        archive.add(project_source, arcname="application/Zirq-source.tar.gz")
        archive.add(manifest_file, arcname="SOURCES.json")
        for name, path in recipe_archives:
            archive.add(path, arcname=f"build-recipes/{name}-source.tar.gz")
        for digest, path in paths.items():
            archive.add(path, arcname="dependencies/" + path.name)
    for path in [project_source, *(path for _, path in recipe_archives)]:
        path.unlink()
    with source_archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"archive": source_archive.name, "sha256": digest,
            "manifest": manifest_file.name, "components": len(entries), "notice_files": sum(e["notice_files"] for e in entries)}


if __name__ == "__main__":
    # Fetch before compilation so unavailable or changed inputs fail the build early.
    fetch_sources(json.loads((ROOT / "craft/release-sources.json").read_text())["components"])
