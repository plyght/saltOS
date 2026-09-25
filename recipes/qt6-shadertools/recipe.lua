local version = "6.7.2"

return {
  name = "qt6-shadertools",
  version = version,
  release = 1,
  summary = "Qt shader tools (qsb)",
  license = "GPL-3.0-only",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.qt.io/archive/qt/6.7/" .. version .. "/submodules/qtshadertools-everywhere-src-" .. version .. ".tar.xz",
    sha256 = "edfa34c0ac8c00fcaa949df1d8e7a77d89dadd6386e683ce6c3e3b117e2f7cc1",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base" },
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
