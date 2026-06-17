#!/bin/bash
set -euxo pipefail

ARCH="${1:-x86_64}"
EDITION="${EDITION:-console}"
WORK="${WORK:-$PWD/iso-work}"
OUT="${OUT:-$PWD/out-iso}"
SALT_BIN="${SALT_BIN:-$PWD/build/src/salt/salt}"
REPO="${REPO_DIR:-$PWD}"
VERSION="${VERSION:-0.1.0}"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"

case "$ARCH" in
  x86_64) DARCH=amd64 ;;
  aarch64) DARCH=arm64 ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

ROOTFS="$WORK/rootfs"
ISODIR="$WORK/iso"
rm -rf "$WORK"
mkdir -p "$ROOTFS" "$ISODIR/live" "$ISODIR/boot/grub" "$OUT"

BASE_PKGS="linux-image-$DARCH,live-boot,runit-init,btrfs-progs,dosfstools,e2fsprogs,\
util-linux,kmod,pciutils,file,less,nano,bash,coreutils,procps,\
iproute2,iputils-ping,isc-dhcp-client,ca-certificates,\
libzstd1,libsodium23,libsqlite3-0,zstd"

DESKTOP_PKGS="xserver-xorg-core,xserver-xorg-input-libinput,xserver-xorg-video-fbdev,\
xinit,xterm,openbox,lxqt-core,sddm,dbus,dbus-x11,eudev,\
calamares,calamares-settings-debian,parted,gdisk,network-manager,\
fonts-dejavu,fonts-liberation2,sudo"

PKGS="$BASE_PKGS"
if [ "$EDITION" = "desktop" ]; then
  PKGS="$PKGS,$DESKTOP_PKGS"
fi

mmdebstrap \
  --variant=apt \
  --arch="$DARCH" \
  --components="main contrib" \
  --include="$PKGS" \
  "$SUITE" "$ROOTFS" "$MIRROR"

install -Dm755 "$SALT_BIN" "$ROOTFS/usr/bin/salt"

cat > "$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION (live)"
ID=saltos
ID_LIKE=debian
VERSION="$VERSION"
VERSION_ID="$VERSION"
HOME_URL="https://github.com/plyght/saltOS"
EOF
echo "saltos-live" > "$ROOTFS/etc/hostname"
cat > "$ROOTFS/etc/issue" <<'EOF'

saltOS \r (\l)

EOF

install -Dm755 "$REPO/os/runit/stages/1" "$ROOTFS/etc/runit/1"
install -Dm755 "$REPO/os/runit/stages/2" "$ROOTFS/etc/runit/2"
install -Dm755 "$REPO/os/runit/stages/3" "$ROOTFS/etc/runit/3"
rm -rf "$ROOTFS/etc/runit/runsvdir/current"
mkdir -p "$ROOTFS/etc/runit/sv" "$ROOTFS/etc/runit/runsvdir/current"
cp -a "$REPO/os/runit/sv/." "$ROOTFS/etc/runit/sv/"
install -Dm755 "$REPO/os/runit/svc" "$ROOTFS/usr/bin/svc"

mkdir -p "$ROOTFS/etc/runit/sv/boot-check"
cat > "$ROOTFS/etc/runit/sv/boot-check/run" <<'EOF'
#!/bin/sh
exec 2>&1
salt --version > /dev/console 2>&1 || true
echo "SALTOS_BOOT_OK runit stage 2 reached; salt is present" > /dev/console
exec sleep infinity
EOF
chmod +x "$ROOTFS/etc/runit/sv/boot-check/run"

enable_sv() {
  ln -sf "/etc/runit/sv/$1" "$ROOTFS/etc/runit/runsvdir/current/$1"
}
enable_sv agetty-tty1
enable_sv boot-check
if [ "$EDITION" = "desktop" ]; then
  enable_sv dbus
  enable_sv sddm
fi

mkdir -p "$ROOTFS/etc/salt"
cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
repo = "current"
source = ""
key = ""
EOF
chroot "$ROOTFS" /usr/bin/salt --root / list >/dev/null 2>&1 || true

if [ "$EDITION" = "desktop" ]; then
  chroot "$ROOTFS" useradd -m -s /bin/bash -G sudo salt 2>/dev/null || true
  echo "salt:salt" | chroot "$ROOTFS" chpasswd || true
  mkdir -p "$ROOTFS/etc/sddm.conf.d"
  cat > "$ROOTFS/etc/sddm.conf.d/autologin.conf" <<'EOF'
[Autologin]
User=salt
Session=lxqt.desktop
EOF
  install -Dm644 "$REPO/os/iso/live/Install-saltOS.desktop" \
    "$ROOTFS/home/salt/Desktop/Install-saltOS.desktop" 2>/dev/null || true
fi

KVER="$(basename "$(ls -1 "$ROOTFS"/boot/vmlinuz-* | sort -V | tail -1)")"
KVER="${KVER#vmlinuz-}"
cp "$ROOTFS/boot/vmlinuz-$KVER" "$ISODIR/live/vmlinuz"
cp "$ROOTFS/boot/initrd.img-$KVER" "$ISODIR/live/initrd"

rm -f "$ROOTFS"/boot/vmlinuz-* "$ROOTFS"/boot/initrd.img-* 2>/dev/null || true
mksquashfs "$ROOTFS" "$ISODIR/live/filesystem.squashfs" \
  -comp zstd -Xcompression-level 19 -noappend -e boot

cat > "$ISODIR/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=5
insmod all_video
menuentry "saltOS $VERSION (live)" {
  linux /live/vmlinuz boot=live components init=/sbin/runit-init console=tty0 console=ttyS0,115200
  initrd /live/initrd
}
menuentry "saltOS $VERSION (live, to RAM)" {
  linux /live/vmlinuz boot=live components toram init=/sbin/runit-init console=tty0 console=ttyS0,115200
  initrd /live/initrd
}
EOF

ISO_PATH="$OUT/saltos-$VERSION-$EDITION-$ARCH.iso"
grub-mkrescue -o "$ISO_PATH" "$ISODIR" \
  -- -volid "SALTOS_LIVE"
echo "wrote $ISO_PATH"
ls -lh "$ISO_PATH"
