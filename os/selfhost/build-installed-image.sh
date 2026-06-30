#!/bin/sh
# Build a bootable installed saltOS disk image from a selfhost rootfs + kernel.
# This is intentionally host-side for now: the tiny live ISO does not yet ship
# mkfs/grub-install/partitioning tools. The installed result boots with a real
# persistent root filesystem, not an initramfs-only live root.
set -eu

ARCH="${ARCH:-$(uname -m)}"
ROOTFS="${ROOTFS:?set ROOTFS to the assembled selfhost rootfs}"
KERNEL="${KERNEL:?set KERNEL to bzImage or Image}"
OUT="${OUT:-out-iso}"
VERSION="${VERSION:-0.1.0}"
SIZE_MB="${SIZE_MB:-4096}"
LABEL="${LABEL:-SALTOS_ROOT}"

case "$ARCH" in
  arm64) ARCH=aarch64 ;;
  amd64) ARCH=x86_64 ;;
esac

case "$ARCH" in
  aarch64)
    KERNEL_NAME=Image
    GRUB_TARGET=arm64-efi
    EFI_DIR=BOOT
    EFI_NAME=BOOTAA64.EFI
    ROOT_DEVICE=/dev/vda2
    CONSOLE_ARGS="console=tty0 fbcon=map:0"
    SERIAL_ARGS="console=ttyAMA0,115200 console=hvc0"
    ;;
  x86_64)
    KERNEL_NAME=bzImage
    GRUB_TARGET=x86_64-efi
    EFI_DIR=BOOT
    EFI_NAME=BOOTX64.EFI
    ROOT_DEVICE=/dev/vda2
    CONSOLE_ARGS="console=tty0 fbcon=map:0"
    SERIAL_ARGS="console=ttyS0,115200"
    ;;
  *)
    echo "unsupported ARCH=$ARCH" >&2
    exit 2
    ;;
esac

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required tool: $1" >&2
    exit 127
  }
}

need dd
need parted
need mkfs.vfat
need mkfs.ext4
need grub-install
need losetup
need mount
need rsync

mkdir -p "$OUT"
IMG="$OUT/saltos-$VERSION-selfhost-$ARCH-installed.img"
MNT="${MNT:-/mnt/saltos-installed}"
ESP_MNT="$MNT/boot/efi"

cleanup() {
  set +e
  mountpoint -q "$ESP_MNT" && umount "$ESP_MNT"
  mountpoint -q "$MNT" && umount "$MNT"
  [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null
}
trap cleanup EXIT INT TERM

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart ESP fat32 1MiB 257MiB
parted -s "$IMG" set 1 esp on
parted -s "$IMG" mkpart saltos ext4 257MiB 100%

LOOP="$(losetup --find --partscan --show "$IMG")"
sleep 1
ESP="${LOOP}p1"
ROOTP="${LOOP}p2"
[ -b "$ESP" ] || ESP="${LOOP}p1"
[ -b "$ROOTP" ] || ROOTP="${LOOP}p2"

mkfs.vfat -F32 -n SALTOS_EFI "$ESP" >/dev/null
mkfs.ext4 -F -L "$LABEL" "$ROOTP" >/dev/null

mkdir -p "$MNT" "$ESP_MNT"
mount "$ROOTP" "$MNT"
mkdir -p "$ESP_MNT"
mount "$ESP" "$ESP_MNT"

rsync -aHAX --numeric-ids \
  --exclude=/dev --exclude=/proc --exclude=/sys --exclude=/run --exclude=/tmp \
  --exclude=/mnt --exclude=/media \
  "$ROOTFS"/ "$MNT"/

mkdir -p "$MNT/boot" "$MNT/dev" "$MNT/proc" "$MNT/sys" "$MNT/run" "$MNT/tmp"
chmod 1777 "$MNT/tmp"
install -Dm644 "$KERNEL" "$MNT/boot/$KERNEL_NAME"
ln -sf /sbin/runit-init "$MNT/init"

cat > "$MNT/etc/fstab" <<EOF
LABEL=$LABEL / ext4 rw,noatime 0 1
LABEL=SALTOS_EFI /boot/efi vfat umask=0077 0 2
tmpfs /tmp tmpfs defaults,nosuid,nodev 0 0
EOF

mkdir -p "$MNT/boot/grub"
cat > "$MNT/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
menuentry "saltOS $VERSION (installed, $ARCH, graphical display)" {
  linux /boot/$KERNEL_NAME root=$ROOT_DEVICE rw rootwait init=/sbin/runit-init $CONSOLE_ARGS
}
menuentry "saltOS $VERSION (installed, $ARCH, serial console)" {
  linux /boot/$KERNEL_NAME root=$ROOT_DEVICE rw rootwait init=/sbin/runit-init $SERIAL_ARGS
}
EOF

grub-install \
  --target="$GRUB_TARGET" \
  --modules="part_msdos part_gpt fat ext2 search search_label normal linux" \
  --efi-directory="$ESP_MNT" \
  --boot-directory="$MNT/boot" \
  --removable \
  --no-nvram \
  --recheck

mkdir -p "$ESP_MNT/EFI/$EFI_DIR"
if [ -f "$ESP_MNT/EFI/$EFI_DIR/$EFI_NAME" ]; then
  :
else
  found="$(find "$ESP_MNT/EFI" -name "$EFI_NAME" -type f | head -1 || true)"
  [ -n "$found" ] && cp "$found" "$ESP_MNT/EFI/$EFI_DIR/$EFI_NAME"
fi

# Optional: register the base as OTA-updatable grains. When REGISTER_BASE_GRAINS=1
# (and SALT_BIN + OTA_URL are provided), build signed salt/saltos-base/kernel
# grains, install them into the image so the package DB tracks the base at its
# built version, and point repo.conf at the OTA source. Then `salt update` on the
# running system can replace the base in place. Off by default so the baked image
# is unaffected; the same grains can also be produced standalone with
# build-base-grains.sh and registered later.
if [ "${REGISTER_BASE_GRAINS:-0}" = 1 ] && [ -n "${SALT_BIN:-}" ] && [ -n "${OTA_URL:-}" ]; then
  echo "===== register base as OTA grains ====="
  GREPO="${GREPO:-$OUT/base-grains}"
  if [ -n "${OTA_KEY:-}" ]; then SEC="$OTA_KEY"; else
    "$SALT_BIN" keygen "$OUT/ota-keys" saltos-ota
    SEC="$(cat "$OUT"/ota-keys/*.sec | head -1)"
    echo "NOTE: generated OTA signing key at $OUT/ota-keys (keep the .sec safe)"
  fi
  PUBKEY="$(cat "$OUT"/ota-keys/*.pub 2>/dev/null | head -1 || echo "${OTA_PUBKEY:-}")"
  SALT="$SALT_BIN" SEC_KEY="$SEC" ARCH="$ARCH" VERSION="$VERSION" \
    OUT="$GREPO" WORK="$OUT/base-grains-work" ROOTFS="$MNT" KERNEL="$MNT/boot/$KERNEL_NAME" \
    sh "$(dirname "$0")/build-base-grains.sh"
  printf 'repo = "current"\nsource = "file://%s/%s"\nkey = "%s"\n' "$GREPO" "$ARCH" "$PUBKEY" \
    > "$MNT/etc/salt/repo.conf"
  "$SALT_BIN" --root "$MNT" sync || true
  "$SALT_BIN" --root "$MNT" --yes install salt saltos-base linux-saltos || true
  printf 'repo = "current"\nsource = "%s"\nkey = "%s"\n' "$OTA_URL" "$PUBKEY" \
    > "$MNT/etc/salt/repo.conf"
  echo "base registered as grains; OTA source set to $OTA_URL"
fi

sync
echo "wrote $IMG"
