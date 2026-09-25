local version = "6.7.2"

return {
  name = "qt6-tools",
  version = version,
  release = 2,
  summary = "Qt tools (linguist, qdbus)",
  license = "LGPL-3.0-only",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.qt.io/archive/qt/6.7/" .. version .. "/submodules/qttools-everywhere-src-" .. version .. ".tar.xz",
    sha256 = "58e855ad1b2533094726c8a425766b63a04a0eede2ed85086860e54593aa4b2a",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "qt6-declarative" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DFEATURE_linguist=ON -DFEATURE_qdbus=ON -DFEATURE_assistant=OFF -DFEATURE_designer=OFF -DFEATURE_distancefieldgenerator=OFF -DFEATURE_pixeltool=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF -DFEATURE_qdoc=OFF -DFEATURE_clang=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_qev=OFF
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
