local version = "1.2.5"

return {
  name = "libxtst",
  version = version,
  release = 1,
  summary = "X11 libXtst library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXtst-" .. version .. ".tar.xz",
    sha256 = "b50d4c25b97009a744706c1039c598f4d8e64910c9fde381994e1cae235d9242",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libxext", "libxi", "xorgproto" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libxext", "libxi" },
  },
  reproducibility = {
    status = "verified",
  },
}
