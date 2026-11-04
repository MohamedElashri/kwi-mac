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
git clone https://github.com/MohamedElashri/kwi-mac.git
cd kwi-mac
make run JOBS=2
```

If `xcode-select` points to Command Line Tools, set `DEVELOPER_DIR` to your full
Xcode `Contents/Developer` directory. Quit an older Kwi instance before testing
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
Install, update and rollback tests run in `make check`.

## Releases

Kwi Mac versions and release dates are independent of KDE Kwi. Use minor releases
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
`Casks/kwi-mac.rb` and runs its tooling checks. Review and merge that PR to update
Homebrew. This repository doubles as the tap. Use the CI workflow's **Sync Homebrew**
option to retry, or `make sync-cask` from a clean, current checkout of `main`.
Actions must be allowed to create pull requests in repository settings. GitHub
combines that permission with review approvals; this workflow never approves PRs.

Use your verified Git identity for personal commits. The app's public contact is
`kwi@elashri.com`; automated cask commits use GitHub's verified bot identity.

## Maintenance

Mac integration and RTL features are maintained in this fork:

- [Ghostty patch](craft/kwi/libghostty-vt/bidi-controls.patch): direction controls, selection and formatting.
- [Rendering libraries](libs/ghostty-qt-quick/src/): bidi layout, shaping and cell maps.
- [Terminal view](libs/kwi-terminal/src/TerminalView.cpp): input, selection and accessibility geometry.

Keep the VT grid, PTY input, copying and search in logical order. Rendering and
pointer coordinates share visual cell maps. Make records the Ghostty patch hash
and rebuilds the dependency when it changes.

`make upstream` and the [weekly workflow](.github/workflows/upstream.yml) report KDE
changes; review updates on an integration branch. Prefer tagged KDE releases as
routine baselines when available, with relevant fixes between releases. Update
Ghostty, Qt and ICU separately. Check control preservation, reflow, selection and
ASCII/RTL performance after relevant changes.
