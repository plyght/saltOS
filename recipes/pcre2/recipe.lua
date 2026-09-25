local version = "10.44"

return {
  name = "pcre2",
  version = version,
  release = 2,
  summary = "Perl Compatible Regular Expressions library version 2",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-" .. version .. "/pcre2-" .. version .. ".tar.bz2",
    sha256 = "d34f02e113cf7193a1ebf2770d3ac527088d485d4e047ed10e5d217c6ef5de96",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "zlib" },
    script = [[
#!/bin/sh
# 16-bit code units are required by Qt (pcre2-16); JIT for speed.
./configure --prefix=/usr --disable-static --enable-pcre2-16 --enable-pcre2-32 --enable-jit
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
