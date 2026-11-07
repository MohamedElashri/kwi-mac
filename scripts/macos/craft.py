#!/usr/bin/env python3
# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
"""Prepare the pinned arm64 Craft prefix, then run Craft in an explicit environment."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CRAFT = "89f72404fd7ff87d22efad2cefe8eda9e0fb85d5"
BLUEPRINTS = "bb83ed202e3ab1488fca639a8d7afc76b20ceeee"
PYTHON = str(Path(getattr(sys, "_base_executable", sys.executable)).resolve())


def run(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], check=True, timeout=21600, **kwargs)


def checkout(url, revision, destination, source=None):
    if not destination.exists():
        if source:
            run(["git", "clone", "--no-hardlinks", source, destination])
        else:
            run(["git", "init", destination])
            run(["git", "-C", destination, "remote", "add", "origin", url])
    result = subprocess.run(["git", "-C", str(destination), "rev-parse", "HEAD"], capture_output=True, text=True, timeout=30)
    current = result.stdout.strip() if result.returncode == 0 else None
    if current != revision:
        if subprocess.check_output(["git", "-C", str(destination), "status", "--porcelain"], text=True):
            raise RuntimeError(f"Refusing to change dirty checkout: {destination}")
        run(["git", "-C", destination, "fetch", "--depth", "1", "origin", revision])
        run(["git", "-C", destination, "checkout", "--detach", revision])


def prepare(prefix, craft_source=None, blueprints_source=None, jobs=4):
    if jobs < 1:
        raise RuntimeError("Build jobs must be positive")
    marker = prefix / "etc/kwi-bootstrap.json"
    if prefix.exists() and any(prefix.iterdir()) and not marker.exists():
        raise RuntimeError(f"Refusing to reuse a prefix not owned by this bootstrap: {prefix}")
    (prefix / "etc").mkdir(parents=True, exist_ok=True)
    # Mark ownership before network operations so an interrupted bootstrap can resume.
    if not marker.exists():
        marker.write_text(json.dumps({"craft": CRAFT, "blueprints": BLUEPRINTS}, indent=2) + "\n")
    elif json.loads(marker.read_text()) != {"craft": CRAFT, "blueprints": BLUEPRINTS}:
        raise RuntimeError("Prefix pins differ; use a new prefix")
    core = prefix / "craft"
    bp = prefix / "etc/blueprints/locations/craft-blueprints-kde"
    checkout("https://invent.kde.org/packaging/craft.git", CRAFT, core, craft_source)
    checkout("https://invent.kde.org/packaging/craft-blueprints-kde.git", BLUEPRINTS, bp, blueprints_source)
    # The pinned upstream helper starts a session daemon merely on environment setup.
    # Keep that optional side effect off in this build prefix; never touch user services.
    helper = core / "bin/CraftSetupHelper.py"
    original = "        if dbusInstalled:\n"
    patched = '        if dbusInstalled and not os.environ.get("KWI_CRAFT_NO_DBUS_AUTOSTART"):\n'
    text = helper.read_text()
    pristine = subprocess.check_output(["git", "-C", str(core), "show", f"{CRAFT}:bin/CraftSetupHelper.py"], text=True)
    if pristine.count(original) != 1:
        raise RuntimeError("Craft D-Bus patch context changed")
    expected = pristine.replace(original, patched, 1)
    if text not in (pristine, expected):
        raise RuntimeError("Refusing to overwrite other Craft helper changes")
    changed = subprocess.check_output(["git", "-C", str(core), "diff", "--name-only"], text=True).splitlines()
    if set(changed) - {"bin/CraftSetupHelper.py"}:
        raise RuntimeError("Craft checkout has other changes; inspect before reusing it")
    if text != expected:
        helper.write_text(expected)
    dbus = bp / "libs/dbus/dbus.py"
    dbus_pristine = subprocess.check_output(["git", "-C", str(bp), "show", f"{BLUEPRINTS}:libs/dbus/dbus.py"], text=True)
    dbus_original = '        if CraftCore.compiler.platform.isMacOS:\n            utils.system(["launchctl", "load",'
    dbus_replacement = '        if CraftCore.compiler.platform.isMacOS and not os.environ.get("KWI_CRAFT_NO_DBUS_AUTOSTART"):\n            utils.system(["launchctl", "load",'
    if dbus_pristine.count(dbus_original) != 1:
        raise RuntimeError("Craft D-Bus recipe patch context changed")
    dbus_expected = dbus_pristine.replace(dbus_original, dbus_replacement, 1)
    if dbus.read_text() not in (dbus_pristine, dbus_expected):
        raise RuntimeError("Refusing to overwrite other D-Bus recipe changes")
    if dbus.read_text() != dbus_expected:
        dbus.write_text(dbus_expected)
    bp_changed = subprocess.check_output(["git", "-C", str(bp), "diff", "--name-only"], text=True).splitlines()
    if set(bp_changed) - {"libs/dbus/dbus.py"}:
        raise RuntimeError("Blueprint checkout has other changes; inspect before reusing it")
    config = prefix / "etc/CraftSettings.ini"
    if not config.exists():
        settings = configparser.ConfigParser(interpolation=None)
        settings.optionxform = str
        settings.read(core / "CraftSettings.ini.template")
        for section, values in {
            "General": {"ABI": "macos-clang-arm64", "MacDeploymentTarget": "14.4", "KFHostToolingVersion": "6", "Notify": "None", "AllowAnsiColor": "False"},
            "Paths": {"Python": str(Path(PYTHON).parent)},
            "Compile": {"BuildType": "RelWithDebInfo", "UseNinja": "True", "MakeProgram": "ninja", "Jobs": "4"},
            "Blueprints": {"Settings": str(prefix / "etc/CraftOptions.ini"), "Locations": str(ROOT / "craft")},
            "BlueprintVersions": {"EnableDailyUpdates": "False"},
            "CraftDebug": {"LogEnvironment": "False", "LogDir": str(prefix / "logs")},
        }.items():
            if not settings.has_section(section):
                settings.add_section(section)
            settings[section].update(values)
        with config.open("w") as stream:
            settings.write(stream)
    settings = configparser.ConfigParser(interpolation=None)
    settings.optionxform = str
    settings.read(config)
    if settings["Compile"]["Jobs"] != str(jobs):
        settings["Compile"]["Jobs"] = str(jobs)
        with config.open("w") as stream:
            settings.write(stream)
    if not settings.has_option("CraftDebug", "LogDir"):
        settings["CraftDebug"]["LogDir"] = str(prefix / "logs")
        with config.open("w") as stream:
            settings.write(stream)
    if settings["Paths"]["Python"] != str(Path(PYTHON).parent):
        settings["Paths"]["Python"] = str(Path(PYTHON).parent)
        with config.open("w") as stream:
            settings.write(stream)
    options = prefix / "etc/CraftOptions.ini"
    if not options.exists():
        options.write_text(f"""[craft/craft-core]
revision = {CRAFT}
dailyUpdate = False
[craft/craft-blueprints-kde]
revision = {BLUEPRINTS}
dailyUpdate = False
[libs/python]
ignored = False
[libs/qt6]
version = 6.12.0
buildTests = False
[kde/frameworks]
version = 6.31.0
buildTests = False
[kde/unreleased/kirigami-addons]
version = 1.15.0
[kde/libs/kirigami-app-components]
version = 1.1.1
[kwi/kwi]
srcDir = {ROOT}
""")
    patches = [
        {"file": "bin/CraftSetupHelper.py", "upstream_commit": CRAFT, "sha256": hashlib.sha256(helper.read_bytes()).hexdigest()},
        {"file": "libs/dbus/dbus.py", "upstream_commit": BLUEPRINTS, "sha256": hashlib.sha256(dbus.read_bytes()).hexdigest()},
    ]
    (prefix / "etc/kwi-craft-patch.json").write_text(json.dumps({"patches": patches, "purpose": "Optional D-Bus LaunchAgent autostart disabled by KWI_CRAFT_NO_DBUS_AUTOSTART"}, indent=2) + "\n")


def refresh_vt_patches(prefix, env):
    record = prefix / "share/kwi/ghostty-build-inputs.json"
    if not record.exists():
        return
    expected = [{"path": str(path.relative_to(ROOT)), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for path in sorted((ROOT / "craft/kwi/libghostty-vt").glob("*.patch"))]
    installed = json.loads(record.read_text())
    sources = json.loads((ROOT / "craft/ghostty-sources.json").read_text())
    if installed.get("patches", []) == expected and installed.get("sources") == sources:
        return
    # Re-extract the verified archive so an older local patch is never stacked
    # underneath its replacement. Compiler caches remain reusable.
    run([PYTHON, prefix / "craft/bin/craft.py", "--ci-mode", "--unpack", "--make", "--install",
         "--post-install", "--qmerge", "--post-qmerge", "kwi/libghostty-vt"], env=env, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=ROOT / "build-macos/craft-root")
    parser.add_argument("--developer-dir", default=os.environ.get("DEVELOPER_DIR"))
    parser.add_argument("--craft-source", help="Optional local clone at the pinned revision")
    parser.add_argument("--blueprints-source", help="Optional local blueprint clone")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("action", choices=["prepare", "run"])
    parser.add_argument("args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("This prefix requires native Apple Silicon macOS")
    prefix = args.prefix.resolve()
    if args.action == "prepare":
        prepare(prefix, args.craft_source, args.blueprints_source, args.jobs)
        return 0
    if not args.developer_dir or not (Path(args.developer_dir) / "usr/bin/xcodebuild").exists():
        parser.error("Set --developer-dir or DEVELOPER_DIR to a full Xcode Contents/Developer directory")
    if not (prefix / "etc/kwi-bootstrap.json").exists():
        parser.error("Run prepare first")
    env = {k: os.environ[k] for k in ("HOME", "USER", "LOGNAME", "TMPDIR", "SSH_AUTH_SOCK", "SSL_CERT_FILE", "SSL_CERT_DIR") if k in os.environ}
    env.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", DEVELOPER_DIR=args.developer_dir,
               MACOSX_DEPLOYMENT_TARGET="14.4", KWI_CRAFT_NO_DBUS_AUTOSTART="1",
               CRAFT_PYTHON_BIN=PYTHON, PYTHONUNBUFFERED="1")
    command = args.args[1:] if args.args[:1] == ["--"] else args.args
    if not command:
        parser.error("Provide Craft arguments after run")
    if "--configure" in command and "kwi/kwi" in command:
        refresh_vt_patches(prefix, env)
    run([PYTHON, prefix / "craft/bin/craft.py", "--ci-mode", *command], env=env, cwd=ROOT)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
