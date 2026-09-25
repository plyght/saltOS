local version = "1.4.3"

return {
  name = "xorg-xinit",
  version = version,
  release = 2,
  summary = "X Window System initializer (startx, xinit)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/app/xinit-" .. version .. ".tar.xz",
    sha256 = "86409f21a6a31148d2c1c17bf5f2d904eb5ef455f9dc67c49fbd0c10ab18fd5a",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "xorg-server" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "xorg-server", "xauth" },
  },
  reproducibility = {
    status = "verified",
  },
}
