<!--
SPDX-FileCopyrightText: none
SPDX-License-Identifier: CC0-1.0
-->
# Zirq

Zirq is a macOS terminal derived from KDE Kwi, with independent features and releases.
Stack: C++23, Qt Quick/QML, KDE Frameworks, libghostty-vt.
Read [Development.md](Development.md) for procedures; keep this file concise.

## Code map

- `src/`: application, windows, settings; `src/declarative/`: QML; `src/ui/`: UI helpers.
- `libs/ghostty-qt/`: VT wrapper, input encoding, history, search.
- `libs/ghostty-qt-quick/`: rendering, glyph shaping, bidi layout, visual cell maps.
- `libs/kwi-terminal/`: terminal view, sessions, selection, accessibility, PTY/process handling.
- `craft/` and `scripts/macos/`: pinned dependencies, patches, packaging, diagnostics.
- `autotests/`, `libs/*/autotests/`, `tests/`: native/GUI tests; `tests/macos/`: Python tooling tests.

## Change policy

- Keep upstream merges manageable: isolate macOS behavior in helpers or guarded paths,
  preserve non-Mac paths, and separate features from broad refactors or formatting.
- Use Zirq for user-facing identity. Do not mechanically rename inherited `Kwi`
  namespaces, targets, QML modules, translation domains, or compatibility identifiers.
  Preserve compatibility for profile/config names, shortcut IDs, and preset formats.
  Breaking changes require migration or fallback and a real previous-release upgrade check.
- Keep the VT grid, PTY input, copy, and search in logical order. Bidi belongs in
  presentation; rendering, hit testing, selection, IME, and accessibility must agree
  on visual cell maps. Preserve bidi controls through VT parsing and reflow.
- Change dependencies through tracked inputs, never hand-edited Craft outputs.
  Pins include `scripts/macos/craft.py` and `craft/` recipes/manifests. Keep Ghostty's
  recipe, `craft/ghostty-sources.json`, and `craft/release-sources.json` consistent.
  Preserve checksums, provenance, and licenses. Separate unrelated dependency updates;
  include required API migrations with the code they enable.
- For upstream syncs, run `make upstream`, integrate on a sync branch, and record the
  baseline in `craft/kwi/kwi/kwi.py`. Review conflicts for both upstream fixes and
  downstream behavior; avoid blanket ours/theirs resolutions.
- Follow surrounding style and SPDX conventions. Add focused regression coverage
  for nontrivial fixes and behavior changes.

## Build and validation

- Use the root Makefile and local `build-macos/` Craft prefix. Native builds need
  Apple Silicon, full Xcode (`DEVELOPER_DIR`), and Python 3.11+.
  Serialize builds sharing a prefix; use `JOBS` for compiler parallelism.
- Run `make check` (tooling/whitespace; no app build) for all changes.
  For application/build changes, run `make test` to build and run headless native tests.
  Add `make test-gui` for rendering, input, window, or other GUI-sensitive changes.
  Run GUI suites serially in a focused Mac session, even across separate prefixes.
- For VT/rendering changes, check bidi controls, reflow, copy/search, selection,
  and ASCII/RTL performance as relevant. For distribution/dependency changes, run
  relevant packaging/upgrade checks. `make ci` includes packaging, excluding GUI tests.
- Use existing harnesses and isolated profiles; never overwrite personal settings
  or install over the user's app for validation. Keep generated outputs out of commits.
  Report results and skipped checks with reasons.
