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

If you receive a warning stating *"Apple could not verify 'Kwi.app' is free of malware that may harm your Mac or compromise your privacy"*, macOS Gatekeeper has quarantined the app because it is not yet signed or notarized.

```bash
xattr -cr /Applications/Kwi.app
```

*Note: If your app is located in the Downloads folder or elsewhere, replace `/Applications/Kwi.app` with the correct path to the app.*


**⌘T** opens a tab, **⌘F** searches output, and **⌘,** opens settings.
Option-as-Alt is available under **Behavior**.

## Build

Use an Apple Silicon Mac with full Xcode, Git and Python 3.11 or later. Dependency
versions are pinned; Craft manages a project-local prefix. The root Makefile is
the entry point for local development and CI.

```sh
make build
make test
make run
```

Make uses the Xcode selected by `xcode-select`; set `DEVELOPER_DIR` to a full Xcode
`Contents/Developer` directory if Command Line Tools is selected. `make help`
lists all commands. Use `JOBS=2` to limit compiler parallelism and `ARGS='…'` to
pass options to test or measurement tools. `make test-gui` needs a focused,
logged-in Mac session; performance and app campaigns are explicit targets.

`make ci` runs checks, builds, headless tests and verified release packaging.
`make package` produces the app and matching sources; commit local changes first
so the source archive matches the build. Python helpers in `scripts/macos/`
implement deployment and verification behind the Makefile.

## Development

This is an independent macOS port. KDE's original Git history is retained, and
[upstream tracking](https://github.com/MohamedElashri/kwi-mac/actions/workflows/upstream.yml)
reports new changes weekly through `make upstream`. Fetch KDE's `master` through
an `upstream` remote, merge updates on a review branch, and build/test before
merging into `main`.

The [CI workflow](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml)
uses `make check` and `make ci` on pushes and pull requests. Version tags also run
`make release` to verify assets and prepare a draft for publication. Locally,
check out the tag, run `make ci`, then `make release TAG=<tag>`.
Downloads include checksums, dependency metadata and source information.

## License

The application is GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL;
the terminal libraries are LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL. Konsole palettes are GPL-2.0-or-later. See
[license texts](LICENSES/) and [palette attribution](libs/kwi-terminal/data/color-schemes/NOTICE).
