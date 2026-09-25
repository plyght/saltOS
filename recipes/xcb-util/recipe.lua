local version = "0.4.1"

return {
  name = "xcb-util",
  version = version,
  release = 1,
  summary = "XCB utility library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://xorg.freedesktop.org/archive/individual/lib/xcb-util-" .. version .. ".tar.xz",
    sha256 = "5abe3bbbd8e54f0fa3ec945291b7e8fa8cfd3cccc43718f8758430f94126e512",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libxcb", "util-macros" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libxcb" },
  },
  reproducibility = {
    status = "verified",
  },
}
