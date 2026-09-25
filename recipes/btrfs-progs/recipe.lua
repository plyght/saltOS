local version = "6.9.2"

return {
  name = "btrfs-progs",
  version = version,
  release = 2,
  summary = "Userspace utilities for the btrfs filesystem",
  license = "GPL-2.0-only",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/btrfs-progs-v" .. version .. ".tar.xz",
    sha256 = "43865bb272dc0ab2585de3605434d81ba217578f0897bf700cd36c14ac40652a",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "util-linux", "lzo", "zlib", "zstd" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --disable-static \
    --disable-documentation \
    --disable-python
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "util-linux", "lzo", "zlib", "zstd" },
  },
  reproducibility = {
    status = "verified",
  },
}
