<!--
SPDX-FileCopyrightText: 2026 Mohamed Elashri
SPDX-License-Identifier: CC0-1.0
-->
# Installation and upgrades

Zirq requires an Apple Silicon Mac running macOS 14.4 or later.

## Release availability

Version 0.3.0 is the first Zirq-branded release. Until it is published, the stable
installer and Homebrew cask still download the published 0.2.0 Kwi bundle. That
older binary can still identify itself as Kwi even when installed as `Zirq.app`.
New Zirq releases use `Zirq-arm64.zip`, executable `zirq`, and bundle identity
`com.elashri.zirq`.

## Installer

```sh
curl -fsSL https://raw.githubusercontent.com/MohamedElashri/zirq/main/install.sh | bash
```

The installer verifies checksums and signatures, and chooses `/Applications` or
`~/Applications` without sudo. Quit the app and run it from another terminal to
update. For a custom destination:

```sh
curl -fsSL https://raw.githubusercontent.com/MohamedElashri/zirq/main/install.sh | bash -s -- --app-dir "$HOME/Applications"
```

## Homebrew

```sh
brew tap MohamedElashri/zirq https://github.com/MohamedElashri/zirq.git
brew install --cask mohamedelashri/zirq/zirq
```

To update:

```sh
brew update
brew upgrade --cask mohamedelashri/zirq/zirq
```

## Manual installation and first launch

Download the app archive from [Releases](https://github.com/MohamedElashri/zirq/releases),
verify it with the included `SHA256SUMS`, extract it, and move `Zirq.app` to
Applications. Keep the app bundle intact.

Builds are ad-hoc signed and not notarized. If macOS blocks a verified download
from this repository, remove quarantine from this app only:

```sh
xattr -dr com.apple.quarantine /Applications/Zirq.app
open /Applications/Zirq.app
```

For a home-directory installation, use `"$HOME/Applications/Zirq.app"` in both
commands. Removing quarantine does not repair a modified or corrupt bundle;
download it again if checksum or signature verification fails.

## Moving from Kwi

Quit Kwi before upgrading. The installer replaces `Kwi.app` with `Zirq.app` and
restores the old app if replacement fails. Existing preferences, presets and
default preset, palettes, shortcuts, and saved window state carry over. The
existing profile paths remain in use for compatibility.

Open tabs, running processes, and terminal scrollback do not resume. Your shell
manages its own history. Re-add Zirq to the Dock and update custom launchers or
aliases pointing to the old app or executable. macOS permission grants belong
to the application identity and may need to be granted to Zirq.

If a tab still says `kwi-mac`, check its working directory: tab titles can show
the current folder. The repository rename does not rename an existing checkout.
The app menu and About window identify the running application; a 0.2.0 binary
will retain its original branding until it is replaced by 0.3.0 or later.

For an existing Homebrew installation, add the new tap and use the update
commands above. The tap maps the old `kwi-mac` cask to `zirq`. After a successful
upgrade, the old tap can be removed:

```sh
brew untap MohamedElashri/kwi-mac
```

Old GitHub repository and release links redirect to
[`MohamedElashri/zirq`](https://github.com/MohamedElashri/zirq).
To update a source checkout's remote, see [Development](Development.md).
