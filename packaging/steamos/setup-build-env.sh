#!/usr/bin/env bash
# Vita3K-Deck: restore the build toolchain after a fresh SteamOS install, or after
# a SteamOS system update wipes it.
#
# SteamOS's atomic image updates can silently drop files belonging to packages that
# were manually installed on top of the base image, while leaving pacman's local
# database entry behind - so `pacman -Q` still reports a package as installed even
# though its actual files (dev headers especially) are gone from disk. Plain
# `pacman -S` then does nothing, since pacman believes there's nothing to do.
# `--overwrite '*'` forces every file to actually be re-laid down regardless of what
# the database currently believes, and `-Sy` refreshes the sync databases first so
# a stale/wiped local state doesn't fool it into skipping a "known-installed" package.
#
# Run this once after a fresh SteamOS install, and again any time a system update
# breaks the Vita3K build (a missing cmake, or a "features.h not found" compile
# error, are the usual symptoms).
#
# This only covers what VITA3K_STEAMOS_NATIVE needs - the Qt desktop build pulls in
# Qt6 separately and isn't handled here.

set -euo pipefail

if [[ $EUID -eq 0 ]]; then
    echo "Run this as your normal user, not root - it calls sudo itself where needed." >&2
    exit 1
fi

echo "==> Disabling SteamOS's read-only root filesystem"
sudo steamos-readonly disable

echo "==> Initializing the pacman keyring"
# A fresh SteamOS install (or a wiped /etc/pacman.d/gnupg) leaves package installs
# failing on signature verification until the keyring is (re)populated.
sudo pacman-key --init
sudo pacman-key --populate archlinux
# "holo" is SteamOS's own keyring; older/non-standard SteamOS images may not ship
# it, so don't treat its absence as fatal.
sudo pacman-key --populate holo || echo "    (no 'holo' keyring on this image - skipping, that's fine)"

PACKAGES=(
    # toolchain
    cmake ninja clang ccache
    # C library / headers
    glibc linux-api-headers
    # X11 / Wayland windowing
    libx11 libxext libxau libxdmcp libxcb wayland xorgproto
    libxrandr libxfixes libxi libxcursor libxinerama libxrender libxss libxkbfile libxtst
    xcb-util xcb-util-wm xcb-util-keysyms xcb-util-cursor xcb-util-image xcb-util-renderutil xcb-util-errors
    # crypto / networking
    openssl curl libidn2 libunistring libffi zlib
    # audio backends (cubeb builds both in)
    libpulse alsa-lib
    # SDL's Linux video/input backends
    dbus libdecor libusb systemd-libs libxkbcommon libdrm mesa libglvnd pipewire ibus
    # misc build-time tools
    pkgconf python git
)

echo "==> Force-reinstalling: ${PACKAGES[*]}"
sudo pacman -Sy --noconfirm --overwrite '*' "${PACKAGES[@]}"

echo "==> Verifying"
missing=0

check_bin() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "    MISSING binary: $1"
        missing=1
    fi
}
check_header() {
    if [[ ! -f "$1" ]]; then
        echo "    MISSING header: $1"
        missing=1
    fi
}

for bin in cmake ninja clang clang++ ccache pkg-config git python3; do
    check_bin "$bin"
done
for hdr in /usr/include/features.h /usr/include/X11/Xlib.h /usr/include/wayland-client.h \
    /usr/include/openssl/ssl.h /usr/include/curl/curl.h /usr/include/pulse/pulseaudio.h \
    /usr/include/alsa/asoundlib.h /usr/include/dbus-1.0/dbus/dbus.h \
    /usr/include/libdecor-0/libdecor.h /usr/include/libusb-1.0/libusb.h; do
    check_header "$hdr"
done

if [[ $missing -eq 0 ]]; then
    echo "==> All good - you can build Vita3K-Deck now."
else
    echo "==> Some files are still missing (see above). Try rebooting and re-running this script."
    exit 1
fi
