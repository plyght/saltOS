local version = "1.18.0"

return {
  name = "cairo",
  version = version,
  release = 2,
  summary = "2D graphics library with support for multiple output devices",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.cairographics.org/releases/cairo-" .. version .. ".tar.xz",
    sha256 = "243a0736b978a33dee29f9cca7521733b78a65b5418206fef7bd1c3d4cf10b64",
  },
  build = {
    system = "meson",
    deps = {
      "meson",
      "ninja",
      "pkgconf",
      "gcc",
      "freetype",
      "fontconfig",
      "libpng",
      "pixman",
      "glib",
      "libx11",
      "libxext",
      "libxrender",
      "libxcb",
    },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dxlib=enabled -Dxcb=enabled -Dtests=disabled -Dglib=enabled -Dspectre=disabled -Dsymbol-lookup=disabled
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = {
      "glibc",
      "freetype",
      "fontconfig",
      "libpng",
      "pixman",
      "glib",
      "libx11",
      "libxext",
      "libxrender",
      "libxcb",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
