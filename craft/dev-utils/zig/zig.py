# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
import info
from CraftCore import CraftCore
from Package.BinaryPackageBase import BinaryPackageBase
from Utils import CraftHash


class subinfo(info.infoclass):
    def registerOptions(self):
        self.parent.package.categoryInfo.platforms = CraftCore.compiler.Platforms.MacOS
        self.parent.package.categoryInfo.architecture = CraftCore.compiler.Architecture.arm64

    def setTargets(self):
        self.description = "Pinned native arm64 Zig compiler for libghostty-vt"
        self.targets["0.17.0"] = "https://ziglang.org/download/0.17.0/zig-aarch64-macos-0.17.0.tar.xz"
        self.targetDigests["0.17.0"] = (["b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a"], CraftHash.HashAlgorithm.SHA256)
        self.targetInstSrc["0.17.0"] = "zig-aarch64-macos-0.17.0"
        self.targetInstallPath["0.17.0"] = "dev-utils/zig"
        self.defaultTarget = "0.17.0"

    def setDependencies(self):
        self.buildDependencies["virtual/base"] = None


class Package(BinaryPackageBase):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.subinfo.options.package.disableBinaryCache = True
