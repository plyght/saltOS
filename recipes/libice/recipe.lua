local version = "1.1.2"

return {
  name = "libice",
  version = version,
  release = 1,
  summary = "X11 Inter-Client Exchange library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libICE-" .. version .. ".tar.xz",
    sha256 = "974e4ed414225eb3c716985df9709f4da8d22a67a2890066bc6dfc89ad298625",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "xorgproto", "xtrans" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
