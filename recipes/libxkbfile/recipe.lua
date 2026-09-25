local version = "1.1.3"

return {
  name = "libxkbfile",
  version = version,
  release = 1,
  summary = "X keyboard file manipulation library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libxkbfile-" .. version .. ".tar.xz",
    sha256 = "a9b63eea997abb9ee6a8b4fbb515831c841f471af845a09de443b28003874bec",
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
