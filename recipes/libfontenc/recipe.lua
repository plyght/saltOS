local version = "1.1.8"

return {
  name = "libfontenc",
  version = version,
  release = 1,
  summary = "X font encoding library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libfontenc-" .. version .. ".tar.xz",
    sha256 = "7b02c3d405236e0d86806b1de9d6868fe60c313628b38350b032914aa4fd14c6",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "xorgproto", "zlib" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "zlib" },
  },
  reproducibility = {
    status = "verified",
  },
}
