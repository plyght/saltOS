local version = "2.1.0"

return {
  name = "lxqt-build-tools",
  version = version,
  release = 2,
  summary = "LXQt build tools (CMake modules)",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/lxqt-build-tools/releases/download/" .. version .. "/lxqt-build-tools-" .. version .. ".tar.xz",
    sha256 = "2458b629936f5e1ff8850e9953e49d66b96ac419cb484fed0a4d28a711fd8ef9",
  },
  build = {
    system = "cmake",
    deps = { "cmake", "ninja", "pkgconf", "gcc", "qt6-base", "glib" },
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
