local version = "1.13.3"

return {
  name = "libevdev",
  version = version,
  release = 1,
  summary = "Kernel evdev device wrapper library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.freedesktop.org/software/libevdev/libevdev-" .. version .. ".tar.xz",
    sha256 = "abf1aace86208eebdd5d3550ffded4c8d73bb405b796d51c389c9d0604cbcfbf",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "python" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dtests=disabled -Ddocumentation=disabled
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
