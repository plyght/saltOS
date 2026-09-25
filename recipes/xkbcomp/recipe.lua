local version = "1.4.7"

return {
  name = "xkbcomp",
  version = version,
  release = 1,
  summary = "XKB keymap compiler",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/app/xkbcomp-" .. version .. ".tar.xz",
    sha256 = "0a288114e5f44e31987042c79aecff1ffad53a8154b8ec971c24a69a80f81f77",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libxkbfile", "util-macros" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libxkbfile" },
  },
  reproducibility = {
    status = "verified",
  },
}
