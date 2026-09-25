local version = "1.17.0"

return {
  name = "xcb-proto",
  version = version,
  release = 1,
  summary = "XML-XCB protocol descriptions and xcbgen",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-" .. version .. ".tar.xz",
    sha256 = "2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "python" },
    script = [[
#!/bin/sh
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --disable-static
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "python" },
  },
  reproducibility = {
    status = "verified",
  },
}
