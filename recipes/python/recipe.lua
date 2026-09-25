local version = "3.12.7"

return {
  name = "python",
  version = version,
  release = 1,
  summary = "Python programming language interpreter",
  license = "Python-2.0.1",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.python.org/ftp/python/" .. version .. "/Python-" .. version .. ".tar.xz",
    sha256 = "24887b92e2afd4a2ac602419ad4b596372f67ac9b077190f459aba390faf5550",
  },
  build = {
    system = "autotools",
    deps = {
      "gcc",
      "make",
      "openssl",
      "zlib",
      "libffi",
      "expat",
      "ncurses",
      "readline",
      "sqlite",
      "bzip2",
      "xz",
      "libxcrypt",
    },
    script = [[
#!/bin/sh
./configure --prefix=/usr --enable-shared --with-system-expat --with-system-ffi --enable-optimizations --without-ensurepip
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
ln -sf python3 "$SALT_DEST/usr/bin/python"
]],
  },
  package = {
    deps = {
      "glibc",
      "openssl",
      "zlib",
      "libffi",
      "expat",
      "ncurses",
      "readline",
      "sqlite",
      "bzip2",
      "xz",
      "libxcrypt",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
