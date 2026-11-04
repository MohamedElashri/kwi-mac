<!--
SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
SPDX-License-Identifier: CC0-1.0
-->
# <img src="logo.png" width="32" alt=""/> Kwi Mac

[![CI](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml/badge.svg?branch=main)](https://github.com/MohamedElashri/kwi-mac/actions/workflows/checks.yml)
[![Latest release](https://img.shields.io/github/v/release/MohamedElashri/kwi-mac)](https://github.com/MohamedElashri/kwi-mac/releases/latest)
[![Platform](https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white)](https://github.com/MohamedElashri/kwi-mac/releases/latest)
[![License](https://img.shields.io/badge/license-GPL%20%2F%20LGPL-blue)](LICENSES/)

A Metal-accelerated terminal for Apple Silicon. An independently maintained
macOS fork of [KDE Kwi](https://invent.kde.org/utilities/kwi), with bidirectional
Arabic, Persian and Hebrew support. Built with Qt Quick, KDE Frameworks and libghostty-vt.

![Kwi with tabbed sessions, truecolor output and Unicode text](assets/readme/screenshot.png)

- **Tabbed sessions** with searchable scrollback and keyboard selection.
- **Arabic, Persian and Hebrew** with bidirectional rendering, joining and mixed-script copy and search.
- **Mac input** with familiar shortcuts and optional Option-as-Alt.
- **Your workspace** with launch presets, color schemes, fonts and configurable shortcuts.

## Install

An Apple Silicon Mac is required; see the release notes for the minimum macOS version.
Install or update the latest stable release:

```sh
curl -fsSL https://raw.githubusercontent.com/MohamedElashri/kwi-mac/main/install.sh | bash
```

Run the same command again to update. The installer verifies checksums and
signatures and installs to `/Applications` or `~/Applications` without sudo.
Quit Kwi and use another terminal when updating. For a custom destination, replace
`bash` with `bash -s -- --app-dir "$HOME/Applications"`.

**Homebrew** uses this repository as its tap:

```sh
brew tap MohamedElashri/kwi-mac https://github.com/MohamedElashri/kwi-mac.git
brew install --cask mohamedelashri/kwi-mac/kwi-mac
```

Update a Homebrew installation with:

```sh
brew update
brew upgrade --cask mohamedelashri/kwi-mac/kwi-mac
```

For manual installation, download **Kwi-arm64.zip** from the
[latest release](https://github.com/MohamedElashri/kwi-mac/releases/latest),
extract it, and move **Kwi.app** to Applications.

Builds are ad-hoc signed and not notarized. If macOS blocks the first launch,
follow [Apple’s Open Anyway instructions](https://support.apple.com/en-us/102445).

## Use

**⌘T** opens a tab, **⌘F** searches output, and **⌘,** opens settings.
Enable **Use Option as Alt** under **Behavior** when needed.

Bidirectional rendering is enabled by default. For applications that already
arrange RTL text themselves, turn it off under **Terminal → Text direction**.

## Development

Kwi Mac has its own versions, releases and support, maintained by
[Mohamed Elashri](https://github.com/MohamedElashri). We regularly review KDE Kwi
updates and integrate them through tested pull requests.

See [Development.md](Development.md) for building, testing, releases and maintenance.
Local development and CI use the same [Makefile](Makefile).

## License

The application is GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL;
the terminal libraries are LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL. Konsole palettes are GPL-2.0-or-later. See
[license texts](LICENSES/) and [palette attribution](libs/kwi-terminal/data/color-schemes/NOTICE).
