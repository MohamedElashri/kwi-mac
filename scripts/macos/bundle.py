#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Deploy the staged native app, close its Mach-O dependencies and ad-hoc sign it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MAGIC = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def tool_environment():
    env = {k: os.environ[k] for k in ("HOME", "USER", "LOGNAME", "TMPDIR", "DEVELOPER_DIR") if k in os.environ}
    env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    return env


def run(args):
    return subprocess.run([str(a) for a in args], check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=tool_environment(), timeout=600).stdout


def macho_files(root):
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) in MAGIC:
                    yield path


def dependencies(path):
    return [line.strip().split(" (compatibility version", 1)[0] for line in run(["otool", "-L", path]).splitlines()[1:]]


def rpaths(path):
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", run(["otool", "-l", path]))


def resolve_dependency(app, binary, link, search_paths):
    """Resolve deployed dyld paths without falling back to the Craft prefix."""
    if system(link):
        return None
    def expand(value):
        for token, base in (("@loader_path", binary.parent), ("@executable_path", app / "Contents/MacOS")):
            if value == token or value.startswith(token + "/"):
                return base / value[len(token):].lstrip("/")
        return Path(value)
    candidates = ([expand(path) / link.removeprefix("@rpath/") for path in search_paths]
                  if link.startswith("@rpath/") else [expand(link)])
    for path in candidates:
        if path.is_absolute() and path.exists():
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(app.resolve()):
                raise RuntimeError(f"Dependency escapes the bundle: {link} in {binary}")
            return resolved
    raise RuntimeError(f"Unresolved deployed dependency {link} in {binary}")


def system(path):
    return path.startswith(("/System/Library/", "/usr/lib/"))


def copy_tree(source, destination):
    if source.exists():
        shutil.copytree(source, destination, symlinks=True, dirs_exist_ok=True, ignore=shutil.ignore_patterns("*.dSYM", "*.a"))


def normalize_library_aliases(app, prefix):
    """macdeployqt may copy a symlink's bytes under its alias; keep one image."""
    app = app.resolve()
    prefix = prefix.resolve()
    frameworks = app / "Contents/Frameworks"
    for binary in sorted(frameworks.glob("*.dylib")):
        installed = prefix / "lib" / binary.name
        if binary.is_symlink() or not installed.exists():
            continue
        source = installed.resolve()
        if not source.is_relative_to(prefix):
            raise RuntimeError(f"Prefix library alias escapes the prefix: {installed}")
        if ".framework" in source.as_posix():
            framework = next(parent for parent in source.parents if parent.suffix == ".framework")
            target = frameworks / framework.name / source.relative_to(framework)
            if not target.exists():
                copy_tree(framework, frameworks / framework.name)
        else:
            target = frameworks / source.name
        if binary == target:
            continue
        if not target.exists():
            binary.rename(target)
        else:
            binary.unlink()
        binary.symlink_to(os.path.relpath(target, binary.parent))


def close_dependencies(app, prefix):
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    normalize_library_aliases(app, prefix)
    done = set()
    while True:
        pending = [p for p in macho_files(app) if p not in done]
        if not pending:
            break
        for binary in pending:
            done.add(binary)
            for dependency in dependencies(binary):
                if system(dependency):
                    continue
                if dependency.startswith("@rpath/"):
                    suffix = dependency.removeprefix("@rpath/")
                    candidates = [frameworks / suffix, prefix / "lib" / suffix]
                elif dependency.startswith("@loader_path/"):
                    candidates = [binary.parent / dependency.removeprefix("@loader_path/")]
                elif dependency.startswith("@executable_path/"):
                    candidates = [app / "Contents/MacOS" / dependency.removeprefix("@executable_path/")]
                else:
                    candidates = [Path(dependency)]
                source = next((p.resolve() for p in candidates if p.exists()), None)
                if source is None:
                    raise RuntimeError(f"Unresolved dependency {dependency} in {binary}")
                if source.is_relative_to(app):
                    target = source
                else:
                    if not source.is_relative_to(prefix):
                        raise RuntimeError(f"Dependency outside the coherent prefix: {source}")
                    if ".framework" in source.as_posix():
                        framework = next(parent for parent in source.parents if parent.suffix == ".framework")
                        copy_tree(framework, frameworks / framework.name)
                        target = frameworks / framework.name / source.relative_to(framework)
                    else:
                        target = frameworks / source.name
                        if not target.exists():
                            shutil.copy2(source, target)  # Never mutate a prefix hardlink.
                relative = target.relative_to(frameworks).as_posix()
                replacement = "@rpath/" + relative
                if dependency != replacement:
                    run(["install_name_tool", "-change", dependency, replacement, binary])
            # Each plugin/framework resolves against the app's private framework directory.
            paths = rpaths(binary)
            for path in paths:
                if path.startswith("/") and not system(path):
                    run(["install_name_tool", "-delete_rpath", path, binary])
            relative = os.path.relpath(frameworks, binary.parent)
            local = "@loader_path/" + relative
            if local not in paths:
                run(["install_name_tool", "-add_rpath", local, binary])
            if binary.is_relative_to(frameworks):
                run(["install_name_tool", "-id", "@rpath/" + binary.relative_to(frameworks).as_posix(), binary])


def audit(app):
    result = {}
    for path in app.rglob("*"):
        if path.is_symlink() and not path.resolve(strict=True).is_relative_to(app):
            raise RuntimeError(f"Symlink escapes the bundle: {path}")
    for binary in macho_files(app):
        architectures = run(["lipo", "-archs", binary]).split()
        load = run(["otool", "-l", binary])
        minimum = re.findall(r"\bminos\s+([\d.]+)", load)
        links = dependencies(binary)
        for link in links:
            if link.startswith("/") and not system(link):
                raise RuntimeError(f"External load path {link} in {binary}")
        search_paths = rpaths(binary)
        for path in search_paths:
            if path.startswith("/") and not system(path):
                raise RuntimeError(f"External runtime search path {path} in {binary}")
        if "arm64" not in architectures or not minimum:
            raise RuntimeError(f"Invalid target architecture/minimum in {binary}")
        if any(tuple(map(int, v.split("."))) > (14, 4, 0) for v in minimum):
            raise RuntimeError(f"Deployment target exceeds 14.4: {binary}")
        resolved = {link: target.relative_to(app.resolve()).as_posix() for link in links
                    if (target := resolve_dependency(app, binary, link, search_paths)) is not None}
        if binary.name.startswith("test-") or "kwi-presentation-trace" in binary.name:
            raise RuntimeError(f"Test instrumentation in production bundle: {binary}")
        result[binary.relative_to(app).as_posix()] = {"architectures": architectures, "minimum_macos": minimum,
                                                   "dependencies": links, "resolved_dependencies": resolved}
    return result


def inventory_resources(app):
    required = ("Contents/MacOS/kwi", "Contents/Resources/kwi.icns", "Contents/Resources/qt.conf",
                "Contents/Resources/qml/org/kde/desktop/qmldir", "Contents/Resources/qml/org/kde/kwi/terminal/qmldir",
                "Contents/Resources/knotifications6/kwi.notifyrc", "Contents/PlugIns/platforms/libqcocoa.dylib",
                "Contents/PlugIns/styles/libqmacstyle.dylib", "Contents/PlugIns/iconengines/libqsvgicon.dylib")
    for relative in required:
        path = app / relative
        if not path.is_file() or not path.stat().st_size:
            raise RuntimeError(f"Missing runtime resource: {relative}")
    for relative in ("Contents/Resources/licenses", "Contents/Resources/kwi/color-schemes/stock", "Contents/Resources/kf6/searchproviders"):
        if not any(p.is_file() for p in (app / relative).rglob("*")):
            raise RuntimeError(f"Empty runtime resource directory: {relative}")
    return {"required": list(required), "licenses": {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                                    for p in sorted((app / "Contents/Resources/licenses").rglob("*")) if p.is_file()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=ROOT / "build-macos/craft-root")
    parser.add_argument("--output", type=Path, default=ROOT / "build-macos/stage/Kwi.app")
    parser.add_argument("--reuse-deployment", action="store_true", help="Local iteration only: reuse the previous Qt/QML closure, then replace/re-audit app code")
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    destination = args.output.absolute()
    if destination.is_symlink() or destination.suffix != ".app":
        parser.error("Output must be a non-symlink .app directory")
    source = prefix / "Applications/KDE/kwi.app"
    if not source.exists():
        parser.error("Install and merge kwi/kwi into the Craft prefix first")
    app = destination.with_name("." + destination.stem + ".staging.app")
    for path in (destination, app):
        marker = path / "Contents/Resources/kwi-bundle.json"
        if path.exists() and (path.is_symlink() or not marker.exists() or json.loads(marker.read_text()).get("owner") != "kwi-macos-bundle"):
            parser.error("Refusing to replace an app not owned by this bundler")
    if app.exists():
        shutil.rmtree(app)
    # Build beside the destination; a failed deployment preserves the previous app.
    app.parent.mkdir(parents=True, exist_ok=True)
    if args.reuse_deployment:
        if not destination.exists():
            parser.error("A previous deployed bundle is required for local reuse")
        copy_tree(destination, app)
    shutil.copytree(source, app, symlinks=True, dirs_exist_ok=True, ignore=shutil.ignore_patterns("*.dSYM"))
    resources = app / "Contents/Resources"
    resources.mkdir(exist_ok=True)
    marker = resources / "kwi-bundle.json"
    inputs = [ROOT / "CMakeLists.txt", *ROOT.glob("src/**/*"), *ROOT.glob("libs/**/*")]
    marker.write_text(json.dumps({"owner": "kwi-macos-bundle", "source_commit": run(["git", "-C", ROOT, "rev-parse", "HEAD"]).strip(),
                                 "source_inputs": {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                                   for p in inputs if p.is_file()}}, indent=2) + "\n")
    copy_tree(prefix / "qml/org/kde/kwi/terminal", resources / "qml/org/kde/kwi/terminal")
    # KirigamiAppDefaults selects this Controls style dynamically; it is not a
    # plain QML import in the application sources and the scanner misses it.
    copy_tree(prefix / "qml/org/kde/desktop", resources / "qml/org/kde/desktop")
    if args.reuse_deployment:
        # Refresh the shared implementation as well as the executable/QML plugin.
        for library in (prefix / "lib").glob("libkwi-terminal*.dylib"):
            if not library.is_symlink():
                shutil.copy2(library, app / "Contents/Frameworks" / library.name)
    for name in ("kwi", "knotifications6", "locale", "licenses"):
        copy_tree(prefix / "share" / name, resources / name)
    for notice in sorted((prefix / "share/icu").glob("*/LICENSE")):
        target = resources / "licenses/icu" / notice.parent.name / "LICENSE"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, target)
    copy_tree(prefix / "share/kf6/searchproviders", resources / "kf6/searchproviders")
    # Non-QML factory plugins are not found by the QML import scanner.
    for name in ("kf6/kirigami", "kf6/urifilters", "iconengines", "kiconthemes6"):
        copy_tree(prefix / "plugins" / name, app / "Contents/PlugIns" / name)
    command = [prefix / "bin/macdeployqt", app, "-always-overwrite", "-no-codesign", "-no-strip", "-verbose=1",
               "-qmldir=" + str(ROOT / "src/declarative"), "-qmldir=" + str(ROOT / "libs/kwi-terminal/src/declarative"),
               "-qmlimport=" + str(prefix / "qml"), "-libpath=" + str(prefix / "lib")]
    if not args.reuse_deployment:
        print("Deploying Qt/QML and plugins", flush=True)
        subprocess.run([str(p) for p in command], check=True, env=tool_environment(), timeout=600)
    else:
        print("Reusing the previous local Qt/QML deployment; replacing app code and auditing closure", flush=True)
    print("Closing native dependency paths", flush=True)
    close_dependencies(app, prefix)
    (resources / "qt.conf").write_text("[Paths]\nPlugins=PlugIns\nQmlImports=Resources/qml\n")
    info_path = app / "Contents/Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    info.update(CFBundleName="Kwi", CFBundleDisplayName="Kwi", CFBundleIdentifier="org.kde.kwi", LSMinimumSystemVersion="14.4")
    info_path.write_bytes(plistlib.dumps(info))
    resource_inventory = inventory_resources(app)
    binaries = audit(app)
    print(f"Ad-hoc signing {len(binaries)} native files", flush=True)
    for binary in macho_files(app):
        run(["codesign", "--force", "--sign", "-", binary])
    for framework in sorted(app.rglob("*.framework"), key=lambda p: len(p.parts), reverse=True):
        run(["codesign", "--force", "--sign", "-", framework])
    run(["codesign", "--force", "--sign", "-", app])
    run(["codesign", "--verify", "--deep", "--strict", app])
    manifest = {"signing": "ad-hoc; not Developer ID signed or notarized", "reused_deployment": args.reuse_deployment, "mach_o": binaries,
                "resources": resource_inventory,
                "files": {p.relative_to(app).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in app.rglob("*") if p.is_file() and not p.is_symlink()}}
    if destination.exists():
        shutil.rmtree(destination)
    app.replace(destination)
    destination.with_suffix(".bundle.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Bundle and dependency audit ready: {destination}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stdout or "", end="")
        raise SystemExit(error.returncode)
