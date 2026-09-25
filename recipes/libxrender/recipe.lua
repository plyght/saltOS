local version = "0.9.11"

return {
  name = "libxrender",
  version = version,
  release = 1,
  summary = "X11 libXrender library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXrender-" .. version .. ".tar.xz",
    sha256 = "bc53759a3a83d1ff702fb59641b3d2f7c56e05051fa0cfa93501166fa782dc24",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "xorgproto" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11" },
  },
  reproducibility = {
    status = "verified",
  },
}
