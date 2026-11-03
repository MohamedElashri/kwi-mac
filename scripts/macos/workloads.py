#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Generate deterministic first-bundle workloads; outputs belong in the build tree."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build-macos/workloads"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    ascii_block = b"".join((f"KWI ASCII row {n:02d} ".encode().ljust(127, b"x") + b"\n") for n in range(32))
    ansi_packet = b"\x1b[H\x1b[38;2;80;160;240mKWI ANSI cursor update\x1b[0m\x1b[K\x1b[2B\rsecond row\x1b[1A\rreplace row\x1b[K"
    ansi_block = ansi_packet.ljust(4096, b" ")
    for name, block in (("ascii-64MiB.bin", ascii_block), ("ansi-64MiB.bin", ansi_block)):
        with (args.output / name).open("wb") as stream:
            for _ in range(64 * 1024 * 1024 // len(block)):
                stream.write(block)
    unicode_text = "\n".join("".join(chr(0x4E00 + start + n) for n in range(64)) for start in range(0, 4096, 64))
    unicode_text += "\nCombining: cafe\u0301  Ligatures: != -> ===  Emoji: 👩🏽‍💻 🚀 🦊 🧪\n"
    (args.output / "unicode.txt").write_text(unicode_text, encoding="utf-8")
    (args.output / "startup.txt").write_bytes(b"KWI_STARTUP_READY\r\n")
    with (args.output / "history-100000.txt").open("w", encoding="utf-8") as stream:
        for n in range(100000):
            stream.write(f"KWI_HISTORY_{n:06d} needle-{n % 997:03d} café\n")
    manifest = {"algorithm": "fixed blocks; no randomness", "files": {}}
    for path in sorted(args.output.iterdir()):
        if path.name == "manifest.json":
            continue
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        manifest["files"][path.name] = {"bytes": path.stat().st_size, "sha256": digest}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
