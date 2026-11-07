#!/bin/bash
# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0

set -euo pipefail

ZIRQ_INSTALL_WORK=""
ZIRQ_INSTALL_LOCK=""
ZIRQ_INSTALL_BACKUP=""
ZIRQ_INSTALL_DEST=""
ZIRQ_INSTALL_COMMITTED=0
ZIRQ_INSTALL_PREVIOUS_DEST=""
ZIRQ_INSTALL_LEGACY_LOCK=""

fail() { printf 'Zirq: %s\n' "$*" >&2; exit 1; }

cleanup() {
    local status=$? preserve=0
    trap - EXIT HUP INT TERM
    if [[ "$ZIRQ_INSTALL_COMMITTED" = 0 && -n "$ZIRQ_INSTALL_BACKUP" && -e "$ZIRQ_INSTALL_BACKUP" ]]; then
        if [[ ! -e "$ZIRQ_INSTALL_PREVIOUS_DEST" && ! -L "$ZIRQ_INSTALL_PREVIOUS_DEST" ]] && mv "$ZIRQ_INSTALL_BACKUP" "$ZIRQ_INSTALL_PREVIOUS_DEST"; then
            printf 'Previous installation restored.\n' >&2
        else
            printf 'Previous installation preserved at %s\n' "$ZIRQ_INSTALL_BACKUP" >&2
            preserve=1
        fi
    fi
    if [[ -n "$ZIRQ_INSTALL_WORK" && "$preserve" = 0 ]]; then
        if ! rm -rf "$ZIRQ_INSTALL_WORK"; then printf 'Could not remove installer files at %s\n' "$ZIRQ_INSTALL_WORK" >&2; fi
    fi
    if [[ -n "$ZIRQ_INSTALL_LEGACY_LOCK" ]]; then rmdir "$ZIRQ_INSTALL_LEGACY_LOCK" || true; fi
    if [[ -n "$ZIRQ_INSTALL_LOCK" ]]; then rmdir "$ZIRQ_INSTALL_LOCK" || true; fi
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
    local identity
    identity="$(plist "$1" CFBundleIdentifier)/$(plist "$1" CFBundleExecutable)"
    [[ "$identity" = com.elashri.zirq/zirq || "$identity" = org.kde.kwi/kwi ]] \
        || fail "The app at $1 is not Zirq or its Kwi predecessor."
}

not_running() {
    local status executable
    for executable in zirq kwi; do
        status=0
        pgrep -x "$executable" >/dev/null 2>&1 || status=$?
        case "$status" in
            0) fail 'Quit Zirq and Kwi and run this installer from another terminal to update.' ;;
            1) ;;
            *) fail 'Cannot check whether Zirq or Kwi is running; the installed app was preserved.' ;;
        esac
    done
}

main() {
    local app_dir="" repository=https://github.com/MohamedElashri/zirq
    local release_url tag version installed system_version minimum checksum actual staged product archive identity
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --app-dir)
                [[ $# -ge 2 && -n "$2" ]] || fail '--app-dir requires a directory.'
                app_dir=$2; shift 2 ;;
            -h|--help)
                printf 'Install or update the latest stable Zirq release.\nUsage: bash install.sh [--app-dir DIRECTORY]\n'
                return ;;
            *) fail "Unknown option: $1" ;;
        esac
    done
    [[ "$(uname -s)" = Darwin ]] || fail 'This installer requires macOS.'
    [[ "$(uname -m)" = arm64 || "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]] \
        || fail 'Zirq requires Apple Silicon.'

    if [[ -z "$app_dir" ]]; then
        if [[ -e /Applications/Zirq.app || -L /Applications/Zirq.app || -e /Applications/Kwi.app || -L /Applications/Kwi.app ]]; then app_dir=/Applications
        elif [[ -e "$HOME/Applications/Zirq.app" || -L "$HOME/Applications/Zirq.app" || -e "$HOME/Applications/Kwi.app" || -L "$HOME/Applications/Kwi.app" ]]; then app_dir="$HOME/Applications"
        elif [[ -w /Applications ]]; then app_dir=/Applications
        else app_dir="$HOME/Applications"; fi
    fi
    mkdir -p "$app_dir"
    app_dir=$(cd "$app_dir" && pwd -P)
    [[ -w "$app_dir" ]] || fail "Cannot write to $app_dir. Choose --app-dir \"\$HOME/Applications\"."
    if ! mkdir "$app_dir/.zirq-install.lock" 2>/dev/null; then fail "Another install may be running: $app_dir/.zirq-install.lock"; fi
    ZIRQ_INSTALL_LOCK="$app_dir/.zirq-install.lock"
    trap cleanup EXIT
    # Coordinate with installers downloaded before the rename as well.
    mkdir "$app_dir/.kwi-install.lock" 2>/dev/null || fail 'Another Kwi install may be running.'
    ZIRQ_INSTALL_LEGACY_LOCK="$app_dir/.kwi-install.lock"
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    ZIRQ_INSTALL_DEST="$app_dir/Zirq.app"
    ZIRQ_INSTALL_PREVIOUS_DEST="$ZIRQ_INSTALL_DEST"
    if [[ ! -e "$ZIRQ_INSTALL_DEST" && ! -L "$ZIRQ_INSTALL_DEST" && ( -e "$app_dir/Kwi.app" || -L "$app_dir/Kwi.app" ) ]]; then
        ZIRQ_INSTALL_PREVIOUS_DEST="$app_dir/Kwi.app"
    fi

    release_url=$(download --output /dev/null --write-out '%{url_effective}' "$repository/releases/latest")
    [[ "$release_url" = "$repository/releases/tag/"* ]] || fail 'GitHub did not return a release tag.'
    tag=${release_url##*/}
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Unsupported release tag: $tag"
    version=${tag#v}
    if [[ -e "$ZIRQ_INSTALL_PREVIOUS_DEST" || -L "$ZIRQ_INSTALL_PREVIOUS_DEST" ]]; then
        check_app "$ZIRQ_INSTALL_PREVIOUS_DEST"
        installed=$(plist "$ZIRQ_INSTALL_PREVIOUS_DEST" CFBundleShortVersionString)
        [[ "$installed" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "Unrecognized installed version: $installed"
        if version_at_least "$installed" "$version"; then
            if [[ "$installed" != "$version" ]] || codesign --verify --deep --strict "$ZIRQ_INSTALL_PREVIOUS_DEST" 2>/dev/null; then
                if [[ "$ZIRQ_INSTALL_PREVIOUS_DEST" != "$ZIRQ_INSTALL_DEST" ]]; then
                    not_running
                    mv "$ZIRQ_INSTALL_PREVIOUS_DEST" "$ZIRQ_INSTALL_DEST"
                fi
                printf 'Zirq %s is already installed at %s.\n' "$installed" "$ZIRQ_INSTALL_DEST"
                return
            fi
        fi
    fi
    not_running
    ZIRQ_INSTALL_WORK=$(mktemp -d "$app_dir/.zirq-install.XXXXXX")
    printf 'Downloading Zirq %s…\n' "$version"
    download --output "$ZIRQ_INSTALL_WORK/SHA256SUMS" "$repository/releases/download/$tag/SHA256SUMS"
    product=$(awk '
        $2 == "Zirq-arm64.zip" { zirq++ }
        $2 == "Kwi-arm64.zip" { kwi++ }
        END { if (zirq == 1 && kwi == 0) print "Zirq"; else if (kwi == 1 && zirq == 0) print "Kwi"; else exit 1 }
    ' "$ZIRQ_INSTALL_WORK/SHA256SUMS") || fail 'The release checksum is missing or ambiguous.'
    archive="$product-arm64.zip"
    download --output "$ZIRQ_INSTALL_WORK/$archive" "$repository/releases/download/$tag/$archive"
    checksum=$(awk -v asset="$archive" '$2 == asset && NF == 2 { count++; hash=$1 } END { if (count != 1) exit 1; print hash }' "$ZIRQ_INSTALL_WORK/SHA256SUMS") \
        || fail 'The release checksum is missing or ambiguous.'
    [[ "$checksum" =~ ^[0-9a-f]{64}$ ]] || fail 'The release checksum is invalid.'
    actual=$(shasum -a 256 "$ZIRQ_INSTALL_WORK/$archive")
    [[ "${actual%% *}" = "$checksum" ]] || fail 'Download checksum verification failed; the installed app was preserved.'
    zipinfo -1 "$ZIRQ_INSTALL_WORK/$archive" | awk -v root="$product.app" '
        /(^|\/)\.\.(\/|$)/ || /^\// { exit 1 }
        $0 != root && index($0, root "/") != 1 && $0 != "__MACOSX" && index($0, "__MACOSX/") != 1 { exit 1 }
    ' || fail 'The release archive contains unexpected paths.'
    mkdir "$ZIRQ_INSTALL_WORK/extracted"
    ditto -x -k "$ZIRQ_INSTALL_WORK/$archive" "$ZIRQ_INSTALL_WORK/extracted"
    staged="$ZIRQ_INSTALL_WORK/extracted/$product.app"
    check_app "$staged"
    identity="$(plist "$staged" CFBundleIdentifier)/$(plist "$staged" CFBundleExecutable)"
    [[ ( "$product" = Zirq && "$identity" = com.elashri.zirq/zirq ) || ( "$product" = Kwi && "$identity" = org.kde.kwi/kwi ) ]] \
        || fail 'Release bundle identity does not match its archive.'
    [[ -x "$staged/Contents/MacOS/$(plist "$staged" CFBundleExecutable)" ]] || fail 'The release executable is missing or not executable.'
    [[ "$(plist "$staged" CFBundleShortVersionString)" = "$version" ]] || fail 'The app version does not match the release.'
    minimum=$(plist "$staged" LSMinimumSystemVersion)
    system_version=$(sw_vers -productVersion)
    [[ "$minimum" =~ ^[0-9]+(\.[0-9]+){0,2}$ && "$system_version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] \
        || fail 'Cannot determine the required macOS version.'
    version_at_least "$system_version" "$minimum" || fail "This release requires macOS $minimum or later."
    codesign --verify --deep --strict "$staged" || fail 'App signature verification failed; the installed app was preserved.'
    not_running
    if [[ -e "$ZIRQ_INSTALL_PREVIOUS_DEST" ]]; then
        ZIRQ_INSTALL_BACKUP="$ZIRQ_INSTALL_WORK/previous.app"
        mv "$ZIRQ_INSTALL_PREVIOUS_DEST" "$ZIRQ_INSTALL_BACKUP"
    fi
    mv "$staged" "$ZIRQ_INSTALL_DEST"
    ZIRQ_INSTALL_COMMITTED=1
    printf 'Installed Zirq %s at %s.\nRun this installer again to update.\n' "$version" "$ZIRQ_INSTALL_DEST"
}

# An incomplete final block cannot execute if a piped download is truncated.
{
    main "$@"
}
