#!/bin/bash
# Stage a complete, bootable x86_64 kernel tree (boot/vmlinuz-<rel>,
# boot/initramfs-<rel>.img, usr/lib/modules/<rel>) from the Void kernel package
# saltOS images ship, without building an image: bootstrap the same Void rootfs
# os/build/vm-x86.sh starts from, install the kernel, run dracut with the same
# virtio/btrfs driver set, and copy the result out. The output is what
# build-base-grains.sh's KERNEL_TREE/KERNEL_RELEASE expects, so
#
#   sudo OUT=./kernel sh os/ota/kernel-stage.sh
#   KERNEL_TREE=./kernel KERNEL_RELEASE=$(cat ./kernel/release) sh os/ota/ship.sh
#
# publishes the linux-saltos grain. Must run as root (chroot). Env: KERNEL_PKG
# (default linux), OUT (default ./kernel-stage), WORK (default $OUT.work).
set -euo pipefail

KERNEL_PKG="${KERNEL_PKG:-linux}"
OUT="${OUT:-$PWD/kernel-stage}"
WORK="${WORK:-$OUT.work}"

VOID_MIRROR="https://repo-default.voidlinux.org/live/current"
VOID_DATE="20250202"
VOID_ARCH=x86_64
VOID_ROOTFS_SHA256="3f48e6673ac5907a897d913c97eb96edbfb230162731b4016562c51b3b8f1876"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1" >&2; exit 1; }; }
need curl; need sha256sum; need tar; need chroot
[ "$(id -u)" = 0 ] || { echo "kernel-stage: run as root (chroot)" >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "kernel-stage: stages $VOID_ARCH kernels; run on x86_64" >&2; exit 1; }

ROOTFS="$WORK/rootfs"
DL="$WORK/dl"
rm -rf "$WORK" "$OUT"
mkdir -p "$ROOTFS" "$DL" "$OUT"

ROOTFS_TARBALL="void-${VOID_ARCH}-ROOTFS-${VOID_DATE}.tar.xz"
echo "==> fetching Void $VOID_ARCH rootfs"
curl -fsSL "$VOID_MIRROR/$ROOTFS_TARBALL" -o "$DL/$ROOTFS_TARBALL"
echo "$VOID_ROOTFS_SHA256  $DL/$ROOTFS_TARBALL" | sha256sum -c -
tar -xpf "$DL/$ROOTFS_TARBALL" -C "$ROOTFS"

cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
mount --bind /dev "$ROOTFS/dev"
mount --bind /dev/pts "$ROOTFS/dev/pts" 2>/dev/null || true
mount -t proc proc "$ROOTFS/proc"
mount -t sysfs sys "$ROOTFS/sys"
cleanup() { umount -R "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

inchroot() { chroot "$ROOTFS" /usr/bin/env XBPS_ARCH="$VOID_ARCH" /bin/sh -c "$1"; }

echo "==> installing $KERNEL_PKG"
inchroot "xbps-install -Suy xbps"
inchroot "xbps-install -Suy"
mkdir -p "$ROOTFS/etc/dracut.conf.d"
cat > "$ROOTFS/etc/dracut.conf.d/10-saltos-virtio.conf" <<'EOF'
hostonly="no"
add_drivers+=" virtio_pci virtio_blk virtio_net virtio_console virtio_gpu virtio_input ahci sd_mod sr_mod ata_piix nvme btrfs "
filesystems+=" btrfs "
EOF
inchroot "xbps-install -Sy $KERNEL_PKG dracut btrfs-progs"
KVER="$(ls -1 "$ROOTFS/usr/lib/modules" | sort -V | tail -n1)"
[ -n "$KVER" ] || { echo "$KERNEL_PKG installed no kernel" >&2; exit 1; }
inchroot "dracut --force --no-hostonly /boot/initramfs-$KVER.img $KVER"

mkdir -p "$OUT/boot" "$OUT/usr/lib/modules"
cp -a "$ROOTFS/boot/vmlinuz-$KVER" "$ROOTFS/boot/initramfs-$KVER.img" "$OUT/boot/"
cp -a "$ROOTFS/usr/lib/modules/$KVER" "$OUT/usr/lib/modules/"
echo "$KVER" > "$OUT/release"
cleanup
rm -rf "$WORK"
echo "staged kernel $KVER in $OUT"
