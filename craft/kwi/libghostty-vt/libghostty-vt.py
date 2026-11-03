# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
from pathlib import Path
import subprocess
import sys

import info
import utils
from BuildSystem.BuildSystemBase import BuildSystemBase
from CraftCore import CraftCore
from Package.PackageBase import PackageBase
from Packager.TypePackager import TypePackager
from Source.MultiSource import MultiSource
from Utils import CraftHash


class subinfo(info.infoclass):
    def setTargets(self):
        self.description = "Pinned Ghostty VT C library for Kwi"
        self.targets["1.4.0"] = "https://codeload.github.com/ghostty-org/ghostty/tar.gz/refs/tags/v1.4.0"
        self.archiveNames["1.4.0"] = "ghostty-v1.4.0.tar.gz"
        self.targetInstSrc["1.4.0"] = "ghostty-1.4.0"
        self.targetDigests["1.4.0"] = (["2e2a2516f9bea89a4991ad7c9401b77682eed366387a5b1d7c629472b5d11a43"], CraftHash.HashAlgorithm.SHA256)
        self.defaultTarget = "1.4.0"

    def setDependencies(self):
        self.buildDependencies["virtual/base"] = None
        self.buildDependencies["dev-utils/zig"] = None


class Package(PackageBase, MultiSource, BuildSystemBase, TypePackager):
    def __init__(self, **kwargs):
        PackageBase.__init__(self, **kwargs)
        MultiSource.__init__(self, **kwargs)
        BuildSystemBase.__init__(self, **kwargs)
        TypePackager.__init__(self, **kwargs)
        self.subinfo.options.package.disableBinaryCache = True

    def make(self):
        root = Path(__file__).resolve().parents[3]
        prefix = CraftCore.standardDirs.craftRoot()
        command = [sys.executable, str(root / "scripts/macos/ghostty.py"),
                   "--source", str(self.sourceDir()), "--zig", str(prefix / "dev-utils/zig/zig"),
                   "--prefix", str(self.buildDir() / "zig-out"),
                   "--cache", str(prefix / "download/archives/kwi/libghostty-vt/packages"),
                   "--jobs", str(CraftCore.settings.get("Compile", "Jobs", "4"))]
        log = prefix / "logs/kwi/libghostty-vt/native-build.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=7200)
        if result.returncode:
            print(log.read_text(errors="replace")[-20000:], flush=True)
        return result.returncode == 0

    def install(self):
        if not BuildSystemBase.install(self):
            return False
        output = self.buildDir() / "zig-out"
        if not utils.copyDir(output, self.installDir(), linkOnly=False):
            return False
        for path in self.installDir().rglob("*"):
            if path.suffix in (".pc", ".cmake"):
                path.write_text(path.read_text().replace(str(output), str(self.subinfo.buildPrefix)))
        return True
