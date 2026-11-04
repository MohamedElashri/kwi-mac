#!/bin/bash
# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0

set -euo pipefail

KWI_INSTALL_WORK=""
KWI_INSTALL_LOCK=""
KWI_INSTALL_BACKUP=""
KWI_INSTALL_DEST=""
KWI_INSTALL_COMMITTED=0

fail() { printf 'Kwi Mac: %s\n' "$*" >&2; exit 1; }

cleanup() {
    local status=$? preserve=0
    trap - EXIT HUP INT TERM
    if [[ "$KWI_INSTALL_COMMITTED" = 0 && -n "$KWI_INSTALL_BACKUP" && -e "$KWI_INSTALL_BACKUP" ]]; then
        if [[ ! -e "$KWI_INSTALL_DEST" && ! -L "$KWI_INSTALL_DEST" ]] && mv "$KWI_INSTALL_BACKUP" "$KWI_INSTALL_DEST"; then
            printf 'Previous installation restored.\n' >&2
        else
            printf 'Previous installation preserved at %s\n' "$KWI_INSTALL_BACKUP" >&2
            preserve=1
        fi
    fi
    if [[ -n "$KWI_INSTALL_WORK" && "$preserve" = 0 ]]; then
        if ! rm -rf "$KWI_INSTALL_WORK"; then printf 'Could not remove installer files at %s\n' "$KWI_INSTALL_WORK" >&2; fi
    fi
    if [[ -n "$KWI_INSTALL_LOCK" ]]; then rmdir "$KWI_INSTALL_LOCK" || true; fi
    exit "$status"
}

download() {
    curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
        --connect-timeout 20 --max-time 300 --retry 3 "$@"
}

plist() { plutil -extract "$2" raw -o - "$1/Contents/Info.plist"; }

version_at_least() {
    local a b c x y z
    IFS=. read -r a b c <<< "$1"
    IFS=. read -r x y z <<< "$2"
    a=$((10#$a)); b=$((10#${b:-0})); c=$((10#${c:-0}))
    x=$((10#$x)); y=$((10#${y:-0})); z=$((10#${z:-0}))
    (( a > x || (a == x && b > y) || (a == x && b == y && c >= z) ))
}

check_app() {
    [[ -d "$1" && ! -L "$1" ]] || fail "Refusing to replace an invalid or symlinked app: $1"
    [[ "$(plist "$1" CFBundleIdentifier)" = org.kde.kwi && "$(plist "$1" CFBundleExecutable)" = kwi ]] \
        || fail "The app at $1 is not Kwi."
}

not_running() {
    local status=0
    pgrep -x kwi >/dev/null 2>&1 || status=$?
    case "$status" in
        0) fail 'Quit Kwi and run this installer from another terminal to update.' ;;
        1) return 0 ;;
        *) fail 'Cannot check whether Kwi is running; the installed app was preserved.' ;;
    esac
}

main() {
    local app_dir="" repository=https://github.com/MohamedElashri/kwi-mac
    local release_url tag version installed system_version minimum checksum actual staged
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --app-dir)
                [[ $# -ge 2 && -n "$2" ]] || fail '--app-dir requires a directory.'
                app_dir=$2; shift 2 ;;
            -h|--help)
                printf 'Install or update the latest stable Kwi Mac release.\nUsage: bash install.sh [--app-dir DIRECTORY]\n'
                return ;;
            *) fail "Unknown option: $1" ;;
        esac
    done
    [[ "$(uname -s)" = Darwin ]] || fail 'This installer requires macOS.'
    [[ "$(uname -m)" = arm64 || "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]] \
        || fail 'Kwi Mac requires Apple Silicon.'

    if [[ -z "$app_dir" ]]; then
        if [[ -e /Applications/Kwi.app || -L /Applications/Kwi.app ]]; then app_dir=/Applications
        elif [[ -e "$HOME/Applications/Kwi.app" || -L "$HOME/Applications/Kwi.app" ]]; then app_dir="$HOME/Applications"
        elif [[ -w /Applications ]]; then app_dir=/Applications
        else app_dir="$HOME/Applications"; fi
    fi
    mkdir -p "$app_dir"
    app_dir=$(cd "$app_dir" && pwd -P)
    [[ -w "$app_dir" ]] || fail "Cannot write to $app_dir. Choose --app-dir \"\$HOME/Applications\"."
    if ! mkdir "$app_dir/.kwi-install.lock" 2>/dev/null; then fail "Another install may be running: $app_dir/.kwi-install.lock"; fi
    KWI_INSTALL_LOCK="$app_dir/.kwi-install.lock"
    trap cleanup EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    KWI_INSTALL_DEST="$app_dir/Kwi.app"

    release_url=$(download --output /dev/null --write-out '%{url_effective}' "$repository/releases/latest")
    [[ "$release_url" = "$repository/releases/tag/"* ]] || fail 'GitHub did not return a release tag.'
    tag=${release_url##*/}
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Unsupported release tag: $tag"
    version=${tag#v}
    if [[ -e "$KWI_INSTALL_DEST" || -L "$KWI_INSTALL_DEST" ]]; then
        check_app "$KWI_INSTALL_DEST"
        installed=$(plist "$KWI_INSTALL_DEST" CFBundleShortVersionString)
        [[ "$installed" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Unrecognized installed version: $installed"
        if version_at_least "$installed" "$version"; then
            if [[ "$installed" != "$version" ]] || codesign --verify --deep --strict "$KWI_INSTALL_DEST" 2>/dev/null; then
                printf 'Kwi Mac %s is already installed at %s.\n' "$installed" "$KWI_INSTALL_DEST"
                return
            fi
        fi
    fi
    not_running
    KWI_INSTALL_WORK=$(mktemp -d "$app_dir/.kwi-install.XXXXXX")
    printf 'Downloading Kwi Mac %s…\n' "$version"
    download --output "$KWI_INSTALL_WORK/SHA256SUMS" "$repository/releases/download/$tag/SHA256SUMS"
    download --output "$KWI_INSTALL_WORK/Kwi-arm64.zip" "$repository/releases/download/$tag/Kwi-arm64.zip"
    checksum=$(awk '$2 == "Kwi-arm64.zip" && NF == 2 { count++; hash=$1 } END { if (count != 1) exit 1; print hash }' "$KWI_INSTALL_WORK/SHA256SUMS") \
        || fail 'The release checksum is missing or ambiguous.'
    [[ "$checksum" =~ ^[0-9a-f]{64}$ ]] || fail 'The release checksum is invalid.'
    actual=$(shasum -a 256 "$KWI_INSTALL_WORK/Kwi-arm64.zip")
    [[ "${actual%% *}" = "$checksum" ]] || fail 'Download checksum verification failed; the installed app was preserved.'
    zipinfo -1 "$KWI_INSTALL_WORK/Kwi-arm64.zip" | awk '
        /(^|\/)\.\.(\/|$)/ || /^\// { exit 1 }
        !/^(Kwi\.app|__MACOSX)(\/|$)/ { exit 1 }
    ' || fail 'The release archive contains unexpected paths.'
    mkdir "$KWI_INSTALL_WORK/extracted"
    ditto -x -k "$KWI_INSTALL_WORK/Kwi-arm64.zip" "$KWI_INSTALL_WORK/extracted"
    staged="$KWI_INSTALL_WORK/extracted/Kwi.app"
    check_app "$staged"
    [[ -x "$staged/Contents/MacOS/kwi" ]] || fail 'The release executable is missing or not executable.'
    [[ "$(plist "$staged" CFBundleShortVersionString)" = "$version" ]] || fail 'The app version does not match the release.'
    minimum=$(plist "$staged" LSMinimumSystemVersion)
    system_version=$(sw_vers -productVersion)
    [[ "$minimum" =~ ^[0-9]+(\.[0-9]+){0,2}$ && "$system_version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] \
        || fail 'Cannot determine the required macOS version.'
    version_at_least "$system_version" "$minimum" || fail "This release requires macOS $minimum or later."
    codesign --verify --deep --strict "$staged" || fail 'App signature verification failed; the installed app was preserved.'
    not_running
    if [[ -e "$KWI_INSTALL_DEST" ]]; then
        KWI_INSTALL_BACKUP="$KWI_INSTALL_WORK/previous.app"
        mv "$KWI_INSTALL_DEST" "$KWI_INSTALL_BACKUP"
    fi
    mv "$staged" "$KWI_INSTALL_DEST"
    KWI_INSTALL_COMMITTED=1
    printf 'Installed Kwi Mac %s at %s.\nRun this installer again to update.\n' "$version" "$KWI_INSTALL_DEST"
}

# An incomplete final block cannot execute if a piped download is truncated.
{
    main "$@"
}
