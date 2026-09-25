local version = "2.41"

return {
  name = "glibc",
  version = version,
  release = 3,
  summary = "GNU C Library",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/glibc/glibc-" .. version .. ".tar.xz",
    sha256 = "a5a26b22f545d6b7d7b3dd828e11e428f24f4fac43c934fb071b6a7d0828e901",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "binutils", "python", "bison" },
    script = [[
#!/bin/sh
mkdir -p build
cd build
../configure --prefix=/usr \
    --disable-werror \
    --enable-kernel=4.19 \
    --enable-stack-protector=strong \
    libc_cv_slibdir=/usr/lib
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
make DESTDIR="$SALT_DEST" localedata/install-locales \
    SUPPORTED-LOCALES="C.UTF-8/UTF-8 en_US.UTF-8/UTF-8"
case "$SALT_ARCH" in
    x86_64)
        mkdir -p "$SALT_DEST/lib64"
        ln -sf ../usr/lib/ld-linux-x86-64.so.2 "$SALT_DEST/lib64/ld-linux-x86-64.so.2"
        ;;
esac
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
