local version = "6.2.3"

return {
  name = "layer-shell-qt",
  version = version,
  release = 1,
  summary = "Qt component for the wlr-layer-shell protocol",
  license = "LGPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.kde.org/stable/plasma/" .. version .. "/layer-shell-qt-" .. version .. ".tar.xz",
    sha256 = "00941fedfc5420f65d6be24704b5cc2f07f5971f5b7c00145668cedc73651e1c",
  },
  build = {
    system = "cmake",
    deps = {
      "cmake",
      "ninja",
      "pkgconf",
      "gcc",
      "qt6-base",
      "extra-cmake-modules",
      "qt6-wayland",
      "qt6-declarative",
      "wayland",
      "wayland-protocols",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DBUILD_TESTING=OFF
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "qt6-wayland", "qt6-declarative", "wayland" },
  },
  reproducibility = {
    status = "verified",
  },
}
