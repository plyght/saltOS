local version = "4.1.0"

return {
  name = "qtxdg-tools",
  version = version,
  release = 1,
  summary = "libqtxdg user tools (qtxdg-mat)",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/qtxdg-tools/releases/download/" .. version .. "/qtxdg-tools-" .. version .. ".tar.xz",
    sha256 = "dbd59b7641091a226fb58222e11b4aeb36e6e65dc235280897d066e59fa966b6",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "libqtxdg", "lxqt-build-tools" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "libqtxdg" },
  },
  reproducibility = {
    status = "verified",
  },
}
