local version = "2.1.0"

return {
  name = "lxqt-panel",
  version = version,
  release = 2,
  summary = "LXQt desktop panel",
  license = "LGPL-2.1-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/lxqt/lxqt-panel/releases/download/" .. version .. "/lxqt-panel-" .. version .. ".tar.xz",
    sha256 = "ab64e6083c389e0f659a7b4caf80b579b03c543d75c4f7531777bdbf1aea75e6",
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
      "qt6-wayland",
      "liblxqt",
      "kwindowsystem",
      "layer-shell-qt",
      "lxqt-globalkeys",
      "lxqt-menu-data",
      "libdbusmenu-lxqt",
      "libxkbcommon",
      "libxtst",
      "alsa-lib",
      "libx11",
      "libxcb",
      "lxqt-build-tools",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DCPULOAD_PLUGIN=No -DNETWORKMONITOR_PLUGIN=No -DSYSSTAT_PLUGIN=No -DMOUNT_PLUGIN=No -DSENSORS_PLUGIN=No -DBACKLIGHT_PLUGIN=No -DVOLUME_PLUGIN=Yes -DVOLUME_USE_ALSA=Yes -DVOLUME_USE_PULSEAUDIO=No
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
]],
  },
  package = {
    deps = {
      "glibc",
      "qt6-base",
      "qt6-wayland",
      "liblxqt",
      "kwindowsystem",
      "layer-shell-qt",
      "lxqt-globalkeys",
      "lxqt-menu-data",
      "libdbusmenu-lxqt",
      "libxkbcommon",
      "libxtst",
      "alsa-lib",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
