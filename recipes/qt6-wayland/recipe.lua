local version = "6.7.2"

return {
  name = "qt6-wayland",
  version = version,
  release = 1,
  summary = "Qt Wayland platform plugin and client library",
  license = "LGPL-3.0-only",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.qt.io/archive/qt/6.7/" .. version .. "/submodules/qtwayland-everywhere-src-" .. version .. ".tar.xz",
    sha256 = "a2a057e1dd644bd44abb9990fecc194b2e25c2e0f39e81aa9fee4c1e5e2a8a5b",
  },
  build = {
    system = "cmake",
    deps = {
      "cmake",
      "ninja",
      "pkgconf",
      "gcc",
      "qt6-base",
      "qt6-declarative",
      "wayland",
      "wayland-protocols",
      "libxkbcommon",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "qt6-declarative", "wayland", "libxkbcommon" },
  },
  reproducibility = {
    status = "verified",
  },
}
