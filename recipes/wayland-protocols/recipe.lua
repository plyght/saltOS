local version = "1.37"

return {
  name = "wayland-protocols",
  version = version,
  release = 1,
  summary = "Wayland protocol extensions",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/" .. version .. "/downloads/wayland-protocols-" .. version .. ".tar.xz",
    sha256 = "a70e9be924f2e8688e6824dceaf6188faacd5ae218dfac8d0a3d0976211ef326",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "wayland" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dtests=false
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
