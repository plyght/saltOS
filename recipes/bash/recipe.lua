local version = "5.2.21"

return {
  name = "bash",
  version = version,
  release = 2,
  summary = "GNU Bourne-Again SHell",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/bash/bash-" .. version .. ".tar.gz",
    sha256 = "c8e31bdc59b69aaffc5b36509905ba3e5cbb12747091d27b4b977f078560d5b8",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "ncurses", "readline", "glibc" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --without-bash-malloc \
    --with-installed-readline \
    --with-curses
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
ln -sf bash "$SALT_DEST/usr/bin/sh"
]],
  },
  package = {
    deps = { "ncurses", "readline", "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
