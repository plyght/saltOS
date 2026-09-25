local version = "1.23.0"

return {
  name = "wayland",
  version = version,
  release = 1,
  summary = "Wayland display protocol libraries",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://gitlab.freedesktop.org/wayland/wayland/-/releases/" .. version .. "/downloads/wayland-" .. version .. ".tar.xz",
    sha256 = "05b3e1574d3e67626b5974f862f36b5b427c7ceeb965cb36a4e6c2d342e45ab2",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "libffi", "expat", "libxml2" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Ddocumentation=false -Dtests=false -Ddtd_validation=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "libffi", "expat" },
  },
  reproducibility = {
    status = "verified",
  },
}
