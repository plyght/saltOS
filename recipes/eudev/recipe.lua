local version = "3.2.14"

return {
  name = "eudev",
  version = version,
  release = 2,
  summary = "Standalone fork of udev device manager",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/eudev-project/eudev/releases/download/v" .. version .. "/eudev-" .. version .. ".tar.gz",
    sha256 = "8da4319102f24abbf7fff5ce9c416af848df163b29590e666d334cc1927f006f",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "gperf", "kmod" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --sysconfdir=/etc \
    --disable-manpages \
    --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "kmod" },
  },
  reproducibility = {
    status = "verified",
  },
}
