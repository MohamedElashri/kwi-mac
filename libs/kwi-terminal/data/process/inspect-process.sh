#!/bin/sh
# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

PATH=/usr/bin:/bin

# Arguments: cmdline byte limit, session PID/start time, mode (startup/shell/command/cleanup),
# and the last observed SSH PID/start time/executable.
command_line_limit=$1
shift

pid=$1
expected=$2
mode=$3
known_ssh_pid=$4
known_ssh_start=$5
known_ssh_executable=$6

stat=$(cat "/proc/$pid/stat") || exit 1
set -- ${stat##*) }

[ "$4" = "$pid" ] || exit 1
start=${20}
[ -z "$expected" ] || [ "$start" = "$expected" ] || exit 1

foreground=$6
[ "$foreground" -gt 0 ] || foreground=$pid
target=$foreground
members=

# The foreground group can outlive its leader, so look for a surviving member.
if [ "$mode" = cleanup ] || [ ! -d "/proc/$target" ]; then
    for path in /proc/[0-9]*/stat; do
        IFS= read -r stat 2>/dev/null < "$path" || continue
        member=${stat%% *}
        set -- ${stat##*) }

        [ "$4" = "$pid" ] || continue

        if [ "$3" = "$foreground" ] && [ ! -d "/proc/$target" ]; then
            target=$member
        fi

        [ "$3" = "$pid" ] || [ "$3" = "$foreground" ] || continue
        members="$members $member:${20}"
    done
fi

# Cleanup needs only the verified identities, not cwd, executable or argv.
if [ "$mode" = cleanup ]; then
    printf '%s\000' "$start" "$foreground" '' '' "$members" '' ''
    exit 0
fi

directory=$(readlink -- "/proc/$target/cwd") || directory=$(readlink -- "/proc/$pid/cwd") || directory=
foreground_executable=$(readlink -- "/proc/$target/exe") || foreground_executable=

# Startup observation also needs argv to distinguish the launcher from the command.
read_arguments=0

if [ "$mode" = startup ] || { [ "$mode" = shell ] && [ "$foreground" = "$pid" ]; }; then
    read_arguments=1
fi

ssh_pid=
ssh_start=

if [ "${foreground_executable##*/}" = ssh ] && IFS= read -r stat < "/proc/$target/stat"; then
    set -- ${stat##*) }
    ssh_pid=$target
    ssh_start=${20}

    if [ "$known_ssh_pid" != "$ssh_pid" ] || [ "$known_ssh_start" != "$ssh_start" ] || [ "$known_ssh_executable" != "$foreground_executable" ]; then
        read_arguments=1
    fi
fi

printf '%s\000' "$start" "$foreground" "$directory" "$foreground_executable" "$members" "$ssh_pid" "$ssh_start"

if [ "$read_arguments" = 1 ]; then
    # Read one extra byte so truncation at a NUL boundary still cannot prove idle.
    head -c "$command_line_limit" "/proc/$target/cmdline" || :
fi
