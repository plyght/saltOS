local version = "3.30.5"

return {
  name = "cmake",
  version = version,
  release = 2,
  summary = "Cross-platform build system generator",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/Kitware/CMake/releases/download/v" .. version .. "/cmake-" .. version .. ".tar.gz",
    sha256 = "9f55e1a40508f2f29b7e065fa08c29f82c402fa0402da839fffe64a25755a86d",
  },
  build = {
    system = "custom",
    deps = { "gcc", "make", "openssl", "zlib", "ncurses", "bzip2", "xz", "zstd", "expat" },
    script = [[
#!/bin/sh
./bootstrap --prefix=/usr --parallel="$SALT_JOBS" --no-qt-gui \
    --no-system-libs --system-zlib --system-bzip2 --system-liblzma \
    --system-zstd --system-expat \
    -- -DCMAKE_BUILD_TYPE=Release
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "openssl", "zlib", "ncurses", "bzip2", "xz", "zstd", "expat" },
  },
  reproducibility = {
    status = "verified",
  },
}
