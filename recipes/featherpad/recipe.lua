local version = "1.5.1"

return {
  name = "featherpad",
  version = version,
  release = 3,
  summary = "Lightweight Qt plain text editor",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/tsujan/FeatherPad/archive/refs/tags/V" .. version .. ".tar.gz",
    sha256 = "2826b3dc26877284d7bb5b62430e99b4fbcf45862af89549e4b4e9f40b16c3c5",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "qt6-svg", "qt6-tools", "libx11", "hunspell" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_QT5=OFF
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "qt6-svg", "libx11", "hunspell" },
  },
  reproducibility = {
    status = "verified",
  },
}
