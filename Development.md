<!--
SPDX-FileCopyrightText: 2026 Kwi contributors
SPDX-License-Identifier: CC0-1.0
-->
# Development

Requires an Apple Silicon Mac, full Xcode, Git and Python 3.11 or later.
Dependencies are pinned in a project-local Craft prefix under `build-macos/`.
Use the root [Makefile](Makefile) for local work and CI.

## Build and run

```sh
git clone https://github.com/MohamedElashri/zirq.git
cd zirq
make run JOBS=2
```

If `xcode-select` points to Command Line Tools, set `DEVELOPER_DIR` to your full
Xcode `Contents/Developer` directory. Quit an older Zirq or Kwi instance before testing
a new build.

## Commands

| Command | Purpose |
| --- | --- |
| `make build` | Build and install into the local prefix |
| `make check` | Fast tooling tests and whitespace checks |
| `make test` | Build, tooling tests and headless native tests |
| `make test-gui` | Isolated Mac GUI tests; keep their windows focused |
| `make package` | Verified app archive and matching sources |
| `make ci` | Tooling checks, build, tests and release packaging |
| `make install-release` | Install or update the published app without building |
| `make update-cask TAG=vX.Y.Z` | Refresh Homebrew's version and checksum after publishing |
| `make sync-cask` | Open a signed cask update PR for the latest published release |
| `make help` | All targets, including screenshots and performance checks |

To test installation in a separate directory, use
`make install-release ARGS='--app-dir /path/to/Applications'`.
Install, update and rollback tests run in `make check`. After producing release
assets, test the actual published Kwi 0.2.0 archive against the new ZIP:

```sh
gh release download v0.2.0 --pattern Kwi-arm64.zip --pattern SHA256SUMS --dir build-macos/upgrade-v0.2.0
python3 scripts/macos/upgrade.py --previous build-macos/upgrade-v0.2.0 --assets build-macos/release-assets --probe build-macos/craft-root/build/kwi/kwi/work/build/bin/test-macos-compatibility.app/Contents/MacOS/test-macos-compatibility --tag v0.3.0 --output build-macos/results/upgrade-v0.3.0
```

The output directory must be new. This launches both genuine bundles with an
isolated home, substitutes only the installer's download transport, and verifies
profile preservation, presets, fonts, palettes, shortcuts, bidi preference and
window state. It never installs into the user's Applications directory.

## Releases

Zirq versions and release dates are independent of KDE Kwi. Use minor releases
for features and patch releases for fixes. Each release should identify its KDE
upstream commit and include the pinned dependency provenance with its sources.

Use a clean, committed checkout of a version tag matching `CMakeLists.txt`:

```sh
make ci JOBS=2
make release TAG=vX.Y.Z
```

Replace `vX.Y.Z` with that tag. Draft releases require authenticated GitHub CLI
(`gh`) and a tag pushed to GitHub. Assets include checksums, licenses and matching sources.

CI runs `make check` for docs and distribution changes. Application/build changes,
version tags and manual runs use `make ci`.

Publishing a stable release automatically opens a GitHub-signed bot PR updating
`Casks/zirq.rb` and runs its tooling checks. Review and merge that PR to update
Homebrew. This repository doubles as the tap. Use the CI workflow's **Sync Homebrew**
option to retry, or `make sync-cask` from a clean, current checkout of `main`.
Actions must be allowed to create pull requests in repository settings. GitHub
combines that permission with review approvals; this workflow never approves PRs.

Use your verified Git identity for personal commits. Support and bug reports go
through the [Zirq issue tracker](https://github.com/MohamedElashri/zirq/issues).
Automated cask commits use GitHub's verified bot identity.

## Maintenance

Mac integration and RTL features are maintained in this fork:

- [Ghostty patch](craft/kwi/libghostty-vt/bidi-controls.patch): direction controls, selection and formatting.
- [Rendering libraries](libs/ghostty-qt-quick/src/): bidi layout, shaping and cell maps.
- [Terminal view](libs/kwi-terminal/src/TerminalView.cpp): input, selection and accessibility geometry.

Keep the VT grid, PTY input, copying and search in logical order. Rendering and
pointer coordinates share visual cell maps. Make records the Ghostty patch hash
and source manifest and rebuilds the dependency when either changes.

The Mac build pins the standalone libghostty-vt 1.0.0 release and its offline
dependency archive in `craft/ghostty-sources.json`. Keep the Craft recipe and
`craft/release-sources.json` in sync when updating it.

`make upstream` and the [weekly workflow](.github/workflows/upstream.yml) report KDE
changes; review updates on an integration branch. Prefer tagged KDE releases as
routine baselines when available, with relevant fixes between releases. Update
Ghostty, Qt and ICU separately. Check control preservation, reflow, selection and
ASCII/RTL performance after relevant changes.

## Branding and profile compatibility

Zirq uses the `zirq` executable, `Zirq.app` bundle and `com.elashri.zirq` app ID.
Version 0.3.0 is the first source version with the Zirq identity. The tap keeps
its verified 0.2.0 archive until a Zirq release is published; the cask updater
then switches both the archive and bundle names together.

The top-level CMake project is `zirq`. Internal library targets, QML modules,
translation domains and `KwiLaunchPreset`
files retain their upstream names. The existing `kwirc`, `kwiterminalrc`, `kwistaterc`,
shortcut collections and `kwi/` data directories remain the profile locations.
This preserves user data and keeps upstream synchronization manageable.

The name audit covers tracked filenames as well as file contents. Remaining
references have specific purposes:

| Location | Why the old name remains |
| --- | --- |
| Preferences, presets, shortcut IDs, drag MIME type, and `KwiLaunchPreset` | Existing data and integrations must keep working |
| `Kwi` C++ namespace, `org.kde.kwi` QML modules, library targets, and translation catalogs | Upstream source/API identities; these do not set the app's displayed name |
| `craft/kwi/`, dependency receipts, resource paths, and test fixture markers | Build and test plumbing, including verification against the predecessor |
| Old archive names, installer detection, locks, cask rename, and owned-output markers | Safe upgrades and recovery from earlier installations/builds |
| KDE Kwi acknowledgments, copyright and license records | Upstream attribution |

New user-facing product names, app metadata, icons, support URLs, terminal
identification, and release artifacts use Zirq. Do not mechanically rename the
compatibility identifiers in the table. When changing one, provide a migration
or fallback and test the real previous-release upgrade.

Renaming the GitHub repository does not rename an existing local checkout.
Update its remote with `git remote set-url origin https://github.com/MohamedElashri/zirq.git`.
The directory name can still appear as a terminal tab title. A built Craft prefix
contains absolute paths, so use a fresh prefix after moving the checkout.

Diagnostics use `ZIRQ_TRACE` and `ZIRQ_TRACE_MARKER`; the old `KWI_TRACE`
variables remain accepted for existing developer scripts. New bundles record
provenance in `zirq-bundle.json`, including the icon and metadata input hashes.

`logo.svg` is the vector source for the Zirq mark. Regenerate the 1024-pixel
`logo.png` with `rsvg-convert -w 1024 -h 1024 -o logo.png logo.svg`; the Mac build
generates its Retina icon from that image.
