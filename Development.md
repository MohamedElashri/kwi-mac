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
| `make help` | All targets, including screenshots and performance checks |

## Releases

Use a clean, committed checkout of a version tag matching `CMakeLists.txt`:

```sh
make ci JOBS=2
make release TAG=vX.Y.Z
```

Replace `vX.Y.Z` with that tag. Draft releases require authenticated GitHub CLI
(`gh`) and a tag pushed to GitHub. Assets include checksums, licenses and matching sources.

CI runs `make check` for docs-only changes and `make ci` for code changes,
version tags and manual runs.

## Maintenance

Mac integration and RTL features are maintained in this fork:

- [Ghostty patch](craft/kwi/libghostty-vt/bidi-controls.patch): direction controls, selection and formatting.
- [Rendering libraries](libs/ghostty-qt-quick/src/): bidi layout, shaping and cell maps.
- [Terminal view](libs/kwi-terminal/src/TerminalView.cpp): input, selection and accessibility geometry.

Keep the VT grid, PTY input, copying and search in logical order. Rendering and
pointer coordinates share visual cell maps. Make records the Ghostty patch hash
and rebuilds the dependency when it changes.

`make upstream` and the [weekly workflow](.github/workflows/upstream.yml) report KDE
changes; review updates on an integration branch. Update Ghostty, Qt and ICU
separately. Check control preservation, reflow, selection and ASCII/RTL performance
after relevant changes.
