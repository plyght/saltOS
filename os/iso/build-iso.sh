#!/bin/sh
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALLER_DIR=$(CDPATH= cd -- "$SELF_DIR/../installer" && pwd)

ARCH=""
ROOTFS=""
REPO=""
OUT=""
VERSION="0.1"
ISO_LABEL="SALTOS"
KERNEL_VERSION=""

usage() {
	cat <<EOF
Usage: build-iso.sh --arch <x86_64|aarch64> --rootfs <dir> [options]

Required:
  --arch <arch>        target architecture: x86_64 or aarch64
  --rootfs <dir>       base rootfs produced by os/bootstrap

Options:
  --repo <dir>         signed package repository to bundle at /repo on the ISO
  --out <file>         output ISO path (default: saltos-<version>-<arch>.iso)
  --version <ver>      version string for boot menu (default: $VERSION)
  --label <label>      ISO volume label (default: $ISO_LABEL)
  --kernel <ver>       kernel modules version under <rootfs>/lib/modules
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--arch) ARCH="$2"; shift 2 ;;
		--rootfs) ROOTFS="$2"; shift 2 ;;
		--repo) REPO="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--version) VERSION="$2"; shift 2 ;;
		--label) ISO_LABEL="$2"; shift 2 ;;
		--kernel) KERNEL_VERSION="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

[ -n "$ARCH" ] || { echo "error: --arch is required" >&2; exit 2; }
[ -n "$ROOTFS" ] || { echo "error: --rootfs is required" >&2; exit 2; }

case "$ARCH" in
	x86_64)
		EFI_GRUB_FORMAT=x86_64-efi
		EFI_BOOT_NAME=BOOTX64.EFI
		;;
	aarch64)
		EFI_GRUB_FORMAT=arm64-efi
		EFI_BOOT_NAME=BOOTAA64.EFI
		;;
	*)
		echo "error: unsupported arch: $ARCH" >&2
		exit 2
		;;
esac

[ -d "$ROOTFS" ] || { echo "error: rootfs not found: $ROOTFS" >&2; exit 1; }

for tool in mksquashfs xorriso grub-mkstandalone grub-mkimage; do
	command -v "$tool" >/dev/null 2>&1 || { echo "error: missing tool: $tool" >&2; exit 1; }
done

[ -n "$OUT" ] || OUT="saltos-$VERSION-$ARCH.iso"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

ISO_ROOT="$WORK/iso"
LIVE_ROOT="$WORK/rootfs"
mkdir -p "$ISO_ROOT/live" "$ISO_ROOT/boot/grub" "$ISO_ROOT/EFI/BOOT"

echo "Copying base rootfs"
cp -a "$ROOTFS" "$LIVE_ROOT"

echo "Installing installer assets into live rootfs"
mkdir -p "$LIVE_ROOT/usr/share/calamares/branding" \
	"$LIVE_ROOT/usr/lib/calamares/modules" \
	"$LIVE_ROOT/etc/calamares" \
	"$LIVE_ROOT/usr/share/applications" \
	"$LIVE_ROOT/usr/lib/saltos" \
	"$LIVE_ROOT/etc/runit/sv/live-setup" \
	"$LIVE_ROOT/etc/runit/runsvdir/current"

cp -a "$INSTALLER_DIR/branding/saltos" "$LIVE_ROOT/usr/share/calamares/branding/saltos"
cp -a "$INSTALLER_DIR/modules/." "$LIVE_ROOT/usr/lib/calamares/modules/"
cp -a "$INSTALLER_DIR/settings.conf" "$LIVE_ROOT/etc/calamares/settings.conf"

cp -a "$SELF_DIR/live/Install-saltOS.desktop" "$LIVE_ROOT/usr/share/applications/Install-saltOS.desktop"
cp -a "$SELF_DIR/live/live-setup.sh" "$LIVE_ROOT/usr/lib/saltos/live-setup.sh"
chmod 0755 "$LIVE_ROOT/usr/lib/saltos/live-setup.sh"

cp -a "$SELF_DIR/live/sv/live-setup/run" "$LIVE_ROOT/etc/runit/sv/live-setup/run"
chmod 0755 "$LIVE_ROOT/etc/runit/sv/live-setup/run"

for svc in udevd dbus seatd socklog dhcpcd chronyd sddm agetty-tty1 live-setup; do
	if [ -d "$LIVE_ROOT/etc/runit/sv/$svc" ]; then
		ln -sf "/etc/runit/sv/$svc" "$LIVE_ROOT/etc/runit/runsvdir/current/$svc"
	fi
done

if [ -n "$REPO" ] && [ -d "$REPO" ]; then
	echo "Bundling repository"
	mkdir -p "$ISO_ROOT/repo"
	cp -a "$REPO/." "$ISO_ROOT/repo/"
fi

echo "Locating kernel and modules"
if [ -z "$KERNEL_VERSION" ]; then
	if [ -d "$LIVE_ROOT/lib/modules" ]; then
		KERNEL_VERSION=$(ls -1 "$LIVE_ROOT/lib/modules" | sort -V | tail -n1)
	fi
fi
[ -n "$KERNEL_VERSION" ] || { echo "error: could not determine kernel version; pass --kernel" >&2; exit 1; }

VMLINUZ=""
for cand in \
	"$LIVE_ROOT/boot/vmlinuz-$KERNEL_VERSION" \
	"$LIVE_ROOT/boot/vmlinuz" \
	"$LIVE_ROOT/lib/modules/$KERNEL_VERSION/vmlinuz"; do
	if [ -f "$cand" ]; then VMLINUZ="$cand"; break; fi
done
[ -n "$VMLINUZ" ] || { echo "error: kernel image not found for $KERNEL_VERSION" >&2; exit 1; }

cp -a "$VMLINUZ" "$ISO_ROOT/boot/vmlinuz-$ARCH"

echo "Building initramfs with live-init"
INITRAMFS_DIR="$WORK/initramfs"
mkdir -p "$INITRAMFS_DIR"/bin "$INITRAMFS_DIR"/proc "$INITRAMFS_DIR"/sys \
	"$INITRAMFS_DIR"/dev "$INITRAMFS_DIR"/run

if command -v dracut >/dev/null 2>&1; then
	dracut --force --no-hostonly \
		--kver "$KERNEL_VERSION" \
		--kmoddir "$LIVE_ROOT/lib/modules/$KERNEL_VERSION" \
		--add "dmsquash-live" \
		--filesystems "squashfs overlay iso9660 vfat" \
		"$ISO_ROOT/boot/initramfs-$ARCH.img"
else
	cp -a "$SELF_DIR/live/live-init" "$INITRAMFS_DIR/init"
	chmod 0755 "$INITRAMFS_DIR/init"
	for b in busybox sh mount umount switch_root blkid modprobe sleep cp mkdir cat udevd udevadm; do
		src=$(command -v "$b" 2>/dev/null || true)
		[ -n "$src" ] && cp -a "$src" "$INITRAMFS_DIR/bin/" 2>/dev/null || true
	done
	mkdir -p "$INITRAMFS_DIR/lib/modules/$KERNEL_VERSION"
	cp -a "$LIVE_ROOT/lib/modules/$KERNEL_VERSION/." \
		"$INITRAMFS_DIR/lib/modules/$KERNEL_VERSION/" 2>/dev/null || true
	( cd "$INITRAMFS_DIR" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) \
		> "$ISO_ROOT/boot/initramfs-$ARCH.img"
fi

echo "Creating squashfs"
mksquashfs "$LIVE_ROOT" "$ISO_ROOT/live/filesystem.squashfs" \
	-comp zstd -Xcompression-level 19 -noappend -no-progress

echo "Generating GRUB configuration"
sed -e "s/@@ISO_LABEL@@/$ISO_LABEL/g" \
	-e "s/@@ARCH@@/$ARCH/g" \
	-e "s/@@VERSION@@/$VERSION/g" \
	"$SELF_DIR/grub/grub.cfg.in" > "$ISO_ROOT/boot/grub/grub.cfg"

EARLY_CFG="$WORK/grub-early.cfg"
cat > "$EARLY_CFG" <<EOF
search --no-floppy --set=root --label $ISO_LABEL
set prefix=(\$root)/boot/grub
configfile (\$root)/boot/grub/grub.cfg
EOF

echo "Building GRUB EFI image ($EFI_GRUB_FORMAT)"
grub-mkstandalone \
	--format="$EFI_GRUB_FORMAT" \
	--output="$ISO_ROOT/EFI/BOOT/$EFI_BOOT_NAME" \
	--modules="part_gpt part_msdos fat iso9660 normal search search_label configfile linux all_video gfxterm png" \
	"boot/grub/grub.cfg=$EARLY_CFG"

EFI_IMG="$ISO_ROOT/boot/grub/efiboot.img"
EFI_IMG_DIR="$WORK/efiimg"
mkdir -p "$EFI_IMG_DIR/EFI/BOOT"
cp "$ISO_ROOT/EFI/BOOT/$EFI_BOOT_NAME" "$EFI_IMG_DIR/EFI/BOOT/$EFI_BOOT_NAME"
EFI_BLOCKS=$(du -sk "$EFI_IMG_DIR" | cut -f1)
EFI_BLOCKS=$((EFI_BLOCKS + 2048))
dd if=/dev/zero of="$EFI_IMG" bs=1024 count="$EFI_BLOCKS" status=none
mkfs.vfat -n SALTEFI "$EFI_IMG" >/dev/null
if command -v mmd >/dev/null 2>&1; then
	mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
	mcopy -i "$EFI_IMG" "$EFI_IMG_DIR/EFI/BOOT/$EFI_BOOT_NAME" "::/EFI/BOOT/$EFI_BOOT_NAME"
else
	MNT="$WORK/efimnt"
	mkdir -p "$MNT"
	mount -o loop "$EFI_IMG" "$MNT"
	mkdir -p "$MNT/EFI/BOOT"
	cp "$EFI_IMG_DIR/EFI/BOOT/$EFI_BOOT_NAME" "$MNT/EFI/BOOT/$EFI_BOOT_NAME"
	umount "$MNT"
fi

XORRISO_ARGS="-as mkisofs -iso-level 3 -full-iso9660-filenames -volid $ISO_LABEL -rational-rock"

if [ "$ARCH" = "x86_64" ]; then
	echo "Building BIOS GRUB core image (i386-pc)"
	BIOS_DIR="$ISO_ROOT/boot/grub/i386-pc"
	mkdir -p "$BIOS_DIR"
	grub-mkimage \
		--format=i386-pc \
		--output="$WORK/core.img" \
		--prefix="/boot/grub" \
		biosdisk iso9660 part_gpt part_msdos fat normal search search_label configfile linux

	if [ -f /usr/lib/grub/i386-pc/cdboot.img ]; then
		cat /usr/lib/grub/i386-pc/cdboot.img "$WORK/core.img" > "$BIOS_DIR/eltorito.img"
	else
		cp "$WORK/core.img" "$BIOS_DIR/eltorito.img"
	fi
	if [ -d /usr/lib/grub/i386-pc ]; then
		cp -a /usr/lib/grub/i386-pc/*.mod "$BIOS_DIR/" 2>/dev/null || true
	fi
	if [ -f /usr/lib/grub/i386-pc/boot_hybrid.img ]; then
		HYBRID="--grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img"
	else
		HYBRID=""
	fi
	xorriso $XORRISO_ARGS \
		$HYBRID \
		-eltorito-boot boot/grub/i386-pc/eltorito.img \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		-eltorito-alt-boot \
		-e boot/grub/efiboot.img -no-emul-boot \
		-isohybrid-gpt-basdat \
		-output "$OUT" \
		"$ISO_ROOT"
else
	xorriso $XORRISO_ARGS \
		-eltorito-alt-boot \
		-e boot/grub/efiboot.img -no-emul-boot \
		-isohybrid-gpt-basdat \
		-output "$OUT" \
		"$ISO_ROOT"
fi

echo "Created $OUT"
