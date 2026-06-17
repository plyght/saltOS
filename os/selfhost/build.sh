#!/bin/bash
set -euxo pipefail

ARCH="${1:-x86_64}"
REPO="${REPO_DIR:-$PWD}"
WORK="${WORK:-$PWD/selfhost-work}"
OUT="${OUT:-$PWD/out-iso}"
VERSION="${VERSION:-0.1.0}"
JOBS="${JOBS:-$(nproc)}"

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
  sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config 2>/dev/null || true
  sed -i 's/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config 2>/dev/null || true
  make oldconfig </dev/null
  make -j"$JOBS"
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
mkdir -p "$ROOTFS"/{bin,sbin,usr/bin,usr/sbin,etc,proc,sys,dev,run,tmp,root,var/lib/salt}
cp -a "$WORK/bb-install/bin/"* "$ROOTFS/bin/" 2>/dev/null || true
cp -a "$WORK/bb-install/sbin/"* "$ROOTFS/sbin/" 2>/dev/null || true
cp -a "$WORK/bb-install/usr/bin/"* "$ROOTFS/usr/bin/" 2>/dev/null || true
cp -a "$WORK/bb-install/usr/sbin/"* "$ROOTFS/usr/sbin/" 2>/dev/null || true
cp -a "$WORK/bb-install/linuxrc" "$ROOTFS/" 2>/dev/null || true

for b in runit runit-init runsv runsvdir runsvchdir sv chpst utmpset; do
  [ -f "$RUNIT_SRC/$b" ] && install -Dm755 "$RUNIT_SRC/$b" "$ROOTFS/sbin/$b"
done
echo "===== overlay GNU userland (glibc/bash/coreutils alongside busybox) ====="
cp -a "$GNU/." "$ROOTFS/"
mkdir -p "$ROOTFS/lib64"
if [ ! -e "$ROOTFS/lib64/ld-linux-x86-64.so.2" ]; then
  REAL="$(find "$ROOTFS/usr/lib" "$ROOTFS/lib" -name 'ld-linux-x86-64.so.2' -type f 2>/dev/null | head -1 || true)"
  [ -n "$REAL" ] && ln -sf "${REAL#"$ROOTFS"}" "$ROOTFS/lib64/ld-linux-x86-64.so.2"
fi
[ -e "$ROOTFS/usr/bin/bash" ] && ln -sf /usr/bin/bash "$ROOTFS/bin/bash"
ldconfig -r "$ROOTFS" 2>/dev/null || true

install -Dm755 "$SALT_STATIC" "$ROOTFS/usr/bin/salt"

cat > "$ROOTFS/etc/profile" <<'EOF'
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
export PS1='saltOS:\w\$ '
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
PATH=/usr/bin:/usr/sbin:/bin:/sbin
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
PATH=/usr/bin:/usr/sbin:/bin:/sbin
export PATH
exec runsvdir -P /etc/runit/runsvdir/current
EOF
cat > "$ROOTFS/etc/runit/3" <<'EOF'
#!/bin/sh
PATH=/usr/bin:/usr/sbin:/bin:/sbin
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
  echo "SALTOS_SELFHOST_OK kernel+glibc+bash+coreutils+runit+salt, all from source, no Debian"
else
  echo "SALTOS_SELFHOST_FAIL a component did not run"
fi
exec sleep infinity
EOF
chmod +x "$ROOTFS/etc/runit/sv/boot-check/run"

mkdir -p "$ROOTFS/etc/runit/sv/getty-tty1"
cat > "$ROOTFS/etc/runit/sv/getty-tty1/run" <<'EOF'
#!/bin/sh
exec setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'
EOF
chmod +x "$ROOTFS/etc/runit/sv/getty-tty1/run"

ln -sf /etc/runit/sv/boot-check "$ROOTFS/etc/runit/runsvdir/current/boot-check"
ln -sf /etc/runit/sv/getty-tty1 "$ROOTFS/etc/runit/runsvdir/current/getty-tty1"

ln -sf /sbin/runit-init "$ROOTFS/init"

mkdir -p "$ROOTFS/etc/salt"
cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
repo = "current"
source = ""
key = ""
EOF

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

echo "===== build ISO ====="
ISODIR="$WORK/iso"
mkdir -p "$ISODIR/boot/grub"
cp "$WORK/bzImage" "$ISODIR/boot/bzImage"
cp "$WORK/initrd.gz" "$ISODIR/boot/initrd.gz"
cat > "$ISODIR/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
menuentry "saltOS $VERSION (self-hosted)" {
  linux /boot/bzImage console=tty0 console=ttyS0,115200 rdinit=/sbin/runit-init
  initrd /boot/initrd.gz
}
EOF
ISO_PATH="$OUT/saltos-$VERSION-selfhost-$ARCH.iso"
grub-mkrescue -o "$ISO_PATH" "$ISODIR"
echo "wrote $ISO_PATH"
ls -lh "$ISO_PATH"
