local version = "2.1.0"

return {
  name = "lxqt-qtplugin",
  version = version,
  release = 1,
  summary = "LXQt Qt platform integration plugin",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/lxqt-qtplugin/releases/download/" .. version .. "/lxqt-qtplugin-" .. version .. ".tar.xz",
    sha256 = "0b52c779e6d6e43c0b942bfe14372c7d7ba7fb0eb17b86e10720505b01a1383e",
  },
  build = {
    system = "cmake",
    deps = {
      "cmake",
      "ninja",
      "pkgconf",
      "gcc",
      "qt6-base",
      "qt6-tools",
      "libqtxdg",
      "libfm-qt",
      "libdbusmenu-lxqt",
      "lxqt-build-tools",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "libqtxdg", "libfm-qt", "libdbusmenu-lxqt" },
  },
  reproducibility = {
    status = "verified",
  },
}
