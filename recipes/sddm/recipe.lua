local version = "0.21.0"

return {
  name = "sddm",
  version = version,
  release = 3,
  summary = "QML-based X11 display manager",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/sddm/sddm/archive/refs/tags/v" .. version .. ".tar.gz",
    sha256 = "f895de2683627e969e4849dbfbbb2b500787481ca5ba0de6d6dfdae5f1549abf",
  },
  build = {
    system = "cmake",
    deps = {
      "cmake",
      "ninja",
      "pkgconf",
      "gcc",
      "extra-cmake-modules",
      "qt6-base",
      "qt6-declarative",
      "qt6-tools",
      "libxcb",
      "libxau",
      "libxkbcommon",
      "libxcrypt",
      "shadow",
      "dbus",
      "linux-pam",
    },
    script = [[
#!/bin/sh
cmake -G Ninja -S "$SALT_SRC" -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_SYSCONFDIR=/etc -DBUILD_WITH_QT6=ON -DINSTALL_PAM_CONFIGURATION=OFF -DNO_SYSTEMD=ON -DENABLE_JOURNALD=OFF -DBUILD_MAN_PAGES=OFF -DRUNTIME_DIR=/run/sddm -DSTATE_DIR=/var/lib/sddm -DDBUS_CONFIG_DIR=/usr/share/dbus-1/system.d -DHALT_COMMAND=/sbin/poweroff -DREBOOT_COMMAND=/sbin/reboot -DUID_MIN=1000 -DUID_MAX=60000
cmake --build build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" cmake --install build
# Upstream PAM files include distro stacks (system-login); ship ours on system-auth.
install -d "$SALT_DEST/etc/pam.d"
install -m644 "$SALT_FILES/sddm" "$SALT_FILES/sddm-autologin" "$SALT_FILES/sddm-greeter" "$SALT_DEST/etc/pam.d/"
]],
  },
  package = {
    deps = {
      "glibc",
      "qt6-base",
      "qt6-declarative",
      "libxcb",
      "libxau",
      "libxkbcommon",
      "libxcrypt",
      "xorg-server",
      "xauth",
      "dbus",
      "shadow",
      "linux-pam",
    },
  },
  hooks = {
    post_install = [[
getent group sddm >/dev/null || groupadd -r sddm
getent passwd sddm >/dev/null || useradd -r -g sddm -d /var/lib/sddm -s /sbin/nologin -c "SDDM greeter" sddm
mkdir -p /var/lib/sddm
chown sddm:sddm /var/lib/sddm
]],
  },
  reproducibility = {
    status = "verified",
  },
}
