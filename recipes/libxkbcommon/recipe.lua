local version = "1.7.0"

return {
  name = "libxkbcommon",
  version = version,
  release = 1,
  summary = "Keymap handling library",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://xkbcommon.org/download/libxkbcommon-" .. version .. ".tar.xz",
    sha256 = "65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247",
  },
  build = {
    system = "meson",
    deps = {
      "meson",
      "ninja",
      "pkgconf",
      "gcc",
      "libxcb",
      "xkeyboard-config",
      "libxml2",
      "bison",
      "wayland",
      "wayland-protocols",
    },
    script = [[
#!/bin/sh
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Ddefault_library=shared -Denable-docs=false -Denable-x11=true -Denable-wayland=true -Denable-xkbregistry=true
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = { "glibc", "libxcb", "xkeyboard-config", "libxml2", "wayland" },
  },
  reproducibility = {
    status = "verified",
  },
}
