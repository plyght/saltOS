local version = "2.1.0"

return {
  name = "lxqt-globalkeys",
  version = version,
  release = 1,
  summary = "LXQt global keyboard shortcut daemon",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/lxqt-globalkeys/releases/download/" .. version .. "/lxqt-globalkeys-" .. version .. ".tar.xz",
    sha256 = "70cc56c452626a2c3ceb7ade8745ed61bac10c7d9aa082443a74aba1e3942874",
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
      "liblxqt",
      "kwindowsystem",
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
    deps = { "glibc", "qt6-base", "liblxqt", "kwindowsystem" },
  },
  reproducibility = {
    status = "verified",
  },
}
