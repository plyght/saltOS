local version = "0.4.1"

return {
  name = "xcb-util-keysyms",
  version = version,
  release = 1,
  summary = "XCB keysym utilities",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://xorg.freedesktop.org/archive/individual/lib/xcb-util-keysyms-" .. version .. ".tar.xz",
    sha256 = "7c260a5294412aed429df1da2f8afd3bd07b7cba3fec772fba15a613a6d5c638",
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
