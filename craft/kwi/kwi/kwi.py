# SPDX-FileCopyrightText: none
# SPDX-License-Identifier: CC0-1.0
import info
from CraftCore import CraftCore
from Package.CMakePackageBase import CMakePackageBase


class subinfo(info.infoclass):
    def setTargets(self):
        self.description = "Kwi terminal for Apple Silicon macOS"
        revision = "aadf139a6a40550b956e30ad501cc0064a597b77"
        self.svnTargets["0.1.0"] = f"https://invent.kde.org/utilities/kwi.git|master|{revision}"
        self.defaultTarget = "0.1.0"

    def setDependencies(self):
        self.buildDependencies["virtual/base"] = None
        self.buildDependencies["kde/frameworks/extra-cmake-modules"] = None
        for package in ["qtbase", "qtdeclarative", "qtshadertools", "qtsvg"]:
            self.runtimeDependencies[f"libs/qt6/{package}"] = None
        for package in ["tier1/kcoreaddons", "tier1/kconfig", "tier1/kdbusaddons", "tier1/kguiaddons", "tier1/ki18n", "tier1/kirigami", "tier1/kitemmodels", "tier1/kwindowsystem", "tier2/kpty", "tier3/knotifications", "tier3/kio", "tier3/qqc2-desktop-style"]:
            self.runtimeDependencies[f"kde/frameworks/{package}"] = None
        self.runtimeDependencies["kde/unreleased/kirigami-addons"] = None
        self.runtimeDependencies["kde/libs/kirigami-app-components"] = None
        self.runtimeDependencies["kwi/libghostty-vt"] = None
        self.runtimeDependencies["libs/icu"] = None
        self.runtimeDependencies["kde/frameworks/tier1/breeze-icons"] = None
        self.runtimeDependencies["kde/frameworks/tier3/kiconthemes"] = None


class Package(CMakePackageBase):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.subinfo.options.configure.args += ["-DBUILD_KPART=OFF", "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4",
                                               "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local",
                                               "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                                               f"-DPython3_EXECUTABLE={CraftCore.standardDirs.craftRoot() / 'bin/python3'}"]
