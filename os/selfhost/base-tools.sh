set -euxo pipefail

UTIL_LINUX_VER="2.40.2"
E2FSPROGS_VER="1.47.1"
NCURSES_VER="6.5"

export PKG_CONFIG_PATH="$T/usr/lib/pkgconfig:$T/usr/share/pkgconfig"
export CPPFLAGS="-I$T/usr/include"
export LDFLAGS="-L$T/usr/lib -Wl,-rpath-link,$T/usr/lib"
export PATH="$T/usr/bin:$PATH"

mkdir -p "$T/.salt-done"
toolsdone() { [ -f "$T/.salt-done/$1" ]; }
toolsmark() { mkdir -p "$T/.salt-done"; touch "$T/.salt-done/$1"; }

cd "$SRC"

if ! toolsdone ncurses; then
  # --enable-fdisks turns on cfdisk too, and configure.ac assigns
  # enable_cfdisk=$enable_fdisks unconditionally so it cannot be switched off
  # on its own. cfdisk hard-requires curses, so build ncurses rather than
  # linking against the container's copy, which is absent from the rootfs.
  # The terminfo database it installs is worth having on a console-only image.
  echo "===== ncurses ====="
  [ -f "ncurses-${NCURSES_VER}.tar.gz" ] || fetch \
    "https://ftp.gnu.org/gnu/ncurses/ncurses-${NCURSES_VER}.tar.gz" \
    "ncurses-${NCURSES_VER}.tar.gz"
  rm -rf "ncurses-${NCURSES_VER}"
  tar -xf "ncurses-${NCURSES_VER}.tar.gz"
  ( cd "ncurses-${NCURSES_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib \
      --with-shared --without-debug --without-ada --without-manpages \
      --enable-widec --enable-pc-files \
      --with-pkg-config-libdir=/usr/lib/pkgconfig \
      --enable-overwrite
    make -j"$JOBS"
    make DESTDIR="$T" install )
  find "$T" -name '*.la' -delete 2>/dev/null || true
  [ -e "$T/usr/lib/libncursesw.so" ] || { echo "FATAL: ncurses did not install"; exit 1; }
  toolsmark ncurses
fi

if ! toolsdone util-linux; then
  echo "===== util-linux (sfdisk, blkid, lsblk) ====="
  ul_series="$(echo "$UTIL_LINUX_VER" | cut -d. -f1,2)"
  [ -f "util-linux-${UTIL_LINUX_VER}.tar.xz" ] || fetch \
    "https://www.kernel.org/pub/linux/utils/util-linux/v${ul_series}/util-linux-${UTIL_LINUX_VER}.tar.xz" \
    "util-linux-${UTIL_LINUX_VER}.tar.xz"
  rm -rf "util-linux-${UTIL_LINUX_VER}"
  tar -xf "util-linux-${UTIL_LINUX_VER}.tar.xz"
  ( cd "util-linux-${UTIL_LINUX_VER}"
    # The option is --enable-fdisks, plural: configure.ac sets
    # enable_sfdisk=$enable_fdisks and defines no --enable-sfdisk at all, so
    # that spelling is silently ignored. lsblk has no AC_ARG_ENABLE whatsoever
    # and can only be re-enabled through the variable.
    ./configure --prefix=/usr --libdir=/usr/lib \
      --disable-all-programs \
      --enable-libblkid --enable-libuuid --enable-libmount --enable-libsmartcols \
      --enable-libfdisk \
      --enable-fdisks --enable-blkid --enable-partx --enable-wipefs \
      --enable-mount --enable-lscpu \
      --disable-static --without-python --without-systemd --without-udev \
      enable_lsblk=yes
    make -j"$JOBS"
    make DESTDIR="$T" install )
  find "$T" -name '*.la' -delete 2>/dev/null || true
  for prog in sfdisk blkid wipefs; do
    [ -e "$T/usr/sbin/$prog" ] || [ -e "$T/usr/bin/$prog" ] \
      || { echo "FATAL: util-linux did not build $prog"; exit 1; }
  done
  [ -e "$T/usr/bin/lsblk" ] || echo "note: lsblk was not built (optional)"
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
