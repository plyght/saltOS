local version = "1.0.16"

return {
  name = "fribidi",
  version = version,
  release = 1,
  summary = "Unicode bidirectional algorithm library",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/fribidi/fribidi/releases/download/v" .. version .. "/fribidi-" .. version .. ".tar.xz",
    sha256 = "1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Ddocs=false -Dtests=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
