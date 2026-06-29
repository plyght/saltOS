#!/bin/bash
set -euxo pipefail

ARCH=aarch64
DARCH=arm64
EDITION="${EDITION:-console}"
WORK="${WORK:-$PWD/pi-work}"
OUT="${OUT:-$PWD/out-pi}"
SALT_BIN="${SALT_BIN:-$PWD/build/src/salt/salt}"
SALTSETUP_BIN="${SALTSETUP_BIN:-$PWD/build/src/setup/salt-setup}"
REPO="${REPO_DIR:-$PWD}"
VERSION="${VERSION:-0.1.0}"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
RPI_MIRROR="${RPI_MIRROR:-http://archive.raspberrypi.com/debian}"
RPI_SUITE="${RPI_SUITE:-bookworm}"
IMG_SIZE_MB="${IMG_SIZE_MB:-7168}"
BOOT_SIZE_MB="${BOOT_SIZE_MB:-512}"
OTA_SOURCE="${OTA_SOURCE:-}"
OTA_PUBKEY="${OTA_PUBKEY:-}"
HOSTNAME="${SALTOS_HOSTNAME:-saltos-pi}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1" >&2; exit 1; }; }
need mmdebstrap
need sgdisk
need mkfs.vfat
need mkfs.btrfs
need losetup

[ "$(uname -m)" = "aarch64" ] || echo "warning: building aarch64 image on $(uname -m); ensure binfmt/qemu-user-static is configured" >&2

ROOTFS="$WORK/rootfs"
rm -rf "$WORK"
mkdir -p "$ROOTFS" "$OUT"

BASE_PKGS="btrfs-progs,dosfstools,e2fsprogs,\
util-linux,kmod,pciutils,usbutils,file,less,nano,bash,coreutils,procps,\
iproute2,iputils-ping,isc-dhcp-client,wpasupplicant,wireless-regdb,ca-certificates,\
curl,tar,xz-utils,debootstrap,sudo,openssh-server,\
runit-init,chrony,\
firmware-brcm80211,firmware-misc-nonfree,\
libzstd1,libsodium23,libsqlite3-0,zstd"

DESKTOP_PKGS="xserver-xorg-core,xserver-xorg-input-libinput,\
xserver-xorg-input-all,xserver-xorg-video-fbdev,xinit,xterm,openbox,\
lxqt-core,lxqt-panel,lxqt-session,lxqt-config,lxqt-policykit,\
qterminal,pcmanfm-qt,featherpad,lxqt-themes,oxygen-icon-theme,\
adwaita-icon-theme,xdg-user-dirs,xdg-utils,mesa-utils,libgl1-mesa-dri,\
network-manager,nm-tray,dbus,dbus-x11,udev,fonts-dejavu,\
elogind,libpam-elogind,policykit-1"

PKGS="$BASE_PKGS"
if [ "$EDITION" = "desktop" ]; then
  PKGS="$PKGS,$DESKTOP_PKGS"
fi

KEYRING="$WORK/raspberrypi-archive-keyring.gpg"
if [ -f /usr/share/keyrings/raspberrypi-archive-keyring.gpg ]; then
  cp /usr/share/keyrings/raspberrypi-archive-keyring.gpg "$KEYRING"
else
  curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key \
    | gpg --dearmor > "$KEYRING"
fi

mmdebstrap \
  --variant=apt \
  --arch="$DARCH" \
  --components="main contrib non-free-firmware non-free" \
  --include="$PKGS" \
  --customize-hook='mkdir -p "$1/etc/apt/sources.list.d" "$1/usr/share/keyrings"' \
  --customize-hook="copy-in $KEYRING /usr/share/keyrings" \
  --customize-hook='echo "deb [signed-by=/usr/share/keyrings/raspberrypi-archive-keyring.gpg] '"$RPI_MIRROR"' '"$RPI_SUITE"' main" > "$1/etc/apt/sources.list.d/raspi.list"' \
  --customize-hook='chroot "$1" apt-get update' \
  --customize-hook='chroot "$1" apt-get install -y --no-install-recommends linux-image-rpi-2712 raspi-firmware raspberrypi-sys-mods' \
  "$SUITE" "$ROOTFS" "$MIRROR"

install -Dm755 "$SALT_BIN" "$ROOTFS/usr/bin/salt"
[ -f "$SALTSETUP_BIN" ] && install -Dm755 "$SALTSETUP_BIN" "$ROOTFS/usr/bin/salt-setup"

mkdir -p "$ROOTFS/etc/salt/strata" "$ROOTFS/usr/local/salt/shims" "$ROOTFS/strata"
cp "$REPO"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
install -Dm644 "$REPO/os/profile.d/salt-shims.sh" "$ROOTFS/etc/profile.d/salt-shims.sh"

cat > "$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION (raspberry pi 5)"
ID=saltos
ID_LIKE=debian
VERSION="$VERSION"
VERSION_ID="$VERSION"
HOME_URL="https://github.com/plyght/saltOS"
EOF
echo "$HOSTNAME" > "$ROOTFS/etc/hostname"
cat > "$ROOTFS/etc/hosts" <<EOF
127.0.0.1   localhost
127.0.1.1   $HOSTNAME
::1         localhost ip6-localhost ip6-loopback
ff02::1     ip6-allnodes
ff02::2     ip6-allrouters
EOF

install -Dm755 "$REPO/os/runit/stages/1" "$ROOTFS/etc/runit/1"
install -Dm755 "$REPO/os/runit/stages/2" "$ROOTFS/etc/runit/2"
install -Dm755 "$REPO/os/runit/stages/3" "$ROOTFS/etc/runit/3"
rm -rf "$ROOTFS/etc/runit/runsvdir/current"
mkdir -p "$ROOTFS/etc/runit/sv" "$ROOTFS/etc/runit/runsvdir/current"
cp -a "$REPO/os/runit/sv/." "$ROOTFS/etc/runit/sv/"
install -Dm755 "$REPO/os/runit/svc" "$ROOTFS/usr/bin/svc"

install -Dm755 "$REPO/os/btrfs/snapshot.sh" "$ROOTFS/usr/lib/saltos/snapshot.sh"
install -Dm755 "$REPO/os/pi/ab-update.sh" "$ROOTFS/usr/lib/saltos/ab-update.sh"
install -Dm755 "$REPO/os/ota/salt-ota.sh" "$ROOTFS/usr/bin/salt-ota"

enable_sv() {
  ln -sf "/etc/runit/sv/$1" "$ROOTFS/etc/runit/runsvdir/current/$1"
}
enable_sv agetty-tty1
enable_sv chronyd
enable_sv sshd
enable_sv salt-update

mkdir -p "$ROOTFS/etc/runit/sv/netdhcp"
cat > "$ROOTFS/etc/runit/sv/netdhcp/run" <<'EOF'
#!/bin/sh
exec 2>&1
iface=$(ip -o link show 2>/dev/null | awk -F': ' '$2 != "lo" {print $2; exit}')
[ -n "$iface" ] || { sleep 5; exec sleep 30; }
ip link set "$iface" up 2>/dev/null
exec dhclient -d "$iface" 2>/dev/null || exec udhcpc -f -i "$iface"
EOF
chmod +x "$ROOTFS/etc/runit/sv/netdhcp/run"
enable_sv netdhcp

mkdir -p "$ROOTFS/etc/salt"
cat > "$ROOTFS/etc/salt/repo.conf" <<EOF
repo = "current"
source = "$OTA_SOURCE"
key = "/etc/salt/keys/ota.pub"
EOF
mkdir -p "$ROOTFS/etc/salt/keys"
if [ -n "$OTA_PUBKEY" ] && [ -f "$OTA_PUBKEY" ]; then
  install -Dm644 "$OTA_PUBKEY" "$ROOTFS/etc/salt/keys/ota.pub"
else
  : > "$ROOTFS/etc/salt/keys/ota.pub"
fi
cat > "$ROOTFS/etc/salt/salt.conf" <<'EOF'
[install]
auto_expose = "prompt"

[strata]
expose_pm = true
auto_service = true

[ota]
enabled = true
interval = "86400"
reboot_on_kernel = false
EOF

chroot "$ROOTFS" useradd -m -s /bin/bash salt 2>/dev/null || true
echo "salt:salt" | chroot "$ROOTFS" chpasswd || true
echo "root:root" | chroot "$ROOTFS" chpasswd || true
chroot "$ROOTFS" usermod -aG sudo salt 2>/dev/null || true
mkdir -p "$ROOTFS/etc/sudoers.d"
echo "salt ALL=(ALL) NOPASSWD: ALL" > "$ROOTFS/etc/sudoers.d/salt"
chmod 0440 "$ROOTFS/etc/sudoers.d/salt"

cat > "$ROOTFS/etc/runit/sv/agetty-tty1/run" <<'EOF'
#!/bin/sh
exec 2>&1
exec setsid -w agetty --noclear --autologin salt tty1 38400 linux
EOF
chmod +x "$ROOTFS/etc/runit/sv/agetty-tty1/run"

mkdir -p "$ROOTFS/etc/runit/sv/agetty-serial"
cat > "$ROOTFS/etc/runit/sv/agetty-serial/run" <<'EOF'
#!/bin/sh
exec 2>&1
exec setsid -w agetty --noclear --autologin salt ttyAMA0 115200 linux
EOF
chmod +x "$ROOTFS/etc/runit/sv/agetty-serial/run"
enable_sv agetty-serial

cat > "$ROOTFS/etc/motd" <<'EOF'

  saltOS on Raspberry Pi 5 -- you are 'salt' (passwordless sudo).

  salt sync && salt update         pull signed OTA updates now
  salt-ota status                  show OTA / rollback state
  salt rollback                    revert the last host transaction
  salt --help

EOF

KVER="$(basename "$(ls -1 "$ROOTFS"/boot/vmlinuz-* | sort -V | tail -1)")"
KVER="${KVER#vmlinuz-}"

BOOT_STAGE="$WORK/boot"
mkdir -p "$BOOT_STAGE"

if [ -d "$ROOTFS/boot/firmware" ] && [ -n "$(ls -A "$ROOTFS/boot/firmware" 2>/dev/null)" ]; then
  cp -a "$ROOTFS/boot/firmware/." "$BOOT_STAGE/"
fi

cp "$ROOTFS/boot/vmlinuz-$KVER" "$BOOT_STAGE/vmlinuz" 2>/dev/null || true
cp "$ROOTFS/boot/initrd.img-$KVER" "$BOOT_STAGE/initramfs" 2>/dev/null || true

cat > "$BOOT_STAGE/config.txt" <<EOF
[all]
kernel=vmlinuz
initramfs initramfs followkernel
arm_64bit=1
arm_boost=1
enable_uart=1
disable_overscan=1
dtoverlay=vc4-kms-v3d
max_framebuffers=2
auto_initramfs=1
EOF

ROOT_LABEL=saltos-root
cat > "$BOOT_STAGE/cmdline.txt" <<EOF
console=serial0,115200 console=tty1 root=LABEL=$ROOT_LABEL rootfstype=btrfs rootflags=subvol=@ rootwait fsck.repair=yes net.ifnames=0
EOF

cat > "$ROOTFS/etc/fstab" <<EOF
LABEL=$ROOT_LABEL  /              btrfs  defaults,subvol=@,compress=zstd:1  0 1
LABEL=saltos-boot  /boot/firmware vfat   defaults                          0 2
EOF

if [ -f "$ROOTFS/etc/default/raspi-firmware" ]; then
  sed -i 's|^#\?ROOTPART=.*|ROOTPART=LABEL='"$ROOT_LABEL"'|' "$ROOTFS/etc/default/raspi-firmware" || true
fi

rm -f "$ROOTFS"/boot/vmlinuz-* "$ROOTFS"/boot/initrd.img-* 2>/dev/null || true
rm -rf "$ROOTFS/boot/firmware"
mkdir -p "$ROOTFS/boot/firmware"

IMG="$OUT/saltos-$VERSION-pi5-$ARCH.img"
rm -f "$IMG"
truncate -s "${IMG_SIZE_MB}M" "$IMG"

sgdisk -Z "$IMG"
sgdisk -n 1:0:+"${BOOT_SIZE_MB}"M -t 1:0c00 -c 1:"bootfs" "$IMG"
sgdisk -n 2:0:0 -t 2:8300 -c 2:"rootfs" "$IMG"

LOOP="$(losetup --show -fP "$IMG")"
trap 'umount -R "$WORK/mnt" 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true' EXIT

BOOTP="${LOOP}p1"
ROOTP="${LOOP}p2"
for i in $(seq 1 20); do [ -b "$BOOTP" ] && [ -b "$ROOTP" ] && break; sleep 0.3; done

mkfs.vfat -F 32 -n saltos-boot "$BOOTP"
mkfs.btrfs -f -L "$ROOT_LABEL" "$ROOTP"

mkdir -p "$WORK/mnt"
mount "$ROOTP" "$WORK/mnt"
btrfs subvolume create "$WORK/mnt/@"
btrfs subvolume create "$WORK/mnt/@snapshots"
umount "$WORK/mnt"

mount -o subvol=@,compress=zstd:1 "$ROOTP" "$WORK/mnt"
mkdir -p "$WORK/mnt/.snapshots" "$WORK/mnt/boot/firmware"
cp -aT "$ROOTFS" "$WORK/mnt"
mount "$BOOTP" "$WORK/mnt/boot/firmware"
cp -aT "$BOOT_STAGE" "$WORK/mnt/boot/firmware"

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
