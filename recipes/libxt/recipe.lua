local version = "1.3.1"

return {
  name = "libxt",
  version = version,
  release = 1,
  summary = "X Toolkit Intrinsics library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXt-" .. version .. ".tar.xz",
    sha256 = "e0a774b33324f4d4c05b199ea45050f87206586d81655f8bef4dba434d931288",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libsm", "libice" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libsm", "libice" },
  },
  reproducibility = {
    status = "verified",
  },
}
