#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0
"""Verify a published Kwi → Zirq upgrade with real bundles and an isolated profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

from app_test_support import ensure_kwi_not_running, executable, process_path, wait_json
from package import verify_assets

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = "https://github.com/MohamedElashri/kwi-mac"


def run(command, **kwargs):
    return subprocess.run([str(v) for v in command], check=True, timeout=60, **kwargs)


def profile_files(home):
    paths = [home / "Library/Preferences" / name for name in ("kwirc", "kwiterminalrc", "State/kwistaterc")]
    paths += sorted((home / "Library/Application Support/kwi").rglob("*"))
    return {str(p.relative_to(home)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}


def launch(app, output, home, label):
    capture = output / (label + ".json")
    env = {k: os.environ[k] for k in ("USER", "LOGNAME") if k in os.environ}
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), PATH="/usr/bin:/bin:/usr/sbin:/sbin", LANG="en_US.UTF-8")
    binary = executable(app)
    with (output / (label + ".log")).open("w") as log:
        process = subprocess.Popen([str(binary), "--hold", "-e", "/usr/bin/python3", str(output / "capture.py"), str(capture)],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            data = wait_json(capture, process, 30)
            if not process_path(process.pid).samefile(binary) or data["parent"] != process.pid:
                raise RuntimeError("Profile launch did not reach the owned app")
            if data["preset"] != "carried العربية" or Path(data["cwd"]) != output / "preset directory":
                raise RuntimeError("The default legacy preset, environment or working directory was lost")
            return data
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, required=True, help="Published Kwi-arm64.zip and SHA256SUMS directory")
    parser.add_argument("--assets", type=Path, required=True, help="Verified Zirq release asset directory")
    parser.add_argument("--probe", type=Path, required=True, help="Native test-macos-compatibility executable")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    ensure_kwi_not_running()
    assets, previous, output = args.assets.resolve(), args.previous.resolve(), args.output.resolve()
    commit = verify_assets(assets)
    # Both archives are verified before any app is launched or replaced.
    expected = [line.split()[0] for line in (previous / "SHA256SUMS").read_text().splitlines()
                if line.split()[1:] == ["Kwi-arm64.zip"]]
    if expected != [hashlib.sha256((previous / "Kwi-arm64.zip").read_bytes()).hexdigest()]:
        raise RuntimeError("Published Kwi archive checksum mismatch")
    output.mkdir(parents=True, exist_ok=False)
    apps, home = output / "Applications with spaces", output / "home"
    apps.mkdir()
    run(["/usr/bin/ditto", "-x", "-k", previous / "Kwi-arm64.zip", apps])
    old = apps / "Kwi.app"
    info = plistlib.loads((old / "Contents/Info.plist").read_bytes())
    if (info["CFBundleIdentifier"], info["CFBundleExecutable"]) != ("org.kde.kwi", "kwi"):
        raise RuntimeError("Expected the published Kwi predecessor")
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", old])
    preferences = home / "Library/Preferences"
    (preferences / "State").mkdir(parents=True)
    (preferences / "kwiterminalrc").write_text(
        "[Appearance]\nUseSystemFont=false\nFontFamily=Menlo\nFontPointSize=11\nBackgroundOpacity=0.83\n"
        "ColorScheme=user:migration.colorscheme\n[Terminal]\nScrollbackLines=321\nBidirectionalText=false\n"
        "[Behavior]\nOptionAsAlt=true\n[Presets]\nDefaultPreset=Migration العربية\n")
    (preferences / "kwirc").write_text("[Shortcuts][org.kde.kwi.tabs][new_tab]\nShortcut=Ctrl+Shift+Y\n")
    (preferences / "State/kwistaterc").write_text("[MainWindow]\n1440x900 screen: Width=741\n1440x900 screen: Height=510\n")
    presets = home / "Library/Application Support/kwi/presets"
    presets.mkdir(parents=True)
    (output / "preset directory").mkdir()
    (presets / "migration.preset").write_text(
        "[General]\nFormat=KwiLaunchPreset\nVersion=1\nName=Migration العربية\n"
        "[Launch]\nDirectoryMode=Custom\nDirectory=" + str(output / "preset directory") + "\n"
        "Environment=ZIRQ_MIGRATION=carried العربية\n")
    palette = home / "Library/Application Support/kwi/color-schemes/user/migration.colorscheme"
    palette.parent.mkdir(parents=True)
    palette.write_text("[General]\nDescription=Migration colors\n[Foreground]\nColor=18,52,86\n[Background]\nColor=10,20,30\n")
    (output / "capture.py").write_text(
        "import json,os,sys\nfrom pathlib import Path\n"
        "Path(sys.argv[1]).write_text(json.dumps({'parent':os.getppid(),'cwd':os.getcwd(),"
        "'preset':os.getenv('ZIRQ_MIGRATION'),'terminal':os.getenv('TERM_PROGRAM')}))\n")
    before_launch = launch(old, output, home, "kwi-before")
    if before_launch["terminal"] != "kwi":
        raise RuntimeError("The old bundle did not identify as Kwi")
    before = profile_files(home)
    # Substitute only the download transport; installer validation, extraction,
    # process checks, replacement, and macOS signature checks remain real.
    tools = output / "download-transport"
    tools.mkdir()
    curl = tools / "curl"
    curl.write_text(f"#!{sys.executable}\n" +
        "import os,shutil,sys\nfrom pathlib import Path\n"
        "args=sys.argv[1:]; base='" + REPOSITORY + "'\n"
        "tag=os.environ['ZIRQ_UPGRADE_TAG']; assets=Path(os.environ['ZIRQ_UPGRADE_ASSETS'])\n"
        "url=args[-1]\n"
        "if '--write-out' in args and url == base+'/releases/latest':\n"
        " print(base+'/releases/tag/'+tag,end='')\n"
        "elif url in [base+'/releases/download/'+tag+'/'+name for name in ('SHA256SUMS','Zirq-arm64.zip')]:\n"
        " shutil.copyfile(assets/url.rsplit('/',1)[-1],args[args.index('--output')+1])\n"
        "else: raise SystemExit('Unexpected download URL: '+url)\n")
    curl.chmod(0o755)
    env = {**os.environ, "PATH": str(tools) + ":/usr/bin:/bin:/usr/sbin:/sbin",
           "ZIRQ_UPGRADE_TAG": args.tag, "ZIRQ_UPGRADE_ASSETS": str(assets)}
    result = run(["/bin/bash", ROOT / "install.sh", "--app-dir", apps], env=env, capture_output=True, text=True)
    (output / "installer.log").write_text(result.stdout + result.stderr)
    new = apps / "Zirq.app"
    if old.exists() or not new.is_dir() or profile_files(home) != before:
        raise RuntimeError("Upgrade failed to replace Kwi or modified the legacy profile")
    after_launch = launch(new, output, home, "zirq-after")
    if after_launch["terminal"] != "zirq":
        raise RuntimeError("The upgraded bundle did not identify as Zirq")
    probe_env = {**env, "HOME": str(home), "CFFIXED_USER_HOME": str(home)}
    run([args.probe.resolve(), "--profile-state", output / "profile.json"], env=probe_env, capture_output=True, text=True)
    profile = json.loads((output / "profile.json").read_text())
    expected = {"font": "Menlo", "opacity": .83, "history": 321, "option_as_alt": True,
                "scheme": "user:migration.colorscheme", "preset": "Migration العربية"}
    if any(profile.get(k) != value for k, value in expected.items()):
        raise RuntimeError("A legacy setting did not carry over: " + json.dumps(profile))
    migration = profile["migration"]
    if (migration["application"], migration["bidi"], migration["palette_foreground"], migration["new_tab_shortcut"]) != (
            "zirq", False, "#123456", "Ctrl+Shift+Y"):
        raise RuntimeError("Palette, bidi preference or custom shortcut did not carry over")
    if migration["window_state"].get("1440x900 screen: Width") != "741":
        raise RuntimeError("Legacy window state was lost")
    report = {"passed": True, "source_commit": commit, "previous_version": info["CFBundleShortVersionString"],
              "target_tag": args.tag, "old_name_removed": True, "profile_unchanged_by_installer": True,
              "profile_files": before, "profile": profile, "before": before_launch, "after": after_launch,
              "scope": "Real published Kwi bundle, real release ZIP and installer; isolated HOME and Applications. Only curl transport is substituted."}
    (output / "summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
