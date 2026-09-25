local version = "6.7.2"

return {
  name = "qt6-declarative",
  version = version,
  release = 1,
  summary = "Qt Quick and QML",
  license = "LGPL-3.0-only",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.qt.io/archive/qt/6.7/" .. version .. "/submodules/qtdeclarative-everywhere-src-" .. version .. ".tar.xz",
    sha256 = "4c29cba1af8c42d425d8eb6e01bad24cb80f4b983d71eef566a0542dfdb9b999",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "qt6-svg", "qt6-shadertools", "python" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "qt6-svg" },
  },
  reproducibility = {
    status = "verified",
  },
}
