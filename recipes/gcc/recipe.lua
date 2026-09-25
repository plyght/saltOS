local version = "14.2.0"

return {
  name = "gcc",
  version = version,
  release = 5,
  summary = "GNU Compiler Collection",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/gcc/gcc-" .. version .. "/gcc-" .. version .. ".tar.xz",
    sha256 = "a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9",
  },
  build = {
    system = "autotools",
    deps = { "gmp", "mpfr", "mpc", "binutils", "glibc", "make" },
    script = [[
#!/bin/sh
sed -e '/m64=/s/lib64/lib/' -i gcc/config/i386/t-linux64
sed -e '/lp64=/s/lib64/lib/' -i gcc/config/aarch64/t-aarch64-linux
mkdir -p build
cd build
../configure --prefix=/usr \
    --enable-languages=c,c++ \
    --enable-default-pie \
    --enable-default-ssp \
    --disable-multilib \
    --disable-bootstrap
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
rm -rf "$SALT_DEST"/usr/lib/gcc/*/*/include-fixed
ln -sf gcc "$SALT_DEST/usr/bin/cc"
]],
  },
  package = {
    deps = { "gmp", "mpfr", "mpc", "binutils", "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
