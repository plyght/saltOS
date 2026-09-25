local version = "2.3.8"

return {
  name = "libxft",
  version = version,
  release = 1,
  summary = "X FreeType font rendering library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXft-" .. version .. ".tar.xz",
    sha256 = "5e8c3c4bc2d4c0a40aef6b4b38ed2fb74301640da29f6528154b5009b1c6dd49",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libx11", "libxrender", "freetype", "fontconfig" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libx11", "libxrender", "freetype", "fontconfig" },
  },
  reproducibility = {
    status = "verified",
  },
}
