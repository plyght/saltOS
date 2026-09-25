local version = "2024.1"

return {
  name = "xorgproto",
  version = version,
  release = 1,
  summary = "X Window System unified protocol headers",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/proto/xorgproto-" .. version .. ".tar.xz",
    sha256 = "372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "util-macros" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dlegacy=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
