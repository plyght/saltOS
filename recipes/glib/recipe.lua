local version = "2.80.4"

return {
  name = "glib",
  version = version,
  release = 2,
  summary = "Low-level core library (GLib, GObject, GIO)",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.gnome.org/sources/glib/2.80/glib-" .. version .. ".tar.xz",
    sha256 = "24e029c5dfc9b44e4573697adf33078a9827c48938555004b3b9096fa4ea034f",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "libffi", "zlib", "pcre2", "python", "python-packaging" },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Dintrospection=disabled -Dtests=false -Dman-pages=disabled -Ddocumentation=false -Dselinux=disabled -Dlibmount=enabled -Dsysprof=disabled
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "libffi", "zlib", "pcre2" },
  },
  reproducibility = {
    status = "verified",
  },
}
