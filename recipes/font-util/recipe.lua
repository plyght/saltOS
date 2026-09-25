local version = "1.4.1"

return {
  name = "font-util",
  version = version,
  release = 1,
  summary = "X.org font utilities (build macros and encodings)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/font/font-util-" .. version .. ".tar.xz",
    sha256 = "5c9f64123c194b150fee89049991687386e6ff36ef2af7b80ba53efaf368cc95",
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
