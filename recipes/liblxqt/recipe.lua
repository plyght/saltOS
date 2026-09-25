local version = "2.1.0"

return {
  name = "liblxqt",
  version = version,
  release = 2,
  summary = "Core utility library for LXQt",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/liblxqt/releases/download/" .. version .. "/liblxqt-" .. version .. ".tar.xz",
    sha256 = "10820b62f83c5f53439b8690c9d71deaad7aa31e6506f9ec53cb789d47b13ce0",
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
      "kwindowsystem",
      "lxqt-build-tools",
      "libx11",
      "libxscrnsaver",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DBUILD_BACKLIGHT_LINUX_BACKEND=OFF
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "libqtxdg", "kwindowsystem", "libx11", "libxscrnsaver" },
  },
  reproducibility = {
    status = "verified",
  },
}
