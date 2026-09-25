local version = "4.36"

return {
  name = "nspr",
  version = version,
  release = 1,
  summary = "Netscape Portable Runtime",
  license = "MPL-2.0",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://archive.mozilla.org/pub/nspr/releases/v" .. version .. "/src/nspr-" .. version .. ".tar.gz",
    sha256 = "55dec317f1401cd2e5dba844d340b930ab7547f818179a4002bce62e6f1c6895",
  },
  build = {
    system = "custom",
    deps = { "gcc", "make" },
    script = [[
#!/bin/sh
cd nspr
./configure --prefix=/usr --with-mozilla --with-pthreads --enable-64bit
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
