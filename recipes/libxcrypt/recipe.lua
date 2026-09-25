local version = "4.4.38"

return {
  name = "libxcrypt",
  version = version,
  release = 1,
  summary = "Extended crypt library for one-way password hashing",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/besser82/libxcrypt/releases/download/v" .. version .. "/libxcrypt-" .. version .. ".tar.xz",
    sha256 = "80304b9c306ea799327f01d9a7549bdb28317789182631f1b54f4511b4206dd6",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "perl" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --enable-hashes=strong,glibc \
    --enable-obsolete-api=glibc \
    --disable-static \
    --disable-failure-tokens
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
