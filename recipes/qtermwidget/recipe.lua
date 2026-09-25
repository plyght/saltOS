local version = "2.1.0"

return {
  name = "qtermwidget",
  version = version,
  release = 2,
  summary = "Terminal emulator widget for Qt",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/qtermwidget/releases/download/" .. version .. "/qtermwidget-" .. version .. ".tar.xz",
    sha256 = "26ecb2c3a38de10db3b5ae24970957a33520f7742e3ae1ff93d345ded2be8e4b",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "qt6-tools", "lxqt-build-tools" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base" },
  },
  reproducibility = {
    status = "verified",
  },
}
