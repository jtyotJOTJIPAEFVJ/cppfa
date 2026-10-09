#!/bin/bash
# Sets up a pyfa Python environment for WSL/Linux in .venv (uv), with the wxPython binary
# wheel, and the shared libraries that wheel needs but newer Ubuntus lack, extracted
# without root into .venv/syslib (the wheel is built for Ubuntu 22.04: libtiff5 etc.).
# Run python through scripts/wsl-python.sh, which puts .venv/syslib on LD_LIBRARY_PATH.
# Re-running is safe: it only adds what is missing.
set -euo pipefail
shopt -s nullglob
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
uv sync --frozen --no-group wx-source --group wx-binary --no-group dev

LIBDIR="$ROOT/.venv/syslib"
DEBS="$ROOT/.venv/syslib-debs"
mkdir -p "$LIBDIR" "$DEBS"
missing() {
    LD_LIBRARY_PATH="$LIBDIR/usr/lib/x86_64-linux-gnu:$LIBDIR/usr/lib/x86_64-linux-gnu/pulseaudio" \
        ldd .venv/lib/python3*/site-packages/wx/_core*.so "$LIBDIR"/usr/lib/x86_64-linux-gnu/*.so* \
        "$LIBDIR"/usr/lib/x86_64-linux-gnu/pulseaudio/*.so* 2>/dev/null | awk '/not found/{print $1}' | sort -u || true
}
# Library file -> Debian packages to try (current names first, then older ones).
package_for() {
    case "$1" in
        libSDL2-2.0.so.0) echo "libsdl2-2.0-0 libsdl2-classic" ;;
        libasound.so.2) echo "libasound2t64 libasound2" ;;
        libpulse.so.0|libpulsecommon-*) echo "libpulse0" ;;
        libsamplerate.so.0) echo "libsamplerate0" ;;
        libdecor-0.so.0) echo "libdecor-0-0" ;;
        libsndfile.so.1) echo "libsndfile1" ;;
        libasyncns.so.0) echo "libasyncns0" ;;
        libFLAC.so.*) echo "libflac14 libflac12 libflac8" ;;
        libvorbisenc.so.2) echo "libvorbisenc2" ;;
        libvorbis.so.0) echo "libvorbis0a" ;;
        libogg.so.0) echo "libogg0" ;;
        libopus.so.0) echo "libopus0" ;;
        libmpg123.so.0) echo "libmpg123-0t64 libmpg123-0" ;;
        libmp3lame.so.0) echo "libmp3lame0" ;;
        libtiff.so.5) echo "libtiff5 URL:http://archive.ubuntu.com/ubuntu/pool/main/t/tiff/libtiff5_4.3.0-6_amd64.deb" ;;
        *) echo "" ;;
    esac
}
for round in 1 2 3 4 5; do
    libs=$(missing)
    [ -z "$libs" ] && break
    for lib in $libs; do
        got=0
        for pkg in $(package_for "$lib"); do
            if [[ $pkg == URL:* ]]; then
                (cd "$DEBS" && curl -sSfLO "${pkg#URL:}") && got=1
            else
                (cd "$DEBS" && apt-get download "$pkg" >/dev/null 2>&1) && got=1
            fi
            [ $got = 1 ] && [ "$lib" != libSDL2-2.0.so.0 ] && break
        done
        [ $got = 0 ] && echo "warning: no package found for $lib" >&2
    done
    for deb in "$DEBS"/*.deb; do dpkg -x "$deb" "$LIBDIR"; done
done
libs=$(missing)
if [ -n "$libs" ]; then
    echo "still missing: $libs" >&2
    exit 1
fi
"$ROOT/scripts/wsl-python.sh" -c "import wx; print('wxPython', wx.version())"
