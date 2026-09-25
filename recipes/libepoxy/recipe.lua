local version = "1.5.10"

return {
  name = "libepoxy",
  version = version,
  release = 1,
  summary = "OpenGL function pointer management library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.gnome.org/sources/libepoxy/1.5/libepoxy-" .. version .. ".tar.xz",
    sha256 = "072cda4b59dd098bba8c2363a6247299db1fa89411dc221c8b81b8ee8192e623",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "mesa", "libx11" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Ddocs=false -Dtests=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "mesa", "libx11" },
  },
  reproducibility = {
    status = "verified",
  },
}
