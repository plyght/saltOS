#!/bin/bash
set -euo pipefail

ARCH="${1:-x86_64}"
EDITION="${EDITION:-installer}"
WORK="${WORK:-$PWD/void-work}"
OUT="${OUT:-$PWD/out-iso}"
VERSION="${VERSION:-0.1.0}"
REPO="${REPO_DIR:-$PWD}"
SALT_BIN="${SALT_BIN:-$PWD/build/src/salt/salt}"
SALTSETUP_BIN="${SALTSETUP_BIN:-$PWD/build/src/setup/salt-setup}"
ISO_LABEL="SALTOS"

VOID_DATE="20250202"
VOID_MIRROR="https://repo-default.voidlinux.org/live/current"

case "$ARCH" in
  x86_64)
    VOID_ARCH=x86_64
    VOID_ROOTFS_SHA256="3f48e6673ac5907a897d913c97eb96edbfb230162731b4016562c51b3b8f1876"
    EFI_GRUB_FORMAT=x86_64-efi
    EFI_BOOT_NAME=BOOTX64.EFI
    ;;
  aarch64)
    VOID_ARCH=aarch64
    VOID_ROOTFS_SHA256="01a30f17ae06d4d5b322cd579ca971bc479e02cc284ec1e5a4255bea6bac3ce6"
    EFI_GRUB_FORMAT=arm64-efi
    EFI_BOOT_NAME=BOOTAA64.EFI
    ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

ROOTFS_TARBALL="void-${VOID_ARCH}-ROOTFS-${VOID_DATE}.tar.xz"
ROOTFS_URL="$VOID_MIRROR/$ROOTFS_TARBALL"

[ -f "$SALT_BIN" ] || { echo "salt binary not found at $SALT_BIN" >&2; exit 1; }
[ -f "$SALTSETUP_BIN" ] || { echo "salt-setup binary not found at $SALTSETUP_BIN" >&2; exit 1; }

ROOTFS="$WORK/rootfs"
ISO_ROOT="$WORK/iso"
DL="$WORK/dl"
rm -rf "$WORK"
mkdir -p "$ROOTFS" "$ISO_ROOT/live" "$ISO_ROOT/boot/grub" "$ISO_ROOT/EFI/BOOT" "$DL" "$OUT"

echo "==> fetching Void rootfs $ROOTFS_TARBALL"
curl -fsSL "$ROOTFS_URL" -o "$DL/$ROOTFS_TARBALL"
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

echo "==> updating xbps and base"
inchroot "xbps-install -Suy xbps"
inchroot "xbps-install -Suy"

echo "==> installing nonfree repo and firmware"
inchroot "xbps-install -Sy void-repo-nonfree"
inchroot "xbps-install -Sy linux-firmware linux-firmware-network linux-firmware-intel linux-firmware-amd" || true

echo "==> installing base, boot, and installer tooling"
inchroot "xbps-install -Sy base-system linux dracut btrfs-progs dosfstools gptfdisk parted rsync squashfs-tools grub grub-${VOID_ARCH}-efi efibootmgr NetworkManager dbus elogind polkit seatd sudo"

if [ "$EDITION" = "desktop" ] || [ "$EDITION" = "installer" ]; then
  echo "==> installing desktop"
  inchroot "xbps-install -Sy xorg-minimal mesa-dri xf86-input-libinput lxqt sddm"
  echo "==> installing desktop apps and fonts (best-effort; lxqt pulls core apps)"
  for app in qterminal pcmanfm-qt featherpad dejavu-fonts-ttf; do
    inchroot "xbps-install -Sy $app" || true
  done
  echo "==> installing optional video drivers (best-effort; modesetting covers the rest)"
  for drv in xf86-video-intel xf86-video-amdgpu xf86-video-ati xf86-video-nouveau xf86-video-qxl xf86-video-fbdev xf86-video-vesa; do
    inchroot "xbps-install -Sy $drv" || true
  done
fi

echo "==> configuring locale and identity"
if [ -f "$ROOTFS/etc/default/libc-locales" ]; then
  sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' "$ROOTFS/etc/default/libc-locales"
  inchroot "xbps-reconfigure -f glibc-locales" || true
fi

cat > "$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION (Void base)"
ID=saltos
ID_LIKE=void
VERSION="$VERSION"
VERSION_ID="$VERSION"
HOME_URL="https://github.com/plyght/saltOS"
EOF
echo "saltos-live" > "$ROOTFS/etc/hostname"

echo "==> installing saltOS control plane"
install -Dm755 "$SALT_BIN" "$ROOTFS/usr/bin/salt"
install -Dm755 "$SALTSETUP_BIN" "$ROOTFS/usr/bin/salt-setup"
mkdir -p "$ROOTFS/etc/salt/strata" "$ROOTFS/usr/local/salt/shims" "$ROOTFS/strata"
cp "$REPO"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
install -Dm644 "$REPO/os/profile.d/salt-shims.sh" "$ROOTFS/etc/profile.d/salt-shims.sh"
cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
repo = "current"
source = ""
key = ""
EOF
cat > "$ROOTFS/etc/salt/salt.conf" <<'EOF'
[install]
auto_expose = "prompt"

[strata]
expose_pm = true
auto_service = true
EOF

echo "==> creating live user"
inchroot "useradd -m -G wheel,audio,video,input,network,storage,_seatd -s /bin/bash salt" 2>/dev/null || \
  inchroot "useradd -m -G wheel,audio,video,input,network,storage -s /bin/bash salt" || true
echo "salt:salt" | chroot "$ROOTFS" chpasswd || true
echo "root:root" | chroot "$ROOTFS" chpasswd || true
mkdir -p "$ROOTFS/etc/sudoers.d"
echo "salt ALL=(ALL) NOPASSWD: ALL" > "$ROOTFS/etc/sudoers.d/salt"
chmod 0440 "$ROOTFS/etc/sudoers.d/salt"

echo "==> enabling runit services (void convention)"
RUNDIR="$ROOTFS/etc/runit/runsvdir/default"
mkdir -p "$RUNDIR"
enable_sv() {
  if [ -d "$ROOTFS/etc/sv/$1" ]; then
    ln -sf "/etc/sv/$1" "$RUNDIR/$1"
  fi
}
for svc in udevd dbus NetworkManager polkitd seatd; do enable_sv "$svc"; done

mkdir -p "$ROOTFS/etc/sv/saltos-boot-check"
cat > "$ROOTFS/etc/sv/saltos-boot-check/run" <<'EOF'
#!/bin/sh
exec 2>&1
if salt --version > /dev/console 2>&1; then
  echo "SALTOS_BOOT_OK saltOS control plane runs on Void base" > /dev/console
else
  echo "SALTOS_BOOT_FAIL salt did not run" > /dev/console
fi
exec sleep infinity
EOF
chmod 0755 "$ROOTFS/etc/sv/saltos-boot-check/run"
enable_sv saltos-boot-check

if [ "${SALTOS_E2E:-0}" = "1" ]; then
  mkdir -p "$ROOTFS/etc/sv/saltos-pkg-e2e"
  cat > "$ROOTFS/etc/sv/saltos-pkg-e2e/run" <<'EOF'
#!/bin/sh
exec 2>&1
echo "SALTOS_PKG_E2E starting" > /dev/console
for i in $(seq 1 90); do
  getent hosts dl-cdn.alpinelinux.org >/dev/null 2>&1 && break
  sleep 2
done
if salt --yes stratum add alpine > /dev/console 2>&1 \
  && salt run alpine apk update > /dev/console 2>&1 \
  && salt pkg alpine install nano > /dev/console 2>&1 \
  && salt run alpine /usr/bin/nano --version > /dev/console 2>&1; then
  echo "SALTOS_PKG_E2E_OK installed and ran a foreign-distro alpine package via salt" > /dev/console
else
  echo "SALTOS_PKG_E2E_FAIL" > /dev/console
fi
exec sleep infinity
EOF
  chmod 0755 "$ROOTFS/etc/sv/saltos-pkg-e2e/run"
  enable_sv saltos-pkg-e2e
fi

if [ "$EDITION" = "desktop" ] || [ "$EDITION" = "installer" ]; then
  enable_sv sddm
  mkdir -p "$ROOTFS/etc/sddm.conf.d"
  cat > "$ROOTFS/etc/sddm.conf.d/autologin.conf" <<'EOF'
[Autologin]
User=salt
Session=lxqt

[General]
EOF
fi

if [ "$EDITION" = "installer" ]; then
  mkdir -p "$ROOTFS/home/salt/.config/autostart" "$ROOTFS/usr/local/bin"
  cat > "$ROOTFS/usr/local/bin/saltos-install" <<'EOF'
#!/bin/sh
exec qterminal -e "sudo salt-setup"
EOF
  chmod 0755 "$ROOTFS/usr/local/bin/saltos-install"
  cat > "$ROOTFS/usr/share/applications/saltos-install.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Install saltOS
Comment=Install saltOS and pick a base distribution
Exec=/usr/local/bin/saltos-install
Terminal=false
Icon=system-software-install
Categories=System;
EOF
  cp "$ROOTFS/usr/share/applications/saltos-install.desktop" \
    "$ROOTFS/home/salt/.config/autostart/saltos-install.desktop"
fi
chroot "$ROOTFS" chown -R salt:salt /home/salt 2>/dev/null || true

echo "==> locating kernel"
KVER="$(ls -1 "$ROOTFS/usr/lib/modules" 2>/dev/null | sort -V | tail -n1 || true)"
[ -n "$KVER" ] || KVER="$(ls -1 "$ROOTFS/lib/modules" 2>/dev/null | sort -V | tail -n1 || true)"
[ -n "$KVER" ] || { echo "no kernel modules found in void rootfs" >&2; exit 1; }

echo "==> building live initramfs in chroot (dracut dmsquash-live), kver=$KVER"
inchroot "dracut --force --no-hostonly --add dmsquash-live --filesystems 'squashfs overlay iso9660 vfat' /boot/initramfs-live.img $KVER"

VMLINUZ=""
for cand in "/boot/vmlinuz-$KVER" "/boot/vmlinuz"; do
  [ -f "$ROOTFS$cand" ] && VMLINUZ="$ROOTFS$cand" && break
done
[ -n "$VMLINUZ" ] || { echo "kernel image not found in void rootfs" >&2; exit 1; }
cp -a "$VMLINUZ" "$ISO_ROOT/boot/vmlinuz-$ARCH"
cp -a "$ROOTFS/boot/initramfs-live.img" "$ISO_ROOT/boot/initramfs-$ARCH.img"

echo "==> unmounting chroot pseudo-fs before squashfs"
umount -R "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" 2>/dev/null || true
trap - EXIT INT TERM
rm -f "$ROOTFS/etc/resolv.conf"

echo "==> creating squashfs"
mksquashfs "$ROOTFS" "$ISO_ROOT/live/filesystem.squashfs" \
  -comp zstd -Xcompression-level 19 -noappend -no-progress -e boot

echo "==> generating grub.cfg"
cat > "$ISO_ROOT/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=10
insmod all_video
insmod gfxterm
insmod part_gpt
insmod part_msdos
insmod iso9660
insmod search
insmod search_label
serial --speed=115200
terminal_input console serial
terminal_output console serial
menuentry "saltOS $VERSION (live, $ARCH)" {
  search --no-floppy --set=root --label $ISO_LABEL
  linux /boot/vmlinuz-$ARCH root=live:LABEL=$ISO_LABEL rd.live.image rd.live.dir=live rd.live.squashimg=filesystem.squashfs console=tty0 console=ttyS0,115200
  initrd /boot/initramfs-$ARCH.img
}
menuentry "saltOS $VERSION (live, $ARCH, safe graphics)" {
  search --no-floppy --set=root --label $ISO_LABEL
  linux /boot/vmlinuz-$ARCH root=live:LABEL=$ISO_LABEL rd.live.image rd.live.dir=live rd.live.squashimg=filesystem.squashfs nomodeset console=tty0 console=ttyS0,115200
  initrd /boot/initramfs-$ARCH.img
}
EOF

EARLY_CFG="$WORK/grub-early.cfg"
cat > "$EARLY_CFG" <<EOF
search --no-floppy --set=root --label $ISO_LABEL
set prefix=(\$root)/boot/grub
configfile (\$root)/boot/grub/grub.cfg
EOF

echo "==> building GRUB EFI image"
grub-mkstandalone \
  --format="$EFI_GRUB_FORMAT" \
  --output="$ISO_ROOT/EFI/BOOT/$EFI_BOOT_NAME" \
  --modules="part_gpt part_msdos fat iso9660 normal search search_label configfile linux all_video gfxterm serial" \
  "boot/grub/grub.cfg=$EARLY_CFG"

EFI_IMG="$ISO_ROOT/boot/grub/efiboot.img"
EFI_IMG_DIR="$WORK/efiimg"
mkdir -p "$EFI_IMG_DIR/EFI/BOOT"
cp "$ISO_ROOT/EFI/BOOT/$EFI_BOOT_NAME" "$EFI_IMG_DIR/EFI/BOOT/$EFI_BOOT_NAME"
EFI_BLOCKS=$(du -sk "$EFI_IMG_DIR" | cut -f1)
EFI_BLOCKS=$((EFI_BLOCKS + 2048))
dd if=/dev/zero of="$EFI_IMG" bs=1024 count="$EFI_BLOCKS" status=none
mkfs.vfat -n SALTEFI "$EFI_IMG" >/dev/null
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$EFI_IMG_DIR/EFI/BOOT/$EFI_BOOT_NAME" "::/EFI/BOOT/$EFI_BOOT_NAME"

XORRISO_ARGS="-as mkisofs -iso-level 3 -full-iso9660-filenames -volid $ISO_LABEL -rational-rock"
ISO_PATH="$OUT/saltos-$VERSION-void-$EDITION-$ARCH.iso"

if [ "$ARCH" = "x86_64" ]; then
  echo "==> building BIOS GRUB core"
  BIOS_DIR="$ISO_ROOT/boot/grub/i386-pc"
  mkdir -p "$BIOS_DIR"
  grub-mkimage \
    --format=i386-pc \
    --output="$WORK/core.img" \
    --prefix="/boot/grub" \
    biosdisk iso9660 part_gpt part_msdos fat normal search search_label configfile linux serial
  if [ -f /usr/lib/grub/i386-pc/cdboot.img ]; then
    cat /usr/lib/grub/i386-pc/cdboot.img "$WORK/core.img" > "$BIOS_DIR/eltorito.img"
  else
    cp "$WORK/core.img" "$BIOS_DIR/eltorito.img"
  fi
  [ -d /usr/lib/grub/i386-pc ] && cp -a /usr/lib/grub/i386-pc/*.mod "$BIOS_DIR/" 2>/dev/null || true
  HYBRID=""
  [ -f /usr/lib/grub/i386-pc/boot_hybrid.img ] && \
    HYBRID="--grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img"
  xorriso $XORRISO_ARGS \
    -eltorito-boot boot/grub/i386-pc/eltorito.img \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    $HYBRID \
    -eltorito-alt-boot \
    -e boot/grub/efiboot.img -no-emul-boot \
    -append_partition 2 0xef "$ISO_ROOT/boot/grub/efiboot.img" \
    -o "$ISO_PATH" "$ISO_ROOT"
else
  xorriso $XORRISO_ARGS \
    -eltorito-alt-boot \
    -e boot/grub/efiboot.img -no-emul-boot \
    -append_partition 2 0xef "$ISO_ROOT/boot/grub/efiboot.img" \
    -o "$ISO_PATH" "$ISO_ROOT"
fi

echo "wrote $ISO_PATH"
