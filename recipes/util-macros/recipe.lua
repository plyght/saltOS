local version = "1.20.1"

return {
  name = "util-macros",
  version = version,
  release = 1,
  summary = "X.org autotools macros",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/util/util-macros-" .. version .. ".tar.xz",
    sha256 = "0b308f62dce78ac0f4d9de6888234bf170f276b64ac7c96e99779bb4319bcef5",
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
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
