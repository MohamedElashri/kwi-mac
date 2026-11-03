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

An independent Apple Silicon port of [KDE Kwi](https://invent.kde.org/utilities/kwi),
with Metal-accelerated rendering powered by libghostty-vt, Qt Quick and KDE Frameworks.

![Kwi on macOS with three tabs, truecolor output and Unicode text](assets/readme/screenshot.png)

- **A tabbed workspace:** scrollback, search and keyboard selection.
- **Your setup:** launch presets, color schemes, fonts and configurable shortcuts.
- **Mac input:** familiar keyboard shortcuts and optional Option-as-Alt.

## Install

Download **Kwi-arm64.zip** from the [latest release](https://github.com/MohamedElashri/kwi-mac/releases/latest),
extract it, and move **Kwi.app** to Applications. System requirements are listed in
its release notes. Builds are ad-hoc signed and not notarized.

If macOS blocks the first launch, follow [Apple’s Open Anyway instructions](https://support.apple.com/en-us/102445).
For a trusted download, you can also remove its quarantine attribute in Terminal:

```sh
xattr -dr com.apple.quarantine /Applications/Kwi.app
```

Adjust the path if you installed the app elsewhere.

**⌘T** opens a tab, **⌘F** searches output, and **⌘,** opens settings.
Enable **Use Option as Alt** under **Behavior** when needed.

## Build and test

Requires an Apple Silicon Mac, full Xcode, Git and Python 3.11 or later.
Dependencies are pinned and installed into a project-local Craft prefix.
The root [Makefile](Makefile) is the shared entry point for development and CI.

```sh
make build
make run
```

Make uses the Xcode selected by `xcode-select`. If Command Line Tools is selected,
set `DEVELOPER_DIR` to your full Xcode `Contents/Developer` directory.
Use `JOBS=2` to limit compiler parallelism.

| Command | Purpose |
| --- | --- |
| `make test` | Tooling and headless native tests |
| `make test-gui` | GUI checks in a focused Mac session |
| `make package` | Verified app archive and matching sources |
| `make ci` | Full build, tests and release packaging |
| `make screenshot` | Recreate the isolated demo screenshot |
| `make help` | All targets, measurements and configuration options |

Commit changes before packaging so the source archive matches the build.
Python helpers in `scripts/macos/` implement deployment and verification behind Make.

## CI and releases

[CI](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml)
runs `make check` on every push and pull request. Source, build-system and workflow
changes also run `make ci` on a hosted Apple Silicon Mac. Documentation-only changes
skip the Mac build; manual runs and version tags always run the full pipeline.

Version tags must match the CMake project version. CI verifies the downloads and
uses `make release` to prepare a draft for review. To prepare one locally, check out
the tag, run `make ci`, then `make release TAG=<tag>`. Downloads include checksums,
dependency metadata, license notices and matching application/dependency sources.

## Following KDE

The original Git history is retained. `make upstream` fetches KDE’s `master` and
reports unmerged commits; the [upstream workflow](https://github.com/MohamedElashri/kwi-mac/actions/workflows/upstream.yml)
runs it weekly. Review and merge updates on a separate branch, then build and test
before merging into `main`.

## License

The application is GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL;
the terminal libraries are LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL. Konsole palettes are GPL-2.0-or-later. See
[license texts](LICENSES/) and [palette attribution](libs/kwi-terminal/data/color-schemes/NOTICE).
