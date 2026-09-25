local version = "2.0.7"

return {
  name = "libxfont2",
  version = version,
  release = 1,
  summary = "X font rasterisation library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libXfont2-" .. version .. ".tar.xz",
    sha256 = "8b7b82fdeba48769b69433e8e3fbb984a5f6bf368b0d5f47abeec49de3e58efb",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "libfontenc", "freetype", "xtrans", "xorgproto", "zlib" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libfontenc", "freetype", "zlib" },
  },
  reproducibility = {
    status = "verified",
  },
}
