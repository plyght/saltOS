local version = "2.1.0"

return {
  name = "qterminal",
  version = version,
  release = 2,
  summary = "Lightweight Qt terminal emulator",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/qterminal/releases/download/" .. version .. "/qterminal-" .. version .. ".tar.xz",
    sha256 = "a65e788645bc694ede5d89de4118ee88443e0d6cbc388b0ce50d5c5d07b1213c",
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
      "qtermwidget",
      "layer-shell-qt",
      "libx11",
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
    deps = { "glibc", "qt6-base", "qtermwidget", "layer-shell-qt", "libx11" },
  },
  reproducibility = {
    status = "verified",
  },
}
