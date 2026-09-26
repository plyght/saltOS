local version = "2.4.10"

return {
  name = "libcups",
  version = version,
  release = 1,
  summary = "CUPS client library (libcups only; no print server)",
  license = "Apache-2.0",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/OpenPrinting/cups/releases/download/v" .. version .. "/cups-" .. version .. "-source.tar.gz",
    sha256 = "d75757c2bc0f7a28b02ee4d52ca9e4b1aa1ba2affe16b985854f5336940e5ad7",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "openssl", "zlib" },
    script = [[
#!/bin/sh
set -e
./configure --prefix=/usr --libdir=/usr/lib --sysconfdir=/etc --localstatedir=/var \
  --with-tls=openssl --with-dnssd=no --with-ondemand=no --with-rcdir=no \
  --disable-dbus --disable-pam --disable-libusb --disable-gssapi --disable-static
make -j"$SALT_JOBS" libs
make DSTROOT="$SALT_DEST" install-headers install-libs
]],
  },
  package = {
    deps = { "glibc", "openssl", "zlib" },
  },
  reproducibility = {
    status = "verified",
  },
}
