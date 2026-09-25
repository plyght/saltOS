local version = "1.0.8"

return {
  name = "bzip2",
  version = version,
  release = 2,
  summary = "Freely available high-quality block-sorting file compressor",
  license = "bzip2-1.0.6",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://sourceware.org/pub/bzip2/bzip2-" .. version .. ".tar.gz",
    sha256 = "ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269",
  },
  build = {
    system = "make",
    deps = { "gcc", "make", "glibc" },
    script = [[
#!/bin/sh
make -f Makefile-libbz2_so CFLAGS="-fPIC -Wall -Winline -O2 -g -D_FILE_OFFSET_BITS=64"
make clean
make -j"$SALT_JOBS" CFLAGS="-fPIC -Wall -Winline -O2 -g -D_FILE_OFFSET_BITS=64"
make PREFIX="$SALT_DEST/usr" install
version=$(sed -n 's/^DISTNAME=bzip2-//p' Makefile)
cp -a libbz2.so.* "$SALT_DEST/usr/lib/"
ln -sf "libbz2.so.$version" "$SALT_DEST/usr/lib/libbz2.so"
ln -sf bzip2 "$SALT_DEST/usr/bin/bunzip2"
ln -sf bzip2 "$SALT_DEST/usr/bin/bzcat"
ln -sf bzdiff "$SALT_DEST/usr/bin/bzcmp"
ln -sf bzgrep "$SALT_DEST/usr/bin/bzegrep"
ln -sf bzgrep "$SALT_DEST/usr/bin/bzfgrep"
ln -sf bzmore "$SALT_DEST/usr/bin/bzless"
ln -sf bzdiff.1 "$SALT_DEST/usr/man/man1/bzcmp.1"
ln -sf bzgrep.1 "$SALT_DEST/usr/man/man1/bzegrep.1"
ln -sf bzgrep.1 "$SALT_DEST/usr/man/man1/bzfgrep.1"
ln -sf bzmore.1 "$SALT_DEST/usr/man/man1/bzless.1"
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
