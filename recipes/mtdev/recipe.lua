local version = "1.1.7"

return {
  name = "mtdev",
  version = version,
  release = 1,
  summary = "Multitouch protocol translation library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://bitmath.org/code/mtdev/mtdev-" .. version .. ".tar.bz2",
    sha256 = "a107adad2101fecac54ac7f9f0e0a0dd155d954193da55c2340c97f2ff1d814e",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make" },
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
