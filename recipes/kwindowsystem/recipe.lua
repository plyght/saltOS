local version = "6.7.0"

return {
  name = "kwindowsystem",
  version = version,
  release = 1,
  summary = "KDE window system access (KF6)",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.kde.org/stable/frameworks/6.7/kwindowsystem-" .. version .. ".tar.xz",
    sha256 = "62c0f0b4a9507939d84aeeda55bbd4300b88c04e37953e5189b139003310a8f4",
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
      "qt6-tools",
      "libx11",
      "libxfixes",
      "xcb-util-keysyms",
      "xcb-util-wm",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DBUILD_TESTING=OFF -DKWINDOWSYSTEM_WAYLAND=OFF -DBUILD_PYTHON_BINDINGS=OFF
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = { "glibc", "qt6-base", "libx11", "libxfixes", "libxcb" },
  },
  reproducibility = {
    status = "verified",
  },
}
