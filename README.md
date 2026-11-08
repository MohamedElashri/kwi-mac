<!--
SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
SPDX-License-Identifier: CC0-1.0
-->
# <img src="logo.svg" width="56" alt=""/> Zirq

A fast terminal for macOS, with Arabic, Persian and Hebrew support.

[![CI](https://github.com/MohamedElashri/zirq/actions/workflows/checks.yml/badge.svg?branch=main)](https://github.com/MohamedElashri/zirq/actions/workflows/checks.yml)
[![Release](https://img.shields.io/github/v/release/MohamedElashri/zirq)](https://github.com/MohamedElashri/zirq/releases)

![Zirq in dark mode with tabbed sessions and multilingual terminal output](assets/readme/screenshot.png)

- Bidirectional text, Arabic shaping, and logical copy and search.
- Tabs, searchable scrollback, launch presets, and configurable shortcuts.
- Metal rendering, custom colors and fonts, and familiar Mac keyboard input.

## Install

Requires **Apple Silicon** and **macOS 14.4 or later**.

```sh
curl -fsSL https://raw.githubusercontent.com/MohamedElashri/zirq/main/install.sh | bash
```

Run the same command to update, after quitting the app. Prefer Homebrew?

```sh
brew tap MohamedElashri/zirq https://github.com/MohamedElashri/zirq.git
brew install --cask mohamedelashri/zirq/zirq
```

Or download the app from [Releases](https://github.com/MohamedElashri/zirq/releases).
See [installation and upgrade notes](Installation.md) for release availability,
custom locations, and migration from the old app.

### First launch

Builds are ad-hoc signed and not notarized. If macOS blocks a download you have
verified came from this repository, remove its quarantine attribute:

```sh
xattr -dr com.apple.quarantine /Applications/Zirq.app
```

Use `"$HOME/Applications/Zirq.app"` instead if installed in your home directory.

## Use

**⌘T** new tab · **⌘F** search · **⌘,** settings.
Fonts, colors, Option-as-Alt, and bidirectional rendering are configurable in settings.

Built on [KDE Kwi](https://invent.kde.org/utilities/kwi), Qt Quick, KDE Frameworks,
and libghostty-vt, with independent development and releases.
[Development](Development.md) · [Issues](https://github.com/MohamedElashri/zirq/issues) · [Licenses](LICENSES/)
