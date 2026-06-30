#!/bin/bash
set -euxo pipefail

ARCH="${1:-x86_64}"
REPO="${REPO_DIR:-$PWD}"
WORK="${WORK:-$PWD/selfhost-work}"
OUT="${OUT:-$PWD/out-iso}"
VERSION="${VERSION:-0.1.0}"
JOBS="${JOBS:-$(nproc)}"
EDITION="${EDITION:-console}"

BUSYBOX_VER="1.36.1"
RUNIT_VER="2.1.2"
ZSTD_VER="1.5.6"
SODIUM_VER="1.0.20"
SQLITE_TAR="sqlite-autoconf-3460000"
KERNEL_VER="6.6.52"
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
iface=""
for p in /sys/class/net/*; do
  n="${p##*/}"
  [ "$n" = "lo" ] && continue
  iface="$n"
  break
done
[ -n "$iface" ] || exec sleep 30
ip link set "$iface" up 2>/dev/null || true
exec udhcpc -f -i "$iface" -s /usr/share/udhcpc/default.script
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

echo "===== pack initramfs ====="
( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$WORK/initrd.gz" )
ls -lh "$WORK/initrd.gz"

echo "===== build kernel ====="
KCACHE="${KCACHE:-}"
if [ -n "$KCACHE" ] && [ -f "$KCACHE/bzImage" ]; then
  echo "using cached kernel from $KCACHE"
  cp "$KCACHE/bzImage" "$WORK/bzImage"
else
  cd "$SRC/linux-${KERNEL_VER}"
  make defconfig
  cat "$REPO/os/selfhost/kernel-${ARCH}.config" >> .config
  make olddefconfig
  make -j"$JOBS" bzImage
  cp arch/x86/boot/bzImage "$WORK/bzImage"
  if [ -n "$KCACHE" ]; then
    mkdir -p "$KCACHE"
    cp "$WORK/bzImage" "$KCACHE/bzImage"
  fi
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
( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$WORK/initrd.gz" )

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
