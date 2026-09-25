local version = "0.43.4"

return {
  name = "pixman",
  version = version,
  release = 2,
  summary = "Low-level pixel manipulation library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.cairographics.org/releases/pixman-" .. version .. ".tar.gz",
    sha256 = "a0624db90180c7ddb79fc7a9151093dc37c646d8c38d3f232f767cf64b85a226",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dtests=disabled -Dgtk=disabled -Dlibpng=disabled
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
