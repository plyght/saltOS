local version = "1.3.2"

return {
  name = "libfm-extra",
  version = version,
  release = 2,
  summary = "libfm-extra (used by menu-cache)",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://downloads.sourceforge.net/pcmanfm/libfm-" .. version .. ".tar.xz",
    sha256 = "a5042630304cf8e5d8cff9d565c6bd546f228b48c960153ed366a34e87cad1e5",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "glib", "gettext", "intltool" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static --with-extra-only --with-gtk=no --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "glib" },
  },
  reproducibility = {
    status = "verified",
  },
}
