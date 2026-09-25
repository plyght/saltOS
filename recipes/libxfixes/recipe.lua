local version = "6.0.1"

return {
  name = "libxfixes",
  version = version,
  release = 1,
  summary = "X11 libXfixes library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXfixes-" .. version .. ".tar.xz",
    sha256 = "b695f93cd2499421ab02d22744458e650ccc88c1d4c8130d60200213abc02d58",
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
