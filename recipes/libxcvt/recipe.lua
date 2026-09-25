local version = "0.1.2"

return {
  name = "libxcvt",
  version = version,
  release = 1,
  summary = "VESA CVT modeline calculator",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libxcvt-" .. version .. ".tar.xz",
    sha256 = "0561690544796e25cfbd71806ba1b0d797ffe464e9796411123e79450f71db38",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared
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
