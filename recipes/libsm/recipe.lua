local version = "1.2.5"

return {
  name = "libsm",
  version = version,
  release = 1,
  summary = "X11 Session Management library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libSM-" .. version .. ".tar.xz",
    sha256 = "2af9e12da5ef670dc3a7bce1895c9c0f1bfb0cb9e64e8db40fcc33f883bd20bc",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libice", "util-linux", "xtrans" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libice", "util-linux" },
  },
  reproducibility = {
    status = "verified",
  },
}
