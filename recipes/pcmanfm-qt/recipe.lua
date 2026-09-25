local version = "2.1.0"

return {
  name = "pcmanfm-qt",
  version = version,
  release = 2,
  summary = "File manager and desktop icon manager (LXQt)",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/pcmanfm-qt/releases/download/" .. version .. "/pcmanfm-qt-" .. version .. ".tar.xz",
    sha256 = "e63486571dfa1bc476785f0d881e2138c736708009589c05a93ab24575e06b4f",
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
      "libfm-qt",
      "layer-shell-qt",
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
    deps = { "glibc", "qt6-base", "libfm-qt", "layer-shell-qt" },
  },
  reproducibility = {
    status = "verified",
  },
}
