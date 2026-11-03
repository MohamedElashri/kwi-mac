#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kwi contributors
# SPDX-License-Identifier: CC0-1.0
"""Shared process-ownership and profile helpers for macOS app tests."""
import ctypes
import json
import os
from pathlib import Path
import plistlib
import subprocess
import time


def ensure_kwi_not_running(lock=None):
    lock = lock if lock is not None else Path(f"/tmp/org.kde.kwi-{os.geteuid()}.socket.lock")
    if not lock.exists():
        return
    owner = int(lock.read_text().splitlines()[0])
    try:
        os.kill(owner, 0)
    except ProcessLookupError:
        return  # QLockFile handles stale endpoint cleanup.
    raise RuntimeError("Close the existing Kwi instance before measuring; it was not modified")


def executable(app):
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    return app / "Contents/MacOS" / info["CFBundleExecutable"]


def process_path(pid):
    library = ctypes.CDLL("/usr/lib/libproc.dylib")
    path = ctypes.create_string_buffer(4096)
    if library.proc_pidpath(pid, path, len(path)) <= 0:
        raise RuntimeError("Cannot identify the controlled app process")
    return Path(os.fsdecode(path.value)).resolve()


def activate_owned_app(app, pid):
    """Set up a foreground parser run before releasing the held producer."""
    if not process_path(pid).samefile(executable(app)):
        raise RuntimeError("Activation target is not the owned app")
    subprocess.run(["/usr/bin/open", "-a", str(app)], check=True, timeout=5)
    query = ('ObjC.import("AppKit"); const target = ' + str(int(pid)) + '; const deadline = Date.now() + 5000; '
             'while ($.NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier !== target && Date.now() < deadline) '
             '{$.NSThread.sleepForTimeInterval(0.05);} $.NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;')
    foreground = int(subprocess.check_output(["/usr/bin/osascript", "-l", "JavaScript", "-e", query], text=True, timeout=7).strip())
    if foreground != pid or not process_path(pid).samefile(executable(app)):
        raise RuntimeError("Owned parser app did not acquire foreground activation")
    time.sleep(.2)  # Activation/layout settling is outside the producer's timed interval.
    return foreground


def isolated_profile(home, width, height, opacity, history):
    preferences = home / "Library/Preferences"
    (preferences / "State").mkdir(parents=True, exist_ok=True)
    (preferences / "kwiterminalrc").write_text(
        f"[Appearance]\nUseSystemFont=false\nFontFamily=Menlo\nFontPointSize=10\nBackgroundOpacity={opacity}\nBackgroundBlur=false\n"
        f"[Terminal]\nScrollbackLines={history}\nCursorBlinking=false\n")
    (preferences / "State/kwistaterc").write_text(
        f"[MainWindow]\n1440x900 screen: Width={width}\n1440x900 screen: Height={height}\n"
        "1440x900 screen: XPosition=0\n1440x900 screen: YPosition=35\n")


def wait_json(path, process, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        error = path.parent / "error.json"
        if error.exists():
            raise RuntimeError(error.read_text())
        if path.exists():
            return json.loads(path.read_text())
        if process.poll() is not None:
            raise RuntimeError(f"Launcher exited {process.returncode}; inspect {path.parent / 'app.log'}")
        time.sleep(.02)
    raise TimeoutError(f"No {path.name} within {timeout}s")
