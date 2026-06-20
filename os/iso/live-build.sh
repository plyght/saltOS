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
  x86_64) DARCH=amd64; SERIAL=ttyS0 ;;
  aarch64) DARCH=arm64; SERIAL=ttyAMA0 ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 1 ;;
esac

ROOTFS="$WORK/rootfs"
ISODIR="$WORK/iso"
rm -rf "$WORK"
mkdir -p "$ROOTFS" "$ISODIR/live" "$ISODIR/boot/grub" "$OUT"

BASE_PKGS="linux-image-$DARCH,live-boot,runit-init,btrfs-progs,dosfstools,e2fsprogs,\
util-linux,kmod,pciutils,file,less,nano,bash,coreutils,procps,\
iproute2,iputils-ping,isc-dhcp-client,ca-certificates,\
curl,tar,xz-utils,debootstrap,sudo,\
libzstd1,libsodium23,libsqlite3-0,zstd"

DESKTOP_PKGS="xserver-xorg-core,xserver-xorg-legacy,xserver-xorg-input-libinput,\
xserver-xorg-video-fbdev,xserver-xorg-video-vesa,xinit,xterm,openbox,lxqt-core,\
qterminal,pcmanfm-qt,lximage-qt,lxqt-archiver,featherpad,\
lxqt-themes,oxygen-icon-theme,breeze-icon-theme,adwaita-icon-theme,\
firefox-esr,network-manager,nm-tray,\
pipewire,pipewire-pulse,wireplumber,pavucontrol-qt,\
elogind,libpam-elogind,policykit-1,\
dbus,dbus-x11,udev,calamares,calamares-settings-debian,parted,gdisk,\
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

mkdir -p "$ROOTFS/etc/salt/strata" "$ROOTFS/usr/local/salt/shims" "$ROOTFS/strata"
cp "$REPO"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
install -Dm644 "$REPO/os/profile.d/salt-shims.sh" "$ROOTFS/etc/profile.d/salt-shims.sh"

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
cat > "$ROOTFS/etc/hosts" <<'EOF'
127.0.0.1   localhost
127.0.1.1   saltos-live
::1         localhost ip6-localhost ip6-loopback
ff02::1     ip6-allnodes
ff02::2     ip6-allrouters
EOF
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
if salt --version > /dev/console 2>&1; then
  echo "SALTOS_BOOT_OK runit stage 2 reached; salt runs" > /dev/console
else
  echo "SALTOS_BOOT_FAIL salt did not run" > /dev/console
fi
if salt --root / stratum list > /dev/console 2>&1; then
  echo "SALTOS_STRATUM_OK stratum plane initialized" > /dev/console
else
  echo "SALTOS_STRATUM_FAIL stratum plane did not initialize" > /dev/console
fi
exec sleep infinity
EOF
chmod +x "$ROOTFS/etc/runit/sv/boot-check/run"

enable_sv() {
  ln -sf "/etc/runit/sv/$1" "$ROOTFS/etc/runit/runsvdir/current/$1"
}
enable_sv agetty-tty1
enable_sv boot-check

if [ "${SALTOS_E2E:-0}" = "1" ] && [ "$EDITION" != "desktop" ]; then
  mkdir -p "$ROOTFS/etc/runit/sv/stratum-e2e"
  cat > "$ROOTFS/etc/runit/sv/stratum-e2e/run" <<'E2E'
#!/bin/sh
exec 2>&1
echo "SALTOS_E2E starting" > /dev/console
sleep 3
iface=$(ip -o link show 2>/dev/null | awk -F': ' '$2 != "lo" {print $2; exit}')
echo "SALTOS_E2E iface=$iface" > /dev/console
[ -n "$iface" ] && ip link set "$iface" up 2>/dev/null
dhclient -1 "$iface" > /dev/console 2>&1 || dhclient "$iface" > /dev/console 2>&1
net=0
for i in $(seq 1 40); do
  if getent hosts dl-cdn.alpinelinux.org > /dev/null 2>&1; then net=1; break; fi
  sleep 2
done
echo "SALTOS_E2E net=$net" > /dev/console
if salt --yes stratum add alpine > /dev/console 2>&1 \
  && salt run alpine apk update > /dev/console 2>&1 \
  && salt pkg alpine install nano > /dev/console 2>&1 \
  && salt run alpine /usr/bin/nano --version > /dev/console 2>&1; then
  echo "SALTOS_STRATUM_E2E_OK booted saltOS bootstrapped alpine, apk-installed nano, and ran it" > /dev/console
else
  echo "SALTOS_STRATUM_E2E_FAIL stratum end-to-end failed" > /dev/console
fi
exec sleep infinity
E2E
  chmod +x "$ROOTFS/etc/runit/sv/stratum-e2e/run"
  enable_sv stratum-e2e
fi

if [ "$EDITION" = "desktop" ]; then
  enable_sv dbus

  mkdir -p "$ROOTFS/etc/runit/sv/elogind"
  cat > "$ROOTFS/etc/runit/sv/elogind/run" <<'EOF'
#!/bin/sh
exec 2>&1
mkdir -p /run/dbus
for d in /lib/elogind/elogind /usr/libexec/elogind/elogind /usr/lib/elogind/elogind; do
  [ -x "$d" ] && exec "$d"
done
echo "elogind binary not found" >&2
exec sleep 5
EOF
  chmod +x "$ROOTFS/etc/runit/sv/elogind/run"
  enable_sv elogind

  mkdir -p "$ROOTFS/etc/runit/sv/NetworkManager"
  cat > "$ROOTFS/etc/runit/sv/NetworkManager/run" <<'EOF'
#!/bin/sh
exec 2>&1
[ -d /var/lib/NetworkManager ] || mkdir -p /var/lib/NetworkManager
exec NetworkManager --no-daemon
EOF
  chmod +x "$ROOTFS/etc/runit/sv/NetworkManager/run"
  enable_sv NetworkManager

  mkdir -p "$ROOTFS/etc/runit/sv/desktop-check"
  cat > "$ROOTFS/etc/runit/sv/desktop-check/run" <<'EOF'
#!/bin/sh
exec 2>&1
n=0
while [ "$n" -lt 120 ]; do
  if pgrep -x lxqt-panel >/dev/null 2>&1; then
    echo "SALTOS_DESKTOP_OK lxqt panel is running" > /dev/console
    exec sleep infinity
  fi
  sleep 2
  n=$((n + 1))
done
{
  echo "SALTOS_DESKTOP_DIAG lxqt-panel not seen after 240s"
  echo "--- processes ---"
  ps -e -o pid,args 2>/dev/null | grep -iE "[X]org|[l]xqt|[o]penbox|[d]bus|[e]logind|[s]tartx" || true
  echo "--- /var/log/Xorg.0.log (tail) ---"
  tail -n 45 /var/log/Xorg.0.log 2>/dev/null || echo "(no Xorg.0.log)"
  echo "--- /home/salt/.xsession-errors (tail) ---"
  tail -n 80 /home/salt/.xsession-errors 2>/dev/null || echo "(no .xsession-errors)"
  echo "SALTOS_DESKTOP_DIAG_END"
} > /dev/console 2>&1
exec sleep infinity
EOF
  chmod +x "$ROOTFS/etc/runit/sv/desktop-check/run"
  enable_sv desktop-check
fi

mkdir -p "$ROOTFS/etc/salt"
cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
repo = "current"
source = ""
key = ""
EOF
chroot "$ROOTFS" /usr/bin/salt --root / list >/dev/null 2>&1 || true

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
exec agetty --noclear --autologin salt tty1 38400 linux
EOF
chmod +x "$ROOTFS/etc/runit/sv/agetty-tty1/run"

mkdir -p "$ROOTFS/etc/runit/sv/agetty-serial"
cat > "$ROOTFS/etc/runit/sv/agetty-serial/run" <<EOF
#!/bin/sh
exec 2>&1
exec agetty --noclear --autologin salt $SERIAL 115200 linux
EOF
chmod +x "$ROOTFS/etc/runit/sv/agetty-serial/run"
enable_sv agetty-serial

cat > "$ROOTFS/etc/motd" <<'EOF'

  saltOS live -- you are 'salt' (passwordless sudo).

  sudo salt stratum add alpine
  sudo salt run alpine /bin/busybox echo hello-from-alpine
  sudo salt pkg alpine install ripgrep && sudo salt run alpine rg --version
  salt --help

EOF

if [ "$EDITION" != "desktop" ]; then
  mkdir -p "$ROOTFS/etc/runit/sv/netdhcp"
  cat > "$ROOTFS/etc/runit/sv/netdhcp/run" <<'EOF'
#!/bin/sh
exec 2>&1
iface=$(ip -o link show 2>/dev/null | awk -F': ' '$2 != "lo" {print $2; exit}')
[ -n "$iface" ] || { sleep 5; exec sleep 30; }
ip link set "$iface" up 2>/dev/null
exec dhclient -d "$iface"
EOF
  chmod +x "$ROOTFS/etc/runit/sv/netdhcp/run"
  enable_sv netdhcp
fi

if [ "$EDITION" = "desktop" ]; then
  cat > "$ROOTFS/home/salt/.bash_profile" <<'EOF'
if [ -z "${DISPLAY:-}" ] && [ "$(tty)" = /dev/tty1 ]; then
  exec startx /usr/bin/lxqt-session > "$HOME/.xsession-errors" 2>&1
fi
EOF
  chroot "$ROOTFS" chown salt:salt /home/salt/.bash_profile || true

  sed -i 's/^allowed_users=.*/allowed_users=anybody/; s/^#\?needs_root_rights=.*/needs_root_rights=yes/' \
    "$ROOTFS/etc/X11/Xwrapper.config" 2>/dev/null || \
    printf 'allowed_users=anybody\nneeds_root_rights=yes\n' > "$ROOTFS/etc/X11/Xwrapper.config"

  install -Dm755 "$REPO/os/iso/live/Install-saltOS.desktop" \
    "$ROOTFS/home/salt/Desktop/Install-saltOS.desktop" 2>/dev/null || true

  mkdir -p "$ROOTFS/etc/xdg/lxqt"
  cat > "$ROOTFS/etc/xdg/lxqt/session.conf" <<'EOF'
[General]
__userfile__=true
window_manager=openbox

[Environment]
EOF
  mkdir -p "$ROOTFS/home/salt/.config/lxqt"
  cp "$ROOTFS/etc/xdg/lxqt/session.conf" "$ROOTFS/home/salt/.config/lxqt/session.conf"

  mkdir -p "$ROOTFS/usr/lib/saltos"
  cat > "$ROOTFS/usr/lib/saltos/start-audio.sh" <<'EOF'
#!/bin/sh
/usr/bin/pipewire &
sleep 1
/usr/bin/wireplumber &
/usr/bin/pipewire-pulse &
EOF
  chmod +x "$ROOTFS/usr/lib/saltos/start-audio.sh"

  mkdir -p "$ROOTFS/etc/xdg/autostart"
  cat > "$ROOTFS/etc/xdg/autostart/saltos-audio.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=saltOS audio (PipeWire)
Exec=/usr/lib/saltos/start-audio.sh
OnlyShowIn=LXQt;
X-LXQt-Need-Tray=false
NoDisplay=true
EOF

  chroot "$ROOTFS" chown -R salt:salt /home/salt 2>/dev/null || true
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
  linux /live/vmlinuz boot=live components init=/sbin/runit-init console=tty0 console=$SERIAL,115200
  initrd /live/initrd
}
menuentry "saltOS $VERSION (live, to RAM)" {
  linux /live/vmlinuz boot=live components toram init=/sbin/runit-init console=tty0 console=$SERIAL,115200
  initrd /live/initrd
}
EOF

ISO_PATH="$OUT/saltos-$VERSION-$EDITION-$ARCH.iso"
grub-mkrescue -o "$ISO_PATH" "$ISODIR" \
  -- -volid "SALTOS_LIVE"
echo "wrote $ISO_PATH"
ls -lh "$ISO_PATH"
