<!--
SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
SPDX-FileCopyrightText: 2026 Mohamed Elashri <mail@elashri.com>
SPDX-License-Identifier: CC0-1.0
-->
# <img src="logo.png" width="32"/> Kwi for macOS

An Apple Silicon port of [KDE Kwi](https://invent.kde.org/utilities/kwi), built
with libghostty-vt, Qt Quick/Metal and KDE Frameworks. Includes native Mac
shortcuts, tabs, presets, color schemes, search and a 14 pt default terminal font.
This repository targets macOS; it is an independent port, not an official KDE release.

## Install

Download an arm64 ZIP from [Releases](https://github.com/MohamedElashri/kwi-mac/releases),
extract it, and copy `Kwi.app` to Applications. Development builds are ad-hoc
signed; Developer ID signing and notarization are not configured. The binary
deployment floor is macOS 14.4; runtime validation has been on an M1 with macOS 27.2.

Use **Command–,** for settings. **Use Option as Alt** is available in Behavior.
IME, other keyboard layouts/displays and VoiceOver still need broader verification.

## Build and test

Requires native Apple Silicon, full Xcode, Git and Python 3.11+. Craft installs
the pinned dependencies into `build-macos/craft-root`, without using Homebrew libraries.

```sh
export DEVELOPER_DIR="$(xcode-select -p)" # Must point to full Xcode, not Command Line Tools
python3 scripts/macos/craft.py prepare
python3 scripts/macos/craft.py run --install-deps kwi/kwi
python3 scripts/macos/craft.py run --configure kwi/kwi
python3 scripts/macos/craft.py run --make --install --post-install --qmerge --post-qmerge kwi/kwi
python3 -m unittest discover -s tests/macos -v
build-macos/craft-root/dev-utils/bin/ctest \
  --test-dir build-macos/craft-root/build/kwi/kwi/work/build -LE gui --output-on-failure
python3 scripts/macos/package.py --output build-macos/package
open build-macos/package/Kwi.app
```

For native GUI checks, generate fixtures with `python3 scripts/macos/workloads.py`,
then run CTest without `-LE gui` in a logged-in Mac session with the test windows
left focused. The interaction suite includes a repeatable settings/action audit.
Builds, local reports and `plan.md` are ignored. Dependency pins live in
`scripts/macos/craft.py` and `craft/`; the original upstream libraries remain in `libs/`.

## Releases

**Actions → macOS release** builds an existing `v0.1.0-mac.1`-style tag, or push
such a tag to start it. The workflow tests, fully deploys, audits and packages
arm64, then creates a **draft prerelease** with the ZIP, SHA-256 checksums and
source/dependency metadata. Review the draft before publishing. Hosted CI runs
the headless tests; local GUI/Metal checks remain separate. No Apple secrets are needed
for the current ad-hoc builds.

## Following upstream

`origin` is this port; `upstream` is KDE Kwi. The original Git history is retained,
and the weekly **Track upstream Kwi** workflow reports commits not yet merged.
Update on a review branch, build/test, then merge it into `main`:

```sh
git remote add upstream https://invent.kde.org/utilities/kwi.git # Once per clone
git fetch upstream
git switch -c sync/kwi
git merge upstream/master
```

The macOS changes are ordinary commits carried alongside upstream development;
there is no separate patch queue. Shared terminal fixes can be contributed to KDE.

## License

The application is GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL;
the embedded terminal libraries are LGPL-2.1-only OR LGPL-3.0-only OR
LicenseRef-KDE-Accepted-LGPL. Konsole palettes are GPL-2.0-or-later. Original
copyrights, [license texts](LICENSES/) and [palette attribution](libs/kwi-terminal/data/color-schemes/NOTICE)
are retained; bundled dependency notices are included inside the app.
