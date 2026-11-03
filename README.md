<!--
SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
SPDX-FileCopyrightText: 2026 Mohamed Elashri <mail@elashri.com>
SPDX-License-Identifier: CC0-1.0
-->
# <img src="logo.png" width="32" alt=""/> Kwi for macOS

[![CI](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml/badge.svg?branch=main)](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml)
[![Latest release](https://img.shields.io/github/v/release/MohamedElashri/kwi-mac)](https://github.com/MohamedElashri/kwi-mac/releases/latest)
[![Platform](https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white)](https://github.com/MohamedElashri/kwi-mac/releases/latest)
[![License](https://img.shields.io/badge/license-GPL%20%2F%20LGPL-blue)](LICENSES/)

An Apple Silicon port of [KDE Kwi](https://invent.kde.org/utilities/kwi), powered
by libghostty-vt, Qt Quick and KDE Frameworks, with Metal-accelerated rendering.

- Tabs, retained output, search and keyboard selection.
- Launch presets, color schemes and customizable shortcuts.
- Mac keyboard handling, with optional Option-as-Alt.

## Install

Download `Kwi-arm64.zip` from the [latest release](https://github.com/MohamedElashri/kwi-mac/releases/latest),
extract it, and move `Kwi.app` to Applications. See the release notes for system
requirements and signing information. Builds are currently ad-hoc signed and
are not notarized; macOS may require approval before opening them.

**⌘T** opens a tab, **⌘F** searches output, and **⌘,** opens settings.
Option-as-Alt is available under **Behavior**.

## Build

Use an Apple Silicon Mac with full Xcode, Git and Python 3.11 or later. Dependency
versions are pinned in the build scripts; Craft manages a project-local prefix.

```sh
export DEVELOPER_DIR="$(xcode-select -p)"
python3 scripts/macos/craft.py prepare
python3 scripts/macos/craft.py run --install-deps kwi/kwi
python3 scripts/macos/craft.py run --configure kwi/kwi
python3 scripts/macos/craft.py run --make --install --post-install --qmerge --post-qmerge kwi/kwi
python3 scripts/macos/package.py --output build-macos/package
open build-macos/package/Kwi.app
```

`DEVELOPER_DIR` must point to full Xcode, not Command Line Tools. Run tooling tests
with `python3 -m unittest discover -s tests/macos -v`. Native tests use CTest in
`build-macos/craft-root/build/kwi/kwi/work/build`; `-LE gui` selects headless checks.
For GUI checks, first run `python3 scripts/macos/workloads.py` and keep the test
windows focused in a logged-in Mac session.
Commit local changes before packaging so the source archive matches the build.

## Development

This is an independent macOS port. KDE's original Git history is retained, and
[upstream tracking](https://github.com/MohamedElashri/kwi-mac/actions/workflows/upstream.yml)
reports new changes weekly. Fetch KDE's `master` through an `upstream` remote,
merge updates on a review branch, and build/test before merging into `main`.

Version tags start the [release workflow](https://github.com/MohamedElashri/kwi-mac/actions/workflows/release.yml),
which tests and packages arm64 builds and prepares a draft for publication.
Downloads include checksums, dependency metadata and source information.

## License

The application is GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL;
the terminal libraries are LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL. Konsole palettes are GPL-2.0-or-later. See
[license texts](LICENSES/) and [palette attribution](libs/kwi-terminal/data/color-schemes/NOTICE).
