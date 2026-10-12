#!/bin/sh
# Runs INSIDE the container (called by build.sh). Builds libusb and OpenOCD, both static.
set -eu

LIBUSB_VER=1.0.26
LIBUSB_URL=https://github.com/libusb/libusb/releases/download/v${LIBUSB_VER}/libusb-${LIBUSB_VER}.tar.bz2
OPENOCD_TAG=v0.12.0
OPENOCD_URL=https://downloads.sourceforge.net/project/openocd/openocd/0.12.0/openocd-0.12.0.tar.bz2
PREFIX=/work/prefix
OUT=/out

mkdir -p /work/src "$PREFIX" "$OUT"
cd /work/src

# /work is a Docker volume, so a rerun after a failure reuses the sources and objects.
if [ ! -f "$PREFIX/lib/libusb-1.0.a" ]; then
    curl -fsSL -o libusb.tar.bz2 "$LIBUSB_URL"
    tar xjf libusb.tar.bz2
    # --disable-udev: libusb finds devices through netlink, so no libudev (static link).
    ( cd libusb-$LIBUSB_VER && ./configure --prefix="$PREFIX" --enable-static --disable-shared \
        --disable-udev && make -j"$(nproc)" && make install )
fi

if [ ! -d openocd-0.12.0 ]; then
    curl -fsSL -o openocd.tar.bz2 "$OPENOCD_URL"
    tar xjf openocd.tar.bz2
fi
cd openocd-0.12.0
# -all-static is a libtool flag: given at make time only (gcc in configure rejects it),
# and plain -static leaves the executable linked to libc.so.
# Only the ST-LINK adapter (--enable-stlink) and the targets that ship with every build.
# Every other adapter is off by default or disabled here.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
[ -f Makefile ] || ./configure --prefix=/opt/openocd \
    --disable-shared --enable-static \
    --enable-stlink --disable-werror \
    --disable-internal-libjaylink \
    --disable-doxygen-html --disable-doxygen-pdf \
    LIBUSB1_CFLAGS="-I$PREFIX/include/libusb-1.0" \
    LIBUSB1_LIBS="-L$PREFIX/lib -lusb-1.0 -lpthread"
make -j"$(nproc)"
# Relink only the openocd executable fully static (jimtcl's own link rejects -all-static).
rm -f src/openocd
make src/openocd LDFLAGS="-all-static"
make DESTDIR=/work/stage install

rm -rf "$OUT/openocd" "$OUT/tcl"
cp /work/stage/opt/openocd/bin/openocd "$OUT/openocd"
cp -r /work/stage/opt/openocd/share/openocd/scripts "$OUT/tcl"
strip "$OUT/openocd"
file "$OUT/openocd" | tee "$OUT/openocd.file.txt"
echo "libusb $LIBUSB_VER; OpenOCD $OPENOCD_TAG (tag object 5cc204e035530d806f308be5ac22552fdb298a02, commit 9ea7f3d647c8ecf6b0f1424002dfc3f4504a162c)" > "$OUT/VERSIONS.txt"
