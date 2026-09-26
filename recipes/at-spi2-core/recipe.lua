local version = "2.52.0"

return {
  name = "at-spi2-core",
  version = version,
  release = 1,
  summary = "Assistive Technology Service Provider Interface, ATK and the ATK bridge",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.gnome.org/sources/at-spi2-core/2.52/at-spi2-core-" .. version .. ".tar.xz",
    sha256 = "0ac3fc8320c8d01fa147c272ba7fa03806389c6b03d3c406d0823e30e35ff5ab",
  },
  build = {
    system = "meson",
    deps = { "meson", "ninja", "pkgconf", "gcc", "glib", "dbus", "libxml2", "libx11", "libxtst", "libxi" },
    script = [[
#!/bin/sh
set -e
meson setup build --prefix=/usr --libdir=lib --sysconfdir=/etc --buildtype=release \
  -Duse_systemd=false -Dgtk2_atk_adaptor=false -Dintrospection=disabled -Ddocs=false \
  -Dx11=enabled -Ddbus_daemon=/usr/bin/dbus-daemon
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "glib", "dbus", "libxml2", "libx11", "libxtst", "libxi" },
  },
  reproducibility = {
    status = "verified",
  },
}
