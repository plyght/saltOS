local version = "1.2.1"

return {
  name = "libxmu",
  version = version,
  release = 1,
  summary = "X miscellaneous utilities library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXmu-" .. version .. ".tar.xz",
    sha256 = "fcb27793248a39e5fcc5b9c4aec40cc0734b3ca76aac3d7d1c264e7f7e14e8b2",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libxext", "libxt" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libxext", "libxt" },
  },
  reproducibility = {
    status = "verified",
  },
}
