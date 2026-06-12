#!/usr/bin/env bash
#
# rebuild_initramfs.sh — repack guests/linux/rootfs into initramfs.cpio.gz
#
# Run from the hypervisor1/ directory:
#     sudo bash rebuild_initramfs.sh
#
# sudo is used so existing device nodes (dev/console, dev/ttyAMA0, dev/null)
# and root:root ownership are preserved in the cpio. If you prefer not to
# use sudo, install 'fakeroot' and run:  fakeroot bash rebuild_initramfs.sh
#
set -e

ROOT="guests/linux/rootfs"
OUT="guests/linux/initramfs.cpio.gz"
CROSS="${CROSS:-aarch64-linux-gnu-}"

[ -d "$ROOT" ] || { echo "ERROR: $ROOT not found (run from hypervisor1/)"; exit 1; }
[ -f "$ROOT/bin/busybox" ] || { echo "ERROR: $ROOT/bin/busybox missing"; exit 1; }
[ -f "$ROOT/init" ] || { echo "ERROR: $ROOT/init missing"; exit 1; }

# --- Sanity check 1: busybox must be statically linked ---------------------
if "${CROSS}readelf" -l "$ROOT/bin/busybox" 2>/dev/null | grep -q INTERP; then
    echo "============================================================"
    echo " ERROR: $ROOT/bin/busybox is DYNAMICALLY linked (has PT_INTERP)."
    echo "        The initramfs has no /lib and no dynamic loader, so it"
    echo "        will fail to exec with ENOENT (-2)."
    echo ""
    echo " Fix: drop in a STATIC aarch64 busybox. For example:"
    echo "        cd /tmp && wget https://busybox.net/downloads/busybox-1.36.1.tar.bz2"
    echo "        tar xf busybox-1.36.1.tar.bz2 && cd busybox-1.36.1"
    echo "        make ARCH=arm64 CROSS_COMPILE=${CROSS} defconfig"
    echo "        # enable: Settings -> Build static binary (no shared libs)"
    echo "        sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config"
    echo "        make ARCH=arm64 CROSS_COMPILE=${CROSS} -j\$(nproc)"
    echo "        cp busybox \$OLDPWD/$ROOT/bin/busybox"
    echo "============================================================"
    exit 1
fi
echo "  ✓ busybox is statically linked"

# --- Sanity check 2: init must have LF (no CRLF) line endings --------------
if grep -lq $'\r' "$ROOT/init" 2>/dev/null; then
    echo "  ! $ROOT/init has CRLF line endings — converting to LF"
    sed -i 's/\r$//' "$ROOT/init"
fi
chmod 0755 "$ROOT/init"

# --- Make sure the console device node exists (kernel opens it for init) ---
# (devtmpfs later replaces /dev, but /dev/console must exist beforehand.)
[ -e "$ROOT/dev/console" ] || mknod -m 600 "$ROOT/dev/console" c 5 1
[ -e "$ROOT/dev/ttyAMA0" ] || mknod -m 666 "$ROOT/dev/ttyAMA0" c 204 64
[ -e "$ROOT/dev/null" ]    || mknod -m 666 "$ROOT/dev/null"    c 1 3

# --- Pack the cpio (newc format) and gzip ----------------------------------
( cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc --quiet ) | gzip -9 > "$OUT"

echo "  ✓ Wrote $OUT ($(stat -c%s "$OUT") bytes)"
echo ""
echo "Now: make run-with-guests"
