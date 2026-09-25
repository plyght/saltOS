local version = "1.5.2"

return {
  name = "xtrans",
  version = version,
  release = 1,
  summary = "X transport library (headers)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/xtrans-" .. version .. ".tar.xz",
    sha256 = "5c5cbfe34764a9131d048f03c31c19e57fb4c682d67713eab6a65541b4dff86c",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "util-macros" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
