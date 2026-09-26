local version = "1.7.2"

return {
  name = "hunspell",
  version = version,
  release = 1,
  summary = "Spell checker and morphological analyzer library",
  license = "MPL-1.1 OR GPL-2.0-or-later OR LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/hunspell/hunspell/releases/download/v" .. version .. "/hunspell-" .. version .. ".tar.gz",
    sha256 = "11ddfa39afe28c28539fe65fc4f1592d410c1e9b6dd7d8a91ca25d85e9ec65b8",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make" },
    script = [[
#!/bin/sh
set -e
./configure --prefix=/usr --disable-static --without-ui --without-readline --disable-nls
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
