local version = "1.54.0"

return {
  name = "pango",
  version = version,
  release = 2,
  summary = "Text layout and rendering library",
  license = "LGPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.gnome.org/sources/pango/1.54/pango-" .. version .. ".tar.xz",
    sha256 = "8a9eed75021ee734d7fc0fdf3a65c3bba51dfefe4ae51a9b414a60c70b2d1ed8",
  },
  build = {
    system = "meson",
    deps = {
      "meson",
      "ninja",
      "pkgconf",
      "gcc",
      "glib",
      "harfbuzz",
      "fontconfig",
      "freetype",
      "cairo",
      "fribidi",
      "libxft",
      "libx11",
      "libxrender",
    },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dxft=enabled -Dcairo=enabled -Dfontconfig=enabled -Dfreetype=enabled -Dintrospection=disabled -Dbuild-testsuite=false -Dbuild-examples=false -Ddocumentation=false -Dman-pages=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = {
      "glibc",
      "glib",
      "harfbuzz",
      "fontconfig",
      "freetype",
      "cairo",
      "fribidi",
      "libxft",
      "libx11",
      "libxrender",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
