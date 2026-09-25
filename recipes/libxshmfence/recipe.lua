local version = "1.3.2"

return {
  name = "libxshmfence",
  version = version,
  release = 1,
  summary = "X shared memory fences",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.x.org/releases/individual/lib/libxshmfence-" .. version .. ".tar.xz",
    sha256 = "870df257bc40b126d91b5a8f1da6ca8a524555268c50b59c0acd1a27f361606f",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "xorgproto" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
