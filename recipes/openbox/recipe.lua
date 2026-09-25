local version = "3.6.1"

return {
  name = "openbox",
  version = version,
  release = 2,
  summary = "Lightweight X11 window manager (LXQt default)",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "http://openbox.org/dist/openbox/openbox-" .. version .. ".tar.xz",
    sha256 = "abe75855cc5616554ffd47134ad15291fe37ebbebf1a80b69cbde9d670f0e26d",
  },
  build = {
    system = "autotools",
    deps = {
      "pkgconf",
      "gcc",
      "make",
      "libx11",
      "libxft",
      "libxrandr",
      "libxinerama",
      "libxcursor",
      "libxext",
      "libsm",
      "libice",
      "libxml2",
      "pango",
      "glib",
    },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static --disable-imlib2 --disable-startup-notification --disable-librsvg --disable-nls
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = {
      "glibc",
      "libx11",
      "libxft",
      "libxrandr",
      "libxinerama",
      "libxcursor",
      "libxext",
      "libsm",
      "libice",
      "libxml2",
      "pango",
      "glib",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
