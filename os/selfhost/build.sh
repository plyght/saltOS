#!/bin/bash
set -euxo pipefail

ARCH="${1:-x86_64}"
REPO="${REPO_DIR:-$PWD}"
WORK="${WORK:-$PWD/selfhost-work}"
OUT="${OUT:-$PWD/out-iso}"
VERSION="${VERSION:-0.1.0}"
JOBS="${JOBS:-$(nproc)}"
EDITION="${EDITION:-console}"
PROFILE="${PROFILE:-base}"

if [ "$PROFILE" = "thinkpad" ]; then
  KERNEL_VER="${KERNEL_VER:-6.12.30}"
  KCONFIG_FRAGMENT="$REPO/os/selfhost/kernel-thinkpad-${ARCH}.config"
  KREQUIRED="$REPO/os/selfhost/kernel-required-${ARCH}.txt"
else
  KERNEL_VER="${KERNEL_VER:-6.6.52}"
  KCONFIG_FRAGMENT="$REPO/os/selfhost/kernel-${ARCH}.config"
  KREQUIRED=""
fi

BUSYBOX_VER="1.36.1"
RUNIT_VER="2.1.2"
ZSTD_VER="1.5.6"
SODIUM_VER="1.0.20"
SQLITE_TAR="sqlite-autoconf-3460000"
GLIBC_VER="2.40"
BASH_VER="5.2.21"
COREUTILS_VER="9.5"

SRC="$WORK/src"
DEPS="$WORK/deps"
GNU="$WORK/gnu"
ROOTFS="$WORK/rootfs"
rm -rf "$WORK"
mkdir -p "$SRC" "$DEPS" "$ROOTFS" "$OUT"

fetch() {
  local url="$1" out="$2"
  echo "fetch $url"
  curl -fsSL "$url" -o "$out"
}

cd "$SRC"
fetch "https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2" busybox.tar.bz2
fetch "http://smarden.org/runit/runit-${RUNIT_VER}.tar.gz" runit.tar.gz
fetch "https://github.com/facebook/zstd/releases/download/v${ZSTD_VER}/zstd-${ZSTD_VER}.tar.gz" zstd.tar.gz
fetch "https://download.libsodium.org/libsodium/releases/libsodium-${SODIUM_VER}.tar.gz" sodium.tar.gz
fetch "https://www.sqlite.org/2024/${SQLITE_TAR}.tar.gz" sqlite.tar.gz
fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VER}.tar.xz" linux.tar.xz
fetch "https://ftp.gnu.org/gnu/glibc/glibc-${GLIBC_VER}.tar.xz" glibc.tar.xz
fetch "https://ftp.gnu.org/gnu/bash/bash-${BASH_VER}.tar.gz" bash.tar.gz
fetch "https://ftp.gnu.org/gnu/coreutils/coreutils-${COREUTILS_VER}.tar.xz" coreutils.tar.xz

for f in busybox.tar.bz2 runit.tar.gz zstd.tar.gz sodium.tar.gz sqlite.tar.gz linux.tar.xz \
         glibc.tar.xz bash.tar.gz coreutils.tar.xz; do
  tar -xf "$f"
done

echo "===== static deps for salt ====="
( cd "zstd-${ZSTD_VER}" && make -j"$JOBS" && make PREFIX="$DEPS" install )
( cd "libsodium-${SODIUM_VER}" && ./configure --prefix="$DEPS" --enable-static --disable-shared \
    && make -j"$JOBS" && make install )
( cd "$SQLITE_TAR" && ./configure --prefix="$DEPS" --enable-static --disable-shared \
    && make -j"$JOBS" && make install )
rm -f "$DEPS"/lib/*.so "$DEPS"/lib/*.so.* 2>/dev/null || true

echo "===== static salt ====="
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig"
cmake -S "$REPO" -B "$WORK/salt-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DEPS" \
  -DCMAKE_FIND_LIBRARY_SUFFIXES=".a" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
cmake --build "$WORK/salt-build" --target salt -j"$JOBS"
SALT_STATIC="$WORK/salt-build/src/salt/salt"
file "$SALT_STATIC"

echo "===== static busybox ====="
( cd "busybox-${BUSYBOX_VER}"
  make defconfig
  sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
  sed -i 's/^CONFIG_PIE=y/# CONFIG_PIE is not set/' .config 2>/dev/null || true
  sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config 2>/dev/null || true
  sed -i 's/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config 2>/dev/null || true
  make oldconfig </dev/null
  grep -q '^CONFIG_STATIC=y' .config || { echo "FATAL: busybox CONFIG_STATIC was dropped"; exit 1; }
  make -j"$JOBS"
  file busybox | grep -q 'statically linked' || { echo "FATAL: busybox is not static"; exit 1; }
  make CONFIG_PREFIX="$WORK/bb-install" install )

echo "===== static runit ====="
RUNIT_SRC="$SRC/admin/runit-${RUNIT_VER}/src"
[ -d "$RUNIT_SRC" ] || RUNIT_SRC="$SRC/runit-${RUNIT_VER}/src"
( cd "$RUNIT_SRC"
  echo 'gcc -static' > conf-cc
  echo 'gcc -static' > conf-ld
  make )

echo "===== glibc (from source) ====="
GLIBC_CC="${GLIBC_CC:-gcc-12}"
command -v "$GLIBC_CC" >/dev/null 2>&1 || GLIBC_CC=gcc
mkdir -p "$SRC/glibc-build"
( cd "$SRC/glibc-build"
  "$SRC/glibc-${GLIBC_VER}/configure" \
    CC="$GLIBC_CC" CXX="${GLIBC_CC/gcc/g++}" \
    --prefix=/usr \
    --disable-werror \
    --disable-nscd \
    --without-selinux \
    --enable-kernel=4.19
  make -j"$JOBS"
  make DESTDIR="$GNU" install )

echo "===== bash (from source) ====="
( cd "$SRC/bash-${BASH_VER}"
  ./configure --prefix=/usr --without-bash-malloc
  make -j"$JOBS"
  make DESTDIR="$GNU" install )

echo "===== coreutils (from source) ====="
( cd "$SRC/coreutils-${COREUTILS_VER}"
  FORCE_UNSAFE_CONFIGURE=1 ./configure --prefix=/usr
  make -j"$JOBS"
  make DESTDIR="$GNU" install )

echo "===== assemble rootfs ====="
mkdir -p "$ROOTFS"/{proc,sys,dev,run,tmp,root,var/lib/salt,var/cache/salt,etc,strata}

echo "===== lay down GNU userland first (glibc/bash/coreutils -> /usr,/lib) ====="
cp -a "$GNU/." "$ROOTFS/"

for d in bin sbin usr/bin usr/sbin; do
  [ -L "$ROOTFS/$d" ] && rm -f "$ROOTFS/$d"
  mkdir -p "$ROOTFS/$d"
done

echo "===== static busybox on top (owns /bin and /sbin) ====="
cp -a "$WORK/bb-install/bin/"* "$ROOTFS/bin/"
cp -a "$WORK/bb-install/sbin/"* "$ROOTFS/sbin/" 2>/dev/null || true
for f in "$WORK/bb-install/usr/bin/"*; do
  n="$(basename "$f")"; [ -e "$ROOTFS/usr/bin/$n" ] || cp -a "$f" "$ROOTFS/usr/bin/$n"
done
for f in "$WORK/bb-install/usr/sbin/"*; do
  n="$(basename "$f")"; [ -e "$ROOTFS/usr/sbin/$n" ] || cp -a "$f" "$ROOTFS/usr/sbin/$n"
done

for b in runit runit-init runsv runsvdir runsvchdir sv chpst utmpset; do
  [ -f "$RUNIT_SRC/$b" ] && install -Dm755 "$RUNIT_SRC/$b" "$ROOTFS/sbin/$b"
done

install -Dm755 "$SALT_STATIC" "$ROOTFS/usr/bin/salt"
install -Dm755 "$REPO/os/selfhost/saltos-install" "$ROOTFS/usr/bin/saltos-install"
mkdir -p "$ROOTFS/etc/salt/strata" "$ROOTFS/usr/local/salt/shims" "$ROOTFS/etc/profile.d"
cp "$REPO"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
install -Dm644 "$REPO/os/profile.d/salt-shims.sh" "$ROOTFS/etc/profile.d/salt-shims.sh"
cat > "$ROOTFS/etc/salt/salt.conf" <<'EOF'
[install]
auto_expose = "always"

[strata]
expose_pm = true
expose_all = true
auto_service = true
EOF

# salt's current fetch path shells out to `curl -fsSL`. The self-host proof base
# has BusyBox wget but not full curl yet, so provide a tiny compatibility wrapper
# until curl/openssl/ca-certificates become native grains in the daily-driver base.
if [ "$PROFILE" != "thinkpad" ]; then
cat > "$ROOTFS/usr/bin/curl" <<'EOF'
#!/bin/sh
out=""
url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    -*) shift ;;
    *) url="$1"; shift ;;
  esac
done
[ -n "$url" ] || exit 2
if [ -n "$out" ]; then
  exec wget -q -O "$out" "$url"
fi
exec wget -q -O - "$url"
EOF
chmod +x "$ROOTFS/usr/bin/curl"
fi

if [ "$PROFILE" = "thinkpad" ]; then
  echo "===== network stack (openssl, curl, libnl, wpa_supplicant) ====="
  # net.sh and base-tools.sh are sourced and export toolchain vars aimed at their
  # staging prefix. The kernel build runs later in this same shell, so snapshot
  # PATH and drop those vars afterwards or the kernel picks up target headers.
  SAVED_PATH="$PATH"
  if [ -n "${NETCACHE:-}" ]; then N="$NETCACHE/net"; else N="$WORK/net"; fi
  mkdir -p "$N"
  . "$REPO/os/selfhost/net.sh"
  cp -a "$N/etc/." "$ROOTFS/etc/" 2>/dev/null || true
  cp -a "$N/usr/." "$ROOTFS/usr/" 2>/dev/null || true
  rm -rf "$ROOTFS/usr/.salt-done"
  ldconfig -r "$ROOTFS" 2>/dev/null || true

  echo "===== base tools (sfdisk, mkfs.ext4) ====="
  if [ -n "${TOOLSCACHE:-}" ]; then T="$TOOLSCACHE/tools"; else T="$WORK/tools"; fi
  mkdir -p "$T"
  . "$REPO/os/selfhost/base-tools.sh"
  cp -a "$T/usr/." "$ROOTFS/usr/" 2>/dev/null || true
  rm -rf "$ROOTFS/usr/.salt-done"
  ldconfig -r "$ROOTFS" 2>/dev/null || true

  install -Dm755 "$REPO/os/selfhost/salt-wifi" "$ROOTFS/usr/bin/salt-wifi"
  install -Dm755 "$REPO/os/selfhost/salt-hw" "$ROOTFS/usr/bin/salt-hw"

  for t in sfdisk mkfs.ext4 blkid; do
    find "$ROOTFS/usr" -name "$t" -type f | grep -q . \
      || { echo "FATAL: $t missing from thinkpad rootfs"; exit 1; }
  done

  unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR CPPFLAGS LDFLAGS ACLOCAL_PATH LD_LIBRARY_PATH
  PATH="$SAVED_PATH"
  export PATH

  [ -x "$ROOTFS/usr/bin/curl" ] || { echo "FATAL: real curl missing from thinkpad rootfs"; exit 1; }
  [ -x "$ROOTFS/usr/sbin/wpa_supplicant" ] || { echo "FATAL: wpa_supplicant missing"; exit 1; }
  [ -f "$ROOTFS/etc/ssl/certs/ca-certificates.crt" ] || { echo "FATAL: CA bundle missing"; exit 1; }
fi

mkdir -p "$ROOTFS/lib64"
if [ ! -e "$ROOTFS/lib64/ld-linux-x86-64.so.2" ]; then
  REAL="$(find "$ROOTFS/usr/lib" "$ROOTFS/lib" -name 'ld-linux-x86-64.so.2' -type f 2>/dev/null | head -1 || true)"
  [ -n "$REAL" ] && ln -sf "${REAL#"$ROOTFS"}" "$ROOTFS/lib64/ld-linux-x86-64.so.2"
fi
[ -e "$ROOTFS/usr/bin/bash" ] && ln -sf /usr/bin/bash "$ROOTFS/bin/bash"
ldconfig -r "$ROOTFS" 2>/dev/null || true

if [ "$EDITION" = "desktop" ]; then
  echo "===== X11 desktop (from source) ====="
  if [ -n "${XCACHE:-}" ]; then X="$XCACHE/x"; else X="$WORK/x"; fi
  mkdir -p "$X"
  . "$REPO/os/selfhost/desktop.sh"
  cp -a "$X/." "$ROOTFS/"
  ldconfig -r "$ROOTFS" 2>/dev/null || true

  mkdir -p "$ROOTFS/etc/X11"
  cat > "$ROOTFS/etc/X11/xorg.conf" <<'EOF'
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "DontZap" "false"
EndSection
Section "Device"
    Identifier "fb"
    Driver "fbdev"
    Option "fbdev" "/dev/fb0"
EndSection
Section "Monitor"
    Identifier "mon"
EndSection
Section "Screen"
    Identifier "scr"
    Device "fb"
    Monitor "mon"
EndSection
Section "InputDevice"
    Identifier "kbd"
    Driver "kbd"
EndSection
Section "InputDevice"
    Identifier "mouse"
    Driver "mouse"
    Option "Device" "/dev/input/mice"
    Option "Protocol" "ImPS/2"
EndSection
Section "ServerLayout"
    Identifier "layout"
    Screen "scr"
    InputDevice "kbd" "CoreKeyboard"
    InputDevice "mouse" "CorePointer"
EndSection
EOF

  cat > "$ROOTFS/root/.xinitrc" <<'EOF'
xsetroot -solid "#1a3a5a"
xclock -geometry 160x160-10+10 &
xterm -geometry 90x30+20+20 -fn fixed -e /bin/bash &
echo "SALTOS_X_OK xorg + twm + xterm running, all from source, no Debian" > /dev/console
exec twm
EOF

  mkdir -p "$ROOTFS/etc/runit/sv/xorg"
  cat > "$ROOTFS/etc/runit/sv/xorg/run" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1
export HOME=/root
export PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
export XKB_CONFIG_ROOT=/usr/share/X11/xkb
sleep 3
exec startx -- :0 vt1
EOF
  chmod +x "$ROOTFS/etc/runit/sv/xorg/run"
  mkdir -p "$ROOTFS/etc/runit/runsvdir/current"
  ln -sf /etc/runit/sv/xorg "$ROOTFS/etc/runit/runsvdir/current/xorg"
fi

cat > "$ROOTFS/etc/profile" <<'EOF'
export PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
export PS1='saltOS:\w\$ '
[ -d /etc/profile.d ] && for f in /etc/profile.d/*.sh; do [ -r "$f" ] && . "$f"; done
EOF

cat > "$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION (self-hosted)"
ID=saltos
VERSION="$VERSION"
EOF
echo "saltos" > "$ROOTFS/etc/hostname"

mkdir -p "$ROOTFS/etc/runit/runsvdir/current"
cat > "$ROOTFS/etc/runit/1" <<'EOF'
#!/bin/sh
PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mount -t tmpfs run /run 2>/dev/null
mount -t tmpfs tmp /tmp 2>/dev/null
mkdir -p /dev/pts /dev/shm 2>/dev/null
mount -t devpts devpts /dev/pts -o gid=5,mode=620,ptmxmode=666 2>/dev/null
mount -t tmpfs shm /dev/shm -o mode=1777 2>/dev/null
# devtmpfs has no /dev/fd; process substitution <(...) and /dev/std* need these.
ln -sf /proc/self/fd /dev/fd 2>/dev/null
ln -sf /proc/self/fd/0 /dev/stdin 2>/dev/null
ln -sf /proc/self/fd/1 /dev/stdout 2>/dev/null
ln -sf /proc/self/fd/2 /dev/stderr 2>/dev/null
[ -r /etc/hostname ] && hostname "$(cat /etc/hostname)" 2>/dev/null
echo "saltOS self-hosted: stage 1 complete" > /dev/console
EOF
cat > "$ROOTFS/etc/runit/2" <<'EOF'
#!/bin/sh
PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
export PATH
exec runsvdir -P /etc/runit/runsvdir/current
EOF
cat > "$ROOTFS/etc/runit/3" <<'EOF'
#!/bin/sh
PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
echo "saltOS: shutting down" > /dev/console
sync
poweroff -f
EOF
chmod +x "$ROOTFS"/etc/runit/1 "$ROOTFS"/etc/runit/2 "$ROOTFS"/etc/runit/3

if [ "$PROFILE" = "thinkpad" ]; then
  cat >> "$ROOTFS/etc/runit/1" <<'EOF'
/usr/bin/salt-hw coldplug > /dev/console 2>&1 || true
mkdir -p /run/wpa_supplicant
echo "SALTOS_COLDPLUG_DONE" > /dev/console
EOF

  mkdir -p "$ROOTFS/etc/runit/sv/wifi"
  cat > "$ROOTFS/etc/runit/sv/wifi/run" <<'EOF'
#!/bin/sh
PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin; export PATH
[ -f /etc/wpa_supplicant.conf ] || exec sleep 3600
grep -q '^[[:space:]]*network=' /etc/wpa_supplicant.conf 2>/dev/null || exec sleep 3600
dev=""
for p in /sys/class/net/*; do
  n="${p##*/}"
  { [ -d "$p/wireless" ] || [ -L "$p/phy80211" ]; } || continue
  dev="$n"; break
done
[ -n "$dev" ] || exec sleep 30
mkdir -p /run/wpa_supplicant
ip link set "$dev" up 2>/dev/null || true
wpa_supplicant -i "$dev" -c /etc/wpa_supplicant.conf &
WPID=$!
n=0
while [ $n -lt 30 ]; do
  state="$(wpa_cli -i "$dev" status 2>/dev/null | sed -n 's/^wpa_state=//p')"
  [ "$state" = "COMPLETED" ] && break
  n=$((n + 1))
  sleep 1
done
if [ "${state:-}" = "COMPLETED" ]; then
  udhcpc -n -q -i "$dev" -s /usr/share/udhcpc/default.script >/dev/null 2>&1 || true
fi
wait $WPID
EOF
  chmod +x "$ROOTFS/etc/runit/sv/wifi/run"
  ln -sf /etc/runit/sv/wifi "$ROOTFS/etc/runit/runsvdir/current/wifi"

  mkdir -p "$ROOTFS/etc/runit/sv/hw-check"
  cat > "$ROOTFS/etc/runit/sv/hw-check/run" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1
PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin; export PATH
sleep 6
ok=1

kver="$(uname -r)"
if [ -d "/lib/modules/$kver" ] && [ -f "/lib/modules/$kver/modules.dep" ]; then
  echo "SALTOS_HW_MODULES_OK $(find /lib/modules/$kver -name '*.ko*' | wc -l) modules for $kver"
else
  echo "SALTOS_HW_MODULES_FAIL no module tree for $kver"; ok=0
fi

if [ -f /lib/firmware/regulatory.db.xz ] || [ -f /lib/firmware/regulatory.db ]; then
  echo "SALTOS_HW_FIRMWARE_OK $(du -sh /lib/firmware 2>/dev/null | cut -f1) firmware present"
else
  echo "SALTOS_HW_FIRMWARE_FAIL regulatory.db missing"; ok=0
fi

for m in iwlwifi ath11k_pci rtw89_pci mt7921e i915 amdgpu; do
  if find /lib/modules -name "${m}.ko*" 2>/dev/null | grep -q .; then
    echo "  driver available: $m"
  else
    echo "  driver MISSING: $m"; ok=0
  fi
done

if wpa_supplicant -v 2>&1 | head -1 | grep -qi wpa_supplicant; then
  echo "SALTOS_HW_WIFI_STACK_OK $(wpa_supplicant -v 2>&1 | head -1)"
else
  echo "SALTOS_HW_WIFI_STACK_FAIL wpa_supplicant did not run"; ok=0
fi

if curl --version 2>/dev/null | grep -qi openssl; then
  echo "SALTOS_HW_TLS_OK $(curl --version 2>/dev/null | head -1)"
else
  echo "SALTOS_HW_TLS_FAIL curl is not linked against openssl"; ok=0
fi

if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
  echo "  CA bundle: $(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt) certs"
else
  echo "SALTOS_HW_TLS_FAIL no CA bundle"; ok=0
fi

for f in suspend_to_idle mem; do
  grep -qw "$f" /sys/power/state 2>/dev/null && echo "  suspend supported: $f"
done

salt-hw report 2>/dev/null | head -60 || true

if [ "$ok" = 1 ]; then
  echo "SALTOS_THINKPAD_OK hardware plane ready"
else
  echo "SALTOS_THINKPAD_FAIL hardware plane incomplete"
fi
exec >/dev/null 2>&1
exec sleep infinity
EOF
  chmod +x "$ROOTFS/etc/runit/sv/hw-check/run"
  ln -sf /etc/runit/sv/hw-check "$ROOTFS/etc/runit/runsvdir/current/hw-check"
fi

mkdir -p "$ROOTFS/etc/runit/sv/boot-check"
cat > "$ROOTFS/etc/runit/sv/boot-check/run" <<'EOF'
#!/bin/sh
exec >/dev/console 2>&1
echo "----------------------------------------"
cat /etc/os-release
ok=1
salt --version || ok=0
bash --version | head -1 || ok=0
ls --version | head -1 || ok=0
echo "ldd salt:"; file /usr/bin/bash 2>/dev/null || true
if [ "$ok" = 1 ]; then
  echo "SALTOS_SELFHOST_OK kernel+glibc+bash+coreutils+runit+salt, all from source, no distro base"
else
  echo "SALTOS_SELFHOST_FAIL a component did not run"
fi
exec >/dev/null 2>&1
exec sleep infinity
EOF
chmod +x "$ROOTFS/etc/runit/sv/boot-check/run"

install -Dm755 /dev/stdin "$ROOTFS/usr/bin/saltos-console-login" <<'EOF'
#!/bin/sh
echo
echo "=== saltOS self-hosted console ($(tty 2>/dev/null || echo tty)) ==="
echo "Type: salt --version ; cat /etc/os-release"
echo
export HOME=/root
export PATH=/usr/local/salt/shims:/usr/bin:/usr/sbin:/bin:/sbin
export PS1='saltOS# '
cd /root 2>/dev/null || cd /
exec /bin/cttyhack /bin/sh -i
EOF

install -Dm755 /dev/stdin "$ROOTFS/usr/share/udhcpc/default.script" <<'EOF'
#!/bin/sh
case "$1" in
  bound|renew)
    ip link set "$interface" up 2>/dev/null || true
    [ -n "$ip" ] && [ -n "$subnet" ] && ip addr add "$ip/$subnet" dev "$interface" 2>/dev/null || true
    if [ -n "$router" ]; then
      for r in $router; do ip route add default via "$r" dev "$interface" 2>/dev/null || true; break; done
    fi
    if [ -n "$dns" ]; then
      : > /etc/resolv.conf
      for ns in $dns; do echo "nameserver $ns" >> /etc/resolv.conf; done
    fi
    ;;
esac
EOF

mkdir -p "$ROOTFS/etc/runit/sv/netdhcp"
cat > "$ROOTFS/etc/runit/sv/netdhcp/run" <<'EOF'
#!/bin/sh
PATH=/usr/bin:/usr/sbin:/bin:/sbin; export PATH
# Wait for a real (non-lo) interface that can be brought up before running dhcp,
# so udhcpc doesn't spam "SIOCGIFINDEX: No such device" before virtio-net probes
# (or when the VM has no NIC).
while :; do
  iface=""
  for p in /sys/class/net/*; do
    n="${p##*/}"
    [ "$n" = "lo" ] && continue
    [ -e "$p" ] || continue
    { [ -d "$p/wireless" ] || [ -L "$p/phy80211" ]; } && continue
    iface="$n"; break
  done
  if [ -n "$iface" ] && ip link set "$iface" up 2>/dev/null; then
    exec udhcpc -f -i "$iface" -s /usr/share/udhcpc/default.script
  fi
  sleep 5
done
EOF
chmod +x "$ROOTFS/etc/runit/sv/netdhcp/run"
ln -sf /etc/runit/sv/netdhcp "$ROOTFS/etc/runit/runsvdir/current/netdhcp"

# Login on the graphical console (tty1, what UTM/QEMU's display shows) plus a
# device-guarded serial fallback (ttyS0 for headless). Never run a getty on
# tty0 -- it aliases the active VT (tty1) and would race the tty1 getty on the
# same screen. An absent serial device just sleeps, so this is safe everywhere.
make_shell_sv() { # <name> <dev>
  mkdir -p "$ROOTFS/etc/runit/sv/$1"
  cat > "$ROOTFS/etc/runit/sv/$1/run" <<EOF
#!/bin/sh
[ -c /dev/$2 ] || exec sleep 5
exec /sbin/getty -L -n -i -l /usr/bin/saltos-console-login 115200 $2 linux
EOF
  chmod +x "$ROOTFS/etc/runit/sv/$1/run"
  ln -sf "/etc/runit/sv/$1" "$ROOTFS/etc/runit/runsvdir/current/$1"
}
make_shell_sv shell-tty1 tty1
make_shell_sv shell-serial ttyS0

ln -sf /etc/runit/sv/boot-check "$ROOTFS/etc/runit/runsvdir/current/boot-check"

ln -sf /sbin/runit-init "$ROOTFS/init"

mkdir -p "$ROOTFS/etc/salt"
cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
repo = "current"
source = ""
key = ""
EOF

echo "===== sanity: rootfs init shell must be static ====="
file "$ROOTFS/bin/busybox"
if ! file "$ROOTFS/bin/busybox" | grep -q 'statically linked'; then
  echo "FATAL: rootfs /bin/busybox is not static (init shell would depend on the loader)"
  ls -la "$ROOTFS/bin/busybox"
  exit 1
fi

pack_initrd() {
  if [ "$PROFILE" = "thinkpad" ]; then
    ( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | zstd -T0 -15 -q -o "$WORK/initrd.gz" -f )
  else
    ( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$WORK/initrd.gz" )
  fi
  ls -lh "$WORK/initrd.gz"
}

# The kernel image and the standalone EFI loader are added to the rootfs after
# the kernel is built, so the only pack that matters is the one further down.
# On the thinkpad profile the tree carries modules and firmware and packing it
# is expensive, so do it exactly once.
if [ "$PROFILE" != "thinkpad" ]; then
  echo "===== pack initramfs ====="
  pack_initrd
fi

echo "===== build kernel ====="
KCACHE="${KCACHE:-}"
if [ -n "$KCACHE" ] && [ -f "$KCACHE/bzImage" ]; then
  echo "using cached kernel from $KCACHE"
  cp "$KCACHE/bzImage" "$WORK/bzImage"
  if [ "$PROFILE" = "thinkpad" ] && [ -d "$KCACHE/modules" ]; then
    mkdir -p "$ROOTFS/lib/modules"
    cp -a "$KCACHE/modules/." "$ROOTFS/lib/modules/"
  fi
else
  cd "$SRC/linux-${KERNEL_VER}"
  make defconfig
  if [ "$PROFILE" = "thinkpad" ]; then
    ./scripts/kconfig/merge_config.sh -m -O . .config "$KCONFIG_FRAGMENT"
  else
    cat "$KCONFIG_FRAGMENT" >> .config
  fi
  make olddefconfig

  if [ -n "$KREQUIRED" ] && [ -f "$KREQUIRED" ]; then
    echo "===== verify required kernel symbols ====="
    missing=0
    while read -r sym want; do
      [ -n "$sym" ] || continue
      case "$sym" in \#*) continue ;; esac
      got="$(sed -n "s/^${sym}=\(.*\)$/\1/p" .config | head -1)"
      case "$want" in
        y)  [ "$got" = "y" ] || { echo "  MISSING $sym (want =y, got '${got:-unset}')"; missing=$((missing + 1)); } ;;
        ym) case "$got" in y|m) ;; *) echo "  MISSING $sym (want =y or =m, got '${got:-unset}')"; missing=$((missing + 1)) ;; esac ;;
      esac
    done < "$KREQUIRED"
    if [ "$missing" -gt 0 ]; then
      echo "FATAL: $missing required kernel symbols were dropped by olddefconfig"
      exit 1
    fi
    echo "  all required symbols present"
  fi

  make -j"$JOBS" bzImage
  cp arch/x86/boot/bzImage "$WORK/bzImage"

  if [ "$PROFILE" = "thinkpad" ]; then
    make -j"$JOBS" modules
    make INSTALL_MOD_PATH="$ROOTFS" INSTALL_MOD_STRIP=1 modules_install
    KREL="$(make -s kernelrelease)"
    rm -f "$ROOTFS/lib/modules/$KREL/build" "$ROOTFS/lib/modules/$KREL/source"
    depmod -b "$ROOTFS" "$KREL"
    echo "installed modules for $KREL"
    du -sh "$ROOTFS/lib/modules"
  fi

  if [ -n "$KCACHE" ]; then
    mkdir -p "$KCACHE"
    cp "$WORK/bzImage" "$KCACHE/bzImage"
    if [ "$PROFILE" = "thinkpad" ]; then
      rm -rf "$KCACHE/modules"
      mkdir -p "$KCACHE/modules"
      cp -a "$ROOTFS/lib/modules/." "$KCACHE/modules/"
    fi
  fi
fi

if [ "$PROFILE" = "thinkpad" ]; then
  echo "===== linux-firmware ====="
  WORK="$WORK" ROOTFS="$ROOTFS" FWCACHE="${FWCACHE:-}" bash "$REPO/os/selfhost/firmware.sh"
fi
mkdir -p "$ROOTFS/boot"
cp "$WORK/bzImage" "$ROOTFS/boot/bzImage"
mkdir -p "$ROOTFS/boot/efi/EFI/BOOT"
cat > "$WORK/grub-installed-standalone.cfg" <<'EOF'
search --no-floppy --label SALTOS_ROOT --set=root
configfile /boot/grub/grub.cfg
EOF
grub-mkstandalone -O x86_64-efi \
  --modules="part_msdos part_gpt fat ext2 search search_label normal linux" \
  -o "$ROOTFS/boot/efi/EFI/BOOT/BOOTX64.EFI" \
  "boot/grub/grub.cfg=$WORK/grub-installed-standalone.cfg"
echo "===== pack initramfs ====="
pack_initrd

if [ "${BUILD_INSTALLED_IMAGE:-0}" = 1 ]; then
  echo "===== build installed $ARCH disk image ====="
  ARCH="$ARCH" ROOTFS="$ROOTFS" KERNEL="$WORK/bzImage" OUT="$OUT" VERSION="$VERSION" \
    sh "$REPO/os/selfhost/build-installed-image.sh"
fi

echo "===== build ISO ====="
ISODIR="$WORK/iso"
mkdir -p "$ISODIR/boot/grub"
cp "$WORK/bzImage" "$ISODIR/boot/bzImage"
cp "$WORK/initrd.gz" "$ISODIR/boot/initrd.gz"
cat > "$ISODIR/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
menuentry "saltOS $VERSION (self-hosted, graphical display)" {
  linux /boot/bzImage console=tty1 rdinit=/sbin/runit-init
  initrd /boot/initrd.gz
}
menuentry "saltOS $VERSION (self-hosted, serial console)" {
  linux /boot/bzImage console=ttyS0,115200 rdinit=/sbin/runit-init
  initrd /boot/initrd.gz
}
EOF
ISO_PATH="$OUT/saltos-$VERSION-selfhost-$ARCH.iso"
grub-mkrescue -o "$ISO_PATH" "$ISODIR"
echo "wrote $ISO_PATH"
ls -lh "$ISO_PATH"
