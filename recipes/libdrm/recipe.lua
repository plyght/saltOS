local version = "2.4.122"

return {
  name = "libdrm",
  version = version,
  release = 2,
  summary = "Direct Rendering Manager userspace library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://dri.freedesktop.org/libdrm/libdrm-" .. version .. ".tar.xz",
    sha256 = "d9f5079b777dffca9300ccc56b10a93588cdfbc9dde2fae111940dfb6292f251",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "libpciaccess" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dudev=true -Dvalgrind=disabled -Dtests=false -Dman-pages=disabled -Dcairo-tests=disabled
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "libpciaccess" },
  },
  reproducibility = {
    status = "verified",
  },
}
