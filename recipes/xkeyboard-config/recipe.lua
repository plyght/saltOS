local version = "2.41"

return {
  name = "xkeyboard-config",
  version = version,
  release = 1,
  summary = "X keyboard configuration database",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-" .. version .. ".tar.xz",
    sha256 = "f02cd6b957295e0d50236a3db15825256c92f67ef1f73bf1c77a4b179edf728f",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "python", "perl", "gettext" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dxorg-rules-symlinks=true
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
