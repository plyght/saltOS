local version = "1.1.5"

return {
  name = "libxxf86vm",
  version = version,
  release = 1,
  summary = "X11 libXxf86vm library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXxf86vm-" .. version .. ".tar.xz",
    sha256 = "247fef48b3e0e7e67129e41f1e789e8d006ba47dba1c0cdce684b9b703f888e7",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libxext", "xorgproto" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libxext" },
  },
  reproducibility = {
    status = "verified",
  },
}
