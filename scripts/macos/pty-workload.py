#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Controlled terminal child: fixed geometry, bounded writes and DSR acknowledgements.

DSR measures parser round trips, never pixel presentation. The child remains alive
after completion so an exit banner cannot change the viewport during a sample.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import termios
import time


def clock_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def geometry():
    rows, columns, width, height = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, bytes(8)))
    return {"rows": rows, "columns": columns, "pixel_width": width, "pixel_height": height}


def write_all(data, deadline):
    view = memoryview(data)
    while view:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([], [1], [], remaining)[1]:
            raise TimeoutError("PTY write deadline")
        try:
            count = os.write(1, view[:65536])
            if count <= 0:
                raise RuntimeError("PTY closed during output")
            view = view[count:]
        except BlockingIOError:
            pass


def reply(deadline):
    data = bytearray()
    while len(data) < 4096:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([0], [], [], remaining)[0]:
            raise TimeoutError("No bounded DSR reply")
        chunk = os.read(0, 1024)
        if not chunk:
            raise RuntimeError("PTY input closed")
        data.extend(chunk)
        found = re.search(rb"\x1b\[(\d+);(\d+)R", data)
        if found:
            return [int(found[1]), int(found[2])]
    raise RuntimeError("Oversized terminal reply")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--workload", type=Path)
    parser.add_argument("--seconds", type=float, default=0)
    parser.add_argument("--rate", type=int, default=0)
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--keep-viewport", action="store_true")
    parser.add_argument("--glyphs", action="store_true")
    parser.add_argument("--seed", action="store_true")
    args = parser.parse_args()
    directory = args.directory.resolve()
    original = termios.tcgetattr(0)
    flags = termios.tcgetattr(0)
    flags[0] &= ~(termios.ICRNL | termios.IXON)
    flags[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN)
    flags[6][termios.VMIN], flags[6][termios.VTIME] = 1, 0
    termios.tcsetattr(0, termios.TCSANOW, flags)
    fcntl.fcntl(1, fcntl.F_SETFL, fcntl.fcntl(1, fcntl.F_GETFL) | os.O_NONBLOCK)
    try:
        time.sleep(.4)  # Exclude transient initial Qt/Kirigami layout.
        ready = {"pid": os.getpid(), "parent_pid": os.getppid(), "ready_ns": clock_ns(),
                 "geometry": geometry(), "onlcr": bool(flags[1] & termios.ONLCR),
                 "terminal": os.environ.get("TERM_PROGRAM"), "terminal_version": os.environ.get("TERM_PROGRAM_VERSION")}
        save(directory / "ready.json", ready)
        deadline = time.monotonic() + 90
        while not (directory / "start").exists():
            if (directory / "stop").exists():
                return
            if time.monotonic() > deadline:
                raise TimeoutError("Controller did not start the workload")
            time.sleep(.01)
        initial = geometry()
        write_all(b"\x1b[?25l\x1b[0m\x1b[2J\x1b[H\x1b[6n", time.monotonic() + 5)
        if reply(time.monotonic() + 5) != [1, 1]:
            raise RuntimeError("Initial cursor response differs from 1,1")
        save(directory / "started.json", {"geometry": geometry(), "started_ns": clock_ns()})
        data = args.workload.read_bytes() if args.workload else b""
        if args.seed:
            write_all(data + b"\x1b[6n", time.monotonic() + 90)
            position = reply(time.monotonic() + 5)
            save(directory / "seeded.json", {"bytes": len(data), "cursor": position})
        started = clock_ns()
        queries = []
        if args.glyphs:
            written = 0
            passes = 0
            until = time.monotonic() + 90
            while time.monotonic() < until and not (directory / "stop").exists():
                if select.select([0], [], [], .05)[0]:
                    incoming = os.read(0, 128)
                    if not incoming:
                        break
                    for _ in incoming:
                        passes += 1
                        write_all(b"\x1b[?25h\x1b[H\x1b[2J" + data + f"\x1b[{geometry()['rows']};1H\x1b[2KKWI_GLYPH_READY\x1b[6n".encode(), time.monotonic() + 5)
                        reply(time.monotonic() + 5)
                        save(directory / "pass.json", {"pass": passes})
                        written += len(data)
        elif args.echo:
            written = 0
            until = time.monotonic() + min(90, args.seconds or 75)
            while time.monotonic() < until and not (directory / "stop").exists():
                if select.select([0], [], [], .05)[0]:
                    incoming = os.read(0, 128)
                    if not incoming:
                        break
                    for value in incoming:
                        written += 1
                        write_all(b"\x1b[H\x1b[2K" + f"ECHO_{written:06d}_{value:02x}".encode(), time.monotonic() + 5)
        elif args.seconds:
            until = time.monotonic() + args.seconds
            next_tick = time.monotonic()
            block = data if data else (b"KWI sustained output " + b"x" * 105 + b"\r\n") * 32
            written = 0
            ticks = 0
            while time.monotonic() < until:
                if args.rate:
                    count = max(1, round((ticks + 1) * args.rate / 60) - written)
                    offset = written % len(block) if data else 0
                    packet = (block * ((offset + count + len(block) - 1) // len(block)))[offset:offset + count]
                    write_all(packet, time.monotonic() + 5)
                    written += len(packet)
                else:
                    stamp = clock_ns()
                    write_all(b"\x1b[H\x1b[2K" + f"KWI_TUI {len(queries):06d}".encode() + b"\x1b[6n", time.monotonic() + 5)
                    position = reply(time.monotonic() + 5)
                    queries.append({"round_trip_ns": clock_ns() - stamp, "position": position})
                ticks += 1
                next_tick += 1 / 60
                time.sleep(max(0, next_tick - time.monotonic()))
        else:
            write_all(data, time.monotonic() + 90)
            written = len(data)
        marker = b"KWI_BENCHMARK_DONE"
        marker_row = geometry()["rows"] if args.keep_viewport else 1
        tail = f"\x18\x1b[0m\x1b[{marker_row};1H\x1b[2K".encode() if args.keep_viewport else b"\x18\x1b[0m\x1b[2J\x1b[H"
        write_all(tail + marker + b"\x1b[6n", time.monotonic() + 5)
        position = reply(time.monotonic() + 5)
        finished = clock_ns()
        if position != [marker_row, len(marker) + 1]:
            raise RuntimeError(f"Final cursor mismatch: {position}")
        final = geometry()
        save(directory / "result.json", {**ready, "started_ns": started, "finished_ns": finished,
             "elapsed_ms": (finished - started) / 1e6, "bytes": written,
             "sha256": hashlib.sha256(data).hexdigest() if data else None,
             "initial_geometry": initial, "final_geometry": final, "geometry_stable": initial == final,
             "kept_viewport": args.keep_viewport,
             "final_cursor": position, "parser_mib_per_second": written / 1024**2 / ((finished - started) / 1e9),
             "protocol_round_trips": queries, "scope": "PTY write through final DSR acknowledgement; not rendered/presented output"})
        while not (directory / "stop").exists() and time.monotonic() < deadline + 180:
            time.sleep(.05)
    except Exception as error:
        save(directory / "error.json", {"error": str(error), "geometry": geometry()})
        raise
    finally:
        termios.tcsetattr(0, termios.TCSANOW, original)


if __name__ == "__main__":
    main()
