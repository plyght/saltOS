#!/bin/bash
# Build a persistent x86_64 saltOS disk image for generic UEFI VMs (QEMU/KVM,
# UTM's QEMU backend, VMware, VirtualBox, Parallels) and bare-metal UEFI PCs.
#
# Output is a raw GPT disk image (ESP + btrfs root) booting via standalone
# x86_64-efi GRUB at /EFI/BOOT/BOOTX64.EFI. Serial console is ttyS0; the
# initramfs carries virtio + AHCI/SATA + NVMe drivers so it boots regardless of
# the hypervisor's disk emulation. This is the Intel-host parallel of vm-apple.sh.
set -euo pipefail

ARCH=x86_64
EDITION="${EDITION:-desktop}"        # console | desktop
WORK="${WORK:-$PWD/vm-work}"
OUT="${OUT:-$PWD/out-vm}"
VERSION="${VERSION:-0.1.0}"
REPO="${REPO_DIR:-$PWD}"
SALT_BIN="${SALT_BIN:-$PWD/build/src/salt/salt}"
SALTSETUP_BIN="${SALTSETUP_BIN:-$PWD/build/src/setup/salt-setup}"
IMG_SIZE_MB="${IMG_SIZE_MB:-12288}"
ESP_SIZE_MB="${ESP_SIZE_MB:-256}"
HOSTNAME="${SALTOS_HOSTNAME:-saltos-vm}"

VOID_MIRROR="https://repo-default.voidlinux.org/live/current"
VOID_DATE="20250202"
VOID_ARCH=x86_64
VOID_ROOTFS_SHA256="3f48e6673ac5907a897d913c97eb96edbfb230162731b4016562c51b3b8f1876"
ROOT_LABEL=saltos-root
EFI_BOOT_NAME=BOOTX64.EFI

# shellcheck source=os/build/common.sh
. "$REPO/os/build/common.sh"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1" >&2; exit 1; }; }
need curl; need sha256sum; need tar; need sgdisk; need mkfs.vfat
need mkfs.btrfs; need losetup; need chroot; need grub-mkstandalone

[ "$(uname -m)" = "aarch64" ] || \
  echo "warning: building an aarch64 image on $(uname -m); ensure binfmt/qemu-user-static is configured" >&2

[ -f "$SALT_BIN" ] || { echo "salt binary not found at $SALT_BIN" >&2; exit 1; }

ROOTFS="$WORK/rootfs"
DL="$WORK/dl"
rm -rf "$WORK"
mkdir -p "$ROOTFS" "$DL" "$OUT"

ROOTFS_TARBALL="void-${VOID_ARCH}-ROOTFS-${VOID_DATE}.tar.xz"
echo "==> fetching Void aarch64 rootfs"
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

echo "==> updating xbps and base"
inchroot "xbps-install -Suy xbps"
inchroot "xbps-install -Suy"
inchroot "xbps-install -Sy void-repo-nonfree" || true
inchroot "xbps-install -Sy linux-firmware-network" || true

echo "==> installing base, boot, and virtio-aware tooling"
inchroot "xbps-install -Sy base-system linux dracut btrfs-progs dosfstools gptfdisk parted \
  rsync curl ca-certificates grub grub-x86_64-efi efibootmgr \
  NetworkManager dbus elogind polkit seatd sudo chrony openssh \
  zstd xz bzip2 tar debootstrap"

if [ "$EDITION" = "desktop" ]; then
  echo "==> installing desktop (virtio-gpu via modesetting)"
  inchroot "xbps-install -Sy xorg-minimal mesa-dri xf86-input-libinput xf86-video-fbdev lxqt sddm" || true
  for app in qterminal pcmanfm-qt featherpad dejavu-fonts-ttf; do
    inchroot "xbps-install -Sy $app" || true
  done
fi

echo "==> configuring locale and identity"
if [ -f "$ROOTFS/etc/default/libc-locales" ]; then
  sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' "$ROOTFS/etc/default/libc-locales"
  inchroot "xbps-reconfigure -f glibc-locales" || true
fi

saltos_write_os_release "generic x86_64 UEFI VM"
echo "$HOSTNAME" > "$ROOTFS/etc/hostname"
cat > "$ROOTFS/etc/hosts" <<EOF
127.0.0.1   localhost
127.0.1.1   $HOSTNAME
::1         localhost ip6-localhost ip6-loopback
EOF

echo "==> installing saltOS control plane (expose-by-default + escalation)"
saltos_install_controlplane
saltos_write_config

echo "==> creating user 'salt' (passwordless sudo)"
inchroot "useradd -m -G wheel,audio,video,input,network,storage,_seatd -s /bin/bash salt" 2>/dev/null || \
  inchroot "useradd -m -G wheel,audio,video,input,network,storage -s /bin/bash salt" || true
echo "salt:salt" | chroot "$ROOTFS" chpasswd || true
echo "root:root" | chroot "$ROOTFS" chpasswd || true
saltos_write_sudoers salt

cat > "$ROOTFS/etc/motd" <<'EOF'

  saltOS on Apple Virtualization / UTM (aarch64) -- you are 'salt'.

  Console is virtio (hvc0). Graphics are virtio-gpu.
  salt --help            native package manager + strata
  salt-setup             pick a base distro / install to disk

EOF

echo "==> enabling runit services (Void native /etc/sv)"
RUNDIR="$ROOTFS/etc/runit/runsvdir/default"
mkdir -p "$RUNDIR"
enable_sv() { [ -d "$ROOTFS/etc/sv/$1" ] && ln -sf "/etc/sv/$1" "$RUNDIR/$1"; }
for svc in udevd dbus NetworkManager polkitd seatd sshd chronyd; do enable_sv "$svc"; done

# Login on the virtio console (hvc0) -- this is the serial console Apple's
# framework presents. Without it `Open serial` in UTM shows a dead terminal.
mkdir -p "$ROOTFS/etc/sv/agetty-ttyS0"
cat > "$ROOTFS/etc/sv/agetty-ttyS0/run" <<'EOF'
#!/bin/sh
exec 2>&1
exec setsid -w agetty --noclear --autologin salt ttyS0 115200 linux
EOF
chmod 0755 "$ROOTFS/etc/sv/agetty-ttyS0/run"
enable_sv agetty-ttyS0
# Keep a normal text console on the virtio-gpu framebuffer too (Void ships this).
enable_sv agetty-tty1

if [ "$EDITION" = "desktop" ]; then
  enable_sv sddm
  mkdir -p "$ROOTFS/etc/sddm.conf.d"
  cat > "$ROOTFS/etc/sddm.conf.d/autologin.conf" <<'EOF'
[Autologin]
User=salt
Session=lxqt

[General]
EOF
fi
chroot "$ROOTFS" chown -R salt:salt /home/salt 2>/dev/null || true

echo "==> detaching runit supervisor from the console (no service flood)"
saltos_quiet_runit_console

# Make sure the virtio modules land in the generic (non-hostonly) initramfs so
# the image boots on Apple's framework regardless of the build host.
mkdir -p "$ROOTFS/etc/dracut.conf.d"
cat > "$ROOTFS/etc/dracut.conf.d/10-saltos-virtio.conf" <<'EOF'
hostonly="no"
add_drivers+=" virtio_pci virtio_blk virtio_net virtio_console virtio_gpu virtio_input ahci sd_mod sr_mod ata_piix nvme btrfs "
filesystems+=" btrfs "
EOF

echo "==> locating kernel"
KVER="$(ls -1 "$ROOTFS/usr/lib/modules" 2>/dev/null | sort -V | tail -n1 || true)"
[ -n "$KVER" ] || { echo "no kernel modules found" >&2; exit 1; }

echo "==> building initramfs (kver=$KVER)"
inchroot "dracut --force --no-hostonly /boot/initramfs-$KVER.img $KVER"

VMLINUZ=""
# Void names the aarch64 kernel image "vmlinux-<ver>" (a raw ARM64 boot Image,
# which GRUB's arm64-efi linux loader boots directly); other bases use vmlinuz.
for cand in "/boot/vmlinuz-$KVER" "/boot/vmlinux-$KVER" "/boot/Image-$KVER" \
            "/boot/vmlinuz" "/boot/vmlinux" "/boot/Image"; do
  [ -f "$ROOTFS$cand" ] && VMLINUZ="$cand" && break
done
[ -n "$VMLINUZ" ] || { echo "kernel image not found" >&2; exit 1; }

# fsck pass MUST be 0 for both: btrfs has no boot-time fsck, and Void's runit
# stage 1 treats a fsck.fat failure on the ESP as fatal and drops to an
# emergency shell. The ESP only holds GRUB + the kernel, so skipping its
# boot-time fsck is safe and standard.
cat > "$ROOTFS/etc/fstab" <<EOF
LABEL=$ROOT_LABEL  /         btrfs  defaults,subvol=@,compress=zstd:1  0 0
LABEL=saltos-esp   /boot/efi vfat   defaults                          0 0
EOF

echo "==> unmounting chroot pseudo-fs"
umount -R "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys" 2>/dev/null || true
trap - EXIT INT TERM
rm -f "$ROOTFS/etc/resolv.conf"

# ---------------------------------------------------------------------------
# Assemble the raw EFI disk image: GPT = ESP (fat32) + btrfs root (subvol @).
# ---------------------------------------------------------------------------
IMG="$OUT/$(saltos_artifact_name vm "$ARCH" img)"
rm -f "$IMG"
truncate -s "${IMG_SIZE_MB}M" "$IMG"

sgdisk -Z "$IMG"
sgdisk -n 1:0:+"${ESP_SIZE_MB}"M -t 1:ef00 -c 1:"esp" "$IMG"
sgdisk -n 2:0:0 -t 2:8300 -c 2:"rootfs" "$IMG"

LOOP="$(losetup --show -fP "$IMG")"
trap 'umount -R "$WORK/mnt" 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true' EXIT
ESPP="${LOOP}p1"; ROOTP="${LOOP}p2"
for i in $(seq 1 20); do [ -b "$ESPP" ] && [ -b "$ROOTP" ] && break; sleep 0.3; done

mkfs.vfat -F 32 -n saltos-esp "$ESPP"
mkfs.btrfs -f -L "$ROOT_LABEL" "$ROOTP"

mkdir -p "$WORK/mnt"
mount "$ROOTP" "$WORK/mnt"
btrfs subvolume create "$WORK/mnt/@"
btrfs subvolume create "$WORK/mnt/@snapshots"
umount "$WORK/mnt"

mount -o subvol=@,compress=zstd:1 "$ROOTP" "$WORK/mnt"
mkdir -p "$WORK/mnt/.snapshots" "$WORK/mnt/boot/efi"
cp -aT "$ROOTFS" "$WORK/mnt"
mount "$ESPP" "$WORK/mnt/boot/efi"

# GRUB cmdline: console=hvc0 (virtio serial) for the text/serial console, plus
# console=tty0 for the virtio-gpu framebuffer. The last console= owns /dev/console.
ROOTFLAGS="rootflags=subvol=@ rootfstype=btrfs"
KCMDLINE="root=LABEL=$ROOT_LABEL $ROOTFLAGS rootwait rw console=tty0 console=ttyS0,115200 loglevel=4 net.ifnames=0"

mkdir -p "$WORK/mnt/boot/efi/EFI/BOOT" "$WORK/mnt/boot/grub"
cat > "$WORK/mnt/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
insmod all_video
insmod gfxterm
serial --unit=0 --speed=115200
terminal_input console serial
terminal_output console serial
menuentry "saltOS $VERSION (x86_64 UEFI VM)" {
  search --no-floppy --set=root --label $ROOT_LABEL
  linux /@$VMLINUZ $KCMDLINE
  initrd /@/boot/initramfs-$KVER.img
}
menuentry "saltOS $VERSION (safe graphics, nomodeset)" {
  search --no-floppy --set=root --label $ROOT_LABEL
  linux /@$VMLINUZ $KCMDLINE nomodeset
  initrd /@/boot/initramfs-$KVER.img
}
EOF

# Standalone GRUB EFI binary that Apple's EFI loader picks up at the default path.
EARLY_CFG="$WORK/grub-early.cfg"
cat > "$EARLY_CFG" <<EOF
search --no-floppy --set=root --label $ROOT_LABEL
set prefix=(\$root)/@/boot/grub
configfile (\$root)/@/boot/grub/grub.cfg
EOF
grub-mkstandalone \
  --format=x86_64-efi \
  --output="$WORK/mnt/boot/efi/EFI/BOOT/$EFI_BOOT_NAME" \
  --modules="part_gpt fat btrfs normal search search_label configfile linux all_video gfxterm serial" \
  "boot/grub/grub.cfg=$EARLY_CFG"

sync
umount -R "$WORK/mnt"
losetup -d "$LOOP"
trap - EXIT

echo "wrote $IMG"
ls -lh "$IMG"

if command -v zstd >/dev/null 2>&1; then
  zstd -19 -T0 -f "$IMG" -o "$IMG.zst"
  echo "wrote $IMG.zst"
  ls -lh "$IMG.zst"
fi
