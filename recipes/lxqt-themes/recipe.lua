local version = "2.1.0"

return {
  name = "lxqt-themes",
  version = version,
  release = 1,
  summary = "LXQt themes, graphics and icons",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/lxqt-themes/releases/download/" .. version .. "/lxqt-themes-" .. version .. ".tar.xz",
    sha256 = "cdd0101c5a53a0e49315c7af3ba784e70a8a2410526eab3d63f32a6678bd0fac",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "lxqt-build-tools" },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
