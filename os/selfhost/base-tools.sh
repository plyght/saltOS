set -euxo pipefail

UTIL_LINUX_VER="2.40.2"
E2FSPROGS_VER="1.47.1"

export PKG_CONFIG_PATH="$T/usr/lib/pkgconfig:$T/usr/share/pkgconfig"
export CPPFLAGS="-I$T/usr/include"
export LDFLAGS="-L$T/usr/lib -Wl,-rpath-link,$T/usr/lib"
export PATH="$T/usr/bin:$PATH"

mkdir -p "$T/.salt-done"
toolsdone() { [ -f "$T/.salt-done/$1" ]; }
toolsmark() { mkdir -p "$T/.salt-done"; touch "$T/.salt-done/$1"; }

cd "$SRC"

if ! toolsdone util-linux; then
  echo "===== util-linux (sfdisk, blkid, lsblk) ====="
  ul_series="$(echo "$UTIL_LINUX_VER" | cut -d. -f1,2)"
  [ -f "util-linux-${UTIL_LINUX_VER}.tar.xz" ] || fetch \
    "https://www.kernel.org/pub/linux/utils/util-linux/v${ul_series}/util-linux-${UTIL_LINUX_VER}.tar.xz" \
    "util-linux-${UTIL_LINUX_VER}.tar.xz"
  rm -rf "util-linux-${UTIL_LINUX_VER}"
  tar -xf "util-linux-${UTIL_LINUX_VER}.tar.xz"
  ( cd "util-linux-${UTIL_LINUX_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib \
      --disable-all-programs \
      --enable-libblkid --enable-libuuid --enable-libmount --enable-libsmartcols \
      --enable-sfdisk --enable-blkid --enable-lsblk --enable-partx --enable-wipefs \
      --enable-fdisk --enable-mount --enable-lscpu \
      --disable-static --without-python --without-systemd --without-udev
    make -j"$JOBS"
    make DESTDIR="$T" install )
  toolsmark util-linux
fi

if ! toolsdone e2fsprogs; then
  echo "===== e2fsprogs (real mkfs.ext4) ====="
  [ -f "e2fsprogs-${E2FSPROGS_VER}.tar.xz" ] || fetch \
    "https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v${E2FSPROGS_VER}/e2fsprogs-${E2FSPROGS_VER}.tar.xz" \
    "e2fsprogs-${E2FSPROGS_VER}.tar.xz"
  rm -rf "e2fsprogs-${E2FSPROGS_VER}"
  tar -xf "e2fsprogs-${E2FSPROGS_VER}.tar.xz"
  mkdir -p "$SRC/e2fsprogs-build"
  ( cd "$SRC/e2fsprogs-build"
    "$SRC/e2fsprogs-${E2FSPROGS_VER}/configure" \
      --prefix=/usr --libdir=/usr/lib --sbindir=/usr/sbin \
      --enable-elf-shlibs --disable-fsck --disable-uuidd \
      --disable-libblkid --disable-libuuid
    make -j"$JOBS"
    make DESTDIR="$T" install
    make DESTDIR="$T" install-libs )
  toolsmark e2fsprogs
fi

find "$T" -name '*.la' -delete 2>/dev/null || true
