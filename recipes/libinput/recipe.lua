local version = "1.26.2"

return {
  name = "libinput",
  version = version,
  release = 1,
  summary = "Input device handling library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://gitlab.freedesktop.org/libinput/libinput/-/archive/" .. version .. "/libinput-" .. version .. ".tar.bz2",
    sha256 = "e2dbbf515905086dc3f8c8536d326e04012f5716b8b047bb3392a17b13ca78ec",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "eudev", "mtdev", "libevdev" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dlibwacom=false -Ddebug-gui=false -Dtests=false -Ddocumentation=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "eudev", "mtdev", "libevdev" },
  },
  reproducibility = {
    status = "verified",
  },
}
